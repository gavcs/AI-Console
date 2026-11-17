#!/usr/bin/env python3
"""AI Console agent runtime: the model <-> MCP tool loop.

vllm-mlx only *emits* tool calls; this module executes them. It is used three ways:

  * By the Qt app's chat page (JSON lines over stdin/stdout):
        python3 agent.py chat --config session.json
  * By your own scripts (synchronous facade):
        from agent import Agent
        with Agent() as agent:                 # reads $AI_CONSOLE_CONFIG
            answer = agent.ask("Summarise the main function.")
  * To list the tools a config exposes:
        python3 agent.py tools --config session.json

Config file (JSON), written by the Qt app:
  {
    "base_url": "http://127.0.0.1:8000/v1",
    "model": "mlx-community/Qwen3.5-9B-MLX-4bit",
    "system": "optional system prompt",
    "history": "/path/to/messages.json",     (optional: the conversation is saved there and
                                               continued from there; see history.py)
    "label": "Qwen3.5-9B-MLX-4bit",          (optional: the name saved with each answer)
    "api_key": "...",                        (optional: sent as a Bearer token to the model server)
    "mcp_servers": [
      {"name": "ghidra", "transport": "stdio", "command": "python3",
       "args": ["ghidra_bridge.py", "--bridge-dir", "/path/to/ghidra-mcp",
                "--url", "http://127.0.0.1:18089", "--tcp-only"], "cwd": "/path"},
      {"name": "remote-thing", "transport": "http", "url": "http://host:8081/mcp",
       "headers": {"Authorization": "Bearer ..."}}
    ]
  }
`transport` is "stdio", "http" (streamable HTTP) or "sse".

Only needs `httpx` and `mcp` (httpx is a dependency of mcp).

Chat protocol (chat mode):
  stdin  -> {"type":"user","content":"...","attachments":[ ... ]} | {"type":"reset"}
            {"type":"stop"}   stop the reply being written (what was written so far is kept)
            "attachments" is optional; each entry is a file the user attached:
              {"kind":"image",  "name","mime","path","data_url":"data:...;base64,..."}
              {"kind":"text",   "name","mime","path","text":"<contents>","truncated":bool}
              {"kind":"binary", "name","mime","path","size":<bytes>}
            An image becomes an OpenAI image_url part (needs a vision model); a
            text file is inlined; a binary file is referenced by path so a tool
            can open it. With no image present the whole message is sent as one
            string, which every model accepts.
  stdout <- {"type":"ready","model":...,"tools":[...]}
            {"type":"delta","content":"..."}           streamed answer text
            {"type":"thinking","content":"..."}        reasoning text (see "Thinking" below)
            {"type":"answer_was_thinking"}             the answer text of this reply so far was reasoning
            {"type":"thinking_was_answer"}             the reasoning text of this reply so far was the answer
            {"type":"tool_call","id":...,"name":...,"arguments":{...}}
            {"type":"tool_result","id":...,"name":...,"content":"..."}
            {"type":"error","message":"..."}
            {"type":"stopped"}                         the reply was stopped on request ("done" follows)
            {"type":"done","content":"<full answer>"}  end of turn
  stderr <- human-readable log lines

Thinking: reasoning reaches us in one of three shapes, all turned into
"thinking" events and kept out of the answer and the conversation history:
  1. the server separates it (`reasoning_content` in the stream);
  2. inline tags:  <think>reasoning</think>answer;
  3. only the closing tag:  reasoning</think>answer  - the model's chat template
     already put <think> in the prompt, so nothing marks where reasoning starts.
     The first such reply is streamed as answer text and then corrected with
     "answer_was_thinking"; that the model behaves this way is remembered
     (model_traits.json next to the config), so later replies start as thinking.
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import os
import re
import sys
import threading
import uuid
from dataclasses import dataclass, field
from typing import Any, Awaitable, Callable

import httpx

import history as saved
from mcp import ClientSession, StdioServerParameters
from mcp.client.sse import sse_client
from mcp.client.stdio import stdio_client
from mcp.client.streamable_http import streamablehttp_client

Event = dict[str, Any]
Emit = Callable[[Event], Awaitable[None]]

MAX_TOOL_ROUNDS = 25          # tool-call rounds per user turn before giving up
MAX_TOOL_RESULT_CHARS = 30000 # cap what goes back into the model's context per tool result


# --------------------------------------------------------------------------- config

@dataclass
class MCPServerConfig:
    name: str
    transport: str = "stdio"                    # "stdio" | "http"
    command: str = ""
    args: list[str] = field(default_factory=list)
    env: dict[str, str] = field(default_factory=dict)
    cwd: str | None = None
    url: str = ""
    headers: dict[str, str] = field(default_factory=dict)   # http / sse only
    tools: list[str] | None = None              # optional allow-list of tool names

    @classmethod
    def from_json(cls, raw: dict[str, Any]) -> "MCPServerConfig":
        known = {f for f in cls.__dataclass_fields__}
        return cls(**{k: v for k, v in raw.items() if k in known})


@dataclass
class Config:
    base_url: str = "http://127.0.0.1:8000/v1"
    model: str = ""
    system: str = ""
    mcp_servers: list[MCPServerConfig] = field(default_factory=list)
    traits_path: str = ""                       # where per-model observations are remembered
    history: str = ""                           # file the conversation is saved in ("" = not saved)
    api_key: str = ""                           # for a model server that wants one
    label: str = ""                             # who answers, as saved with each reply

    @classmethod
    def load(cls, path: str | None = None) -> "Config":
        """Load from `path`, else $AI_CONSOLE_CONFIG, else plain env vars (no tools)."""
        path = path or os.environ.get("AI_CONSOLE_CONFIG")
        if not path:
            return cls(base_url=os.environ.get("OPENAI_BASE_URL", cls.base_url),
                       model=os.environ.get("MODEL", ""))  # no key: the env var is a placeholder for other clients
        with open(path, encoding="utf-8") as f:
            raw = json.load(f)
        return cls(
            base_url=raw.get("base_url", cls.base_url).rstrip("/"),
            model=raw.get("model", ""),
            system=raw.get("system", ""),
            mcp_servers=[MCPServerConfig.from_json(s) for s in raw.get("mcp_servers", [])],
            traits_path=os.path.join(os.path.dirname(os.path.abspath(path)), "model_traits.json"),
            history=raw.get("history", ""),
            api_key=raw.get("api_key", ""),
            label=raw.get("label", ""),
        )

    # What we have learned about a model by watching its output (see "Thinking").
    def trait(self, name: str, default: Any = None) -> Any:
        try:
            with open(self.traits_path, encoding="utf-8") as f:
                return json.load(f).get(self.model, {}).get(name, default)
        except (OSError, ValueError):
            return default

    def set_trait(self, name: str, value: Any) -> None:
        if not self.traits_path:
            return
        try:
            with open(self.traits_path, encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            data = {}
        data.setdefault(self.model, {})[name] = value
        try:
            with open(self.traits_path, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
        except OSError:
            pass


# --------------------------------------------------------------------------- MCP tools

def _safe_name(s: str) -> str:
    # OpenAI-style function names: [A-Za-z0-9_-], max 64 chars.
    return re.sub(r"[^A-Za-z0-9_-]", "_", s)[:64]


class ToolRegistry:
    """Maps model-facing tool names to (MCP session, MCP tool name)."""

    def __init__(self) -> None:
        self.specs: list[dict[str, Any]] = []
        self.routes: dict[str, tuple[ClientSession, str]] = {}
        self.servers: list[tuple[MCPServerConfig, ClientSession]] = []

    @staticmethod
    async def _list(cfg: MCPServerConfig, session: ClientSession):
        """One server's tools as (specs, routes)."""
        specs: list[dict[str, Any]] = []
        routes: dict[str, tuple[ClientSession, str]] = {}
        result = await session.list_tools()
        for tool in result.tools:
            if cfg.tools and tool.name not in cfg.tools:
                continue
            name = _safe_name(f"{cfg.name}__{tool.name}")
            routes[name] = (session, tool.name)
            specs.append({
                "type": "function",
                "function": {
                    "name": name,
                    "description": tool.description or "",
                    "parameters": tool.inputSchema or {"type": "object", "properties": {}},
                },
            })
        return specs, routes

    async def add_server(self, cfg: MCPServerConfig, session: ClientSession) -> int:
        specs, routes = await self._list(cfg, session)
        self.servers.append((cfg, session))
        self.specs.extend(specs)
        self.routes.update(routes)
        return len(specs)

    async def refresh(self) -> tuple[list[str], list[str]]:
        """Re-list every server's tools; returns (added, removed) names.

        A server may change its tool set while running. The Ghidra bridge does:
        it starts with a few tool groups and adds more when the model calls
        load_tool_group, so without this the model could load a group and then
        be unable to call anything in it.
        """
        specs: list[dict[str, Any]] = []
        routes: dict[str, tuple[ClientSession, str]] = {}
        for cfg, session in self.servers:
            s, r = await self._list(cfg, session)
            specs.extend(s)
            routes.update(r)
        before, after = set(self.routes), set(routes)
        self.specs, self.routes = specs, routes
        return sorted(after - before), sorted(before - after)

    @property
    def names(self) -> list[str]:
        return [s["function"]["name"] for s in self.specs]

    async def call(self, name: str, args: dict[str, Any]) -> str:
        if name not in self.routes:
            return f"ERROR: unknown tool {name!r}"
        session, mcp_name = self.routes[name]
        result = await session.call_tool(mcp_name, args)
        parts = [c.text for c in result.content if getattr(c, "text", None)]
        text = "\n".join(parts)
        if not text and getattr(result, "structuredContent", None):
            text = json.dumps(result.structuredContent)
        if result.isError:
            text = "ERROR: " + text
        if len(text) > MAX_TOOL_RESULT_CHARS:
            text = text[:MAX_TOOL_RESULT_CHARS] + f"\n...[truncated {len(text) - MAX_TOOL_RESULT_CHARS} chars]"
        return text


@contextlib.asynccontextmanager
async def open_servers(configs: list[MCPServerConfig], log: Callable[[str], None]):
    """Connect to every configured MCP server; a server that fails is logged and skipped."""
    async with contextlib.AsyncExitStack() as stack:
        registry = ToolRegistry()
        for cfg in configs:
            try:
                if cfg.transport == "http":
                    read, write, _ = await stack.enter_async_context(
                        streamablehttp_client(cfg.url, headers=cfg.headers or None))
                elif cfg.transport == "sse":
                    read, write = await stack.enter_async_context(
                        sse_client(cfg.url, headers=cfg.headers or None))
                else:
                    params = StdioServerParameters(command=cfg.command, args=cfg.args,
                                                   env={**os.environ, **cfg.env}, cwd=cfg.cwd)
                    read, write = await stack.enter_async_context(stdio_client(params, errlog=sys.stderr))
                session = await stack.enter_async_context(ClientSession(read, write))
                await session.initialize()
                n = await registry.add_server(cfg, session)
                log(f"[mcp] {cfg.name}: connected, {n} tools")
            except Exception as e:  # noqa: BLE001 - keep going with the other servers
                log(f"[mcp] {cfg.name}: FAILED to connect: {e}")
        yield registry


# --------------------------------------------------------------------------- model calls

def _partial_tag(text: str, tags: tuple[str, ...]) -> int:
    """Length of the longest tail of `text` that could be the start of one of `tags`."""
    for n in range(min(len(text), max(len(t) for t in tags) - 1), 0, -1):
        if any(t.startswith(text[-n:]) for t in tags):
            return n
    return 0


class ThinkSplitter:
    """Sorts streamed text into reasoning and answer on <think> / </think>.

    feed() returns a list of (kind, text) with kind "thinking" or "content",
    plus ("answer_was_thinking", "") when a </think> turns up with no <think>
    before it: everything returned as "content" so far was reasoning.
    A tag split across two chunks is held back until it is complete.
    """
    OPEN, CLOSE = "<think>", "</think>"

    def __init__(self, start_thinking: bool = False) -> None:
        self.thinking = start_thinking      # currently inside reasoning
        self.assumed = start_thinking       # ...only because we assumed so, no tag seen yet
        self.closed_unopened = False        # saw </think> without <think>
        self.thinking_text = ""
        self.content = ""
        self._buf = ""
        self._strip = True                  # drop whitespace at the start of the next part

    def _emit(self, out: list[tuple[str, str]], kind: str, text: str) -> None:
        if self._strip:
            text = text.lstrip()
        if not text:
            return
        self._strip = False
        if kind == "thinking":
            self.thinking_text += text
        else:
            self.content += text
        out.append((kind, text))

    def feed(self, text: str) -> list[tuple[str, str]]:
        self._buf += text
        out: list[tuple[str, str]] = []
        while self._buf:
            if self.thinking:
                if self.assumed and not self.thinking_text:
                    # Assumed reasoning, but the model wrote the opening tag itself: drop it.
                    head = self._buf.lstrip()
                    if head.startswith(self.OPEN):
                        self._buf = head[len(self.OPEN):]
                        self.assumed = False
                        continue
                    if head and self.OPEN.startswith(head):
                        break                       # possibly a partial tag: wait for more
                i = self._buf.find(self.CLOSE)
                if i < 0:
                    keep = _partial_tag(self._buf, (self.CLOSE,))
                    self._emit(out, "thinking", self._buf[:len(self._buf) - keep])
                    self._buf = self._buf[len(self._buf) - keep:]
                    break
                self._emit(out, "thinking", self._buf[:i])
                self._buf = self._buf[i + len(self.CLOSE):]
                self.thinking = self.assumed = False
                self._strip = True
                continue
            i, j = self._buf.find(self.OPEN), self._buf.find(self.CLOSE)
            if j >= 0 and (i < 0 or j < i):
                self._emit(out, "content", self._buf[:j])
                self._buf = self._buf[j + len(self.CLOSE):]
                self.thinking_text += self.content
                self.content = ""
                self.closed_unopened = True
                self._strip = True
                out.append(("answer_was_thinking", ""))
            elif i >= 0:
                self._emit(out, "content", self._buf[:i])
                self._buf = self._buf[i + len(self.OPEN):]
                self.thinking = True
                self._strip = True
            else:
                keep = _partial_tag(self._buf, (self.OPEN, self.CLOSE))
                self._emit(out, "content", self._buf[:len(self._buf) - keep])
                self._buf = self._buf[len(self._buf) - keep:]
                break
        return out

    def flush(self) -> list[tuple[str, str]]:
        """End of the reply: whatever was held back is ordinary text after all."""
        out: list[tuple[str, str]] = []
        self._emit(out, "thinking" if self.thinking else "content", self._buf)
        self._buf = ""
        return out

    def server_separates(self) -> None:
        """The server sends reasoning in its own field, so nothing here is assumed."""
        if self.assumed and not self.thinking_text:
            self.thinking = self.assumed = False


async def stream_completion(client: httpx.AsyncClient, cfg: Config, messages: list[dict[str, Any]],
                            tools: list[dict[str, Any]], emit: Emit, start_thinking: bool = False,
                            split: "ThinkSplitter | None" = None):
    """One streamed chat completion. Returns (splitter, tool_calls, finish_reason).

    `splitter.content` is the answer with reasoning removed. `start_thinking`
    says this model is known to begin its replies with unmarked reasoning.
    A caller that wants to see the reply while it is still coming in (to keep
    what was written if the turn is stopped) passes its own `split`.
    """
    body: dict[str, Any] = {"model": cfg.model, "messages": saved.for_model(messages), "stream": True}
    if tools:
        body["tools"] = tools
        body["tool_choice"] = "auto"

    split = split or ThinkSplitter(start_thinking)
    calls: dict[int, dict[str, str]] = {}      # index -> {id, name, arguments}
    finish: str | None = None

    async def emit_parts(parts: list[tuple[str, str]]) -> None:
        for kind, text in parts:
            if kind == "content":
                await emit({"type": "delta", "content": text})
            elif kind == "thinking":
                await emit({"type": "thinking", "content": text})
            else:
                await emit({"type": kind})

    async with client.stream("POST", f"{cfg.base_url}/chat/completions", json=body) as resp:
        if resp.status_code != 200:
            detail = (await resp.aread()).decode("utf-8", errors="replace")
            raise RuntimeError(f"HTTP {resp.status_code} from model server: {detail[:500]}")
        async for line in resp.aiter_lines():
            if not line.startswith("data:"):
                continue
            data = line[5:].strip()
            if data == "[DONE]":
                break
            chunk = json.loads(data)
            choices = chunk.get("choices") or []
            if not choices:
                continue
            choice = choices[0]
            delta = choice.get("delta") or {}
            reasoning = delta.get("reasoning_content") or delta.get("reasoning")
            if reasoning:
                split.server_separates()
                split.thinking_text += reasoning
                await emit({"type": "thinking", "content": reasoning})
            if delta.get("content"):
                await emit_parts(split.feed(delta["content"]))
            for tc in delta.get("tool_calls") or []:
                slot = calls.setdefault(tc.get("index", 0), {"id": "", "name": "", "arguments": ""})
                if tc.get("id"):
                    slot["id"] = tc["id"]
                fn = tc.get("function") or {}
                if fn.get("name") and not slot["name"]:
                    slot["name"] = fn["name"]
                if fn.get("arguments"):
                    slot["arguments"] += fn["arguments"]
            if choice.get("finish_reason"):
                finish = choice["finish_reason"]

    await emit_parts(split.flush())
    ordered = [calls[i] for i in sorted(calls)]
    for c in ordered:
        c["id"] = c["id"] or f"call_{uuid.uuid4().hex[:8]}"
    return split, ordered, finish


class AsyncAgent:
    """Holds one conversation and runs the tool loop for each user turn."""

    def __init__(self, cfg: Config, registry: ToolRegistry, emit: Emit) -> None:
        self.cfg = cfg
        self.registry = registry
        self.emit = emit
        self.http = httpx.AsyncClient(
            timeout=httpx.Timeout(15.0, read=None),
            headers={"Authorization": f"Bearer {cfg.api_key}"} if cfg.api_key else None)
        self._split: ThinkSplitter | None = None    # the reply being streamed right now
        self.messages: list[dict[str, Any]] = []
        # The saved conversation (see history.py). Loaded once: a session that
        # starts with a history file continues that conversation.
        self.history = saved.History(cfg.history)
        self.label = cfg.label or cfg.model.rsplit("/", 1)[-1]
        # True once this model has been seen to start replies with unmarked reasoning.
        self.think_in_prompt: bool = bool(cfg.trait("think_in_prompt", False))
        self._load()

    def _load(self) -> None:
        system = [{"role": "system", "content": self.cfg.system}] if self.cfg.system else []
        self.messages = system + list(self.history.messages)

    def reset(self) -> None:
        self.history.clear()
        self._load()

    def _add(self, msg: dict[str, Any]) -> None:
        """Add a message to the conversation and save it."""
        self.messages.append(msg)
        self.history.append(msg)

    def stopped(self) -> None:
        """The user stopped the turn: keep what was written and leave the conversation valid."""
        split, self._split = self._split, None
        for missing in saved.close_tool_calls(self.messages, saved.STOPPED):
            self._add(missing)
        extras: dict[str, Any] = {"_model": self.label}
        if split is not None and split.thinking_text:
            extras["_thinking"] = split.thinking_text
        self._add(saved.stopped_message(split.content if split is not None else "", extras))

    def note_failure(self, why: str) -> None:
        """The turn ended without an answer; keep the conversation valid to continue."""
        if self.messages and self.messages[-1].get("role") in ("user", "tool"):
            self._add({"role": "assistant", "content": f"[No reply: {why}]", "_error": why})

    _attachment_text = staticmethod(saved.attachment_text)
    _user_content = staticmethod(saved.openai_user_content)

    async def ask(self, text: str, attachments: list[dict[str, Any]] | None = None) -> str:
        self._add(saved.user_message(text, attachments))
        final = ""
        for _ in range(MAX_TOOL_ROUNDS):
            self._split = ThinkSplitter(self.think_in_prompt)
            split, calls, _finish = await stream_completion(
                self.http, self.cfg, self.messages, self.registry.specs, self.emit, split=self._split)
            self._split = None
            content = split.content
            if split.closed_unopened and not self.think_in_prompt:
                self.think_in_prompt = True                 # learned: shape 3 (see module docstring)
                self.cfg.set_trait("think_in_prompt", True)
            elif split.assumed and split.thinking_text and not calls:
                # We assumed the reply opened with reasoning, but no </think> ever
                # came: all of it was the answer. Correct the display and unlearn.
                await self.emit({"type": "thinking_was_answer"})
                content = split.thinking_text
                self.think_in_prompt = False
                self.cfg.set_trait("think_in_prompt", False)
            thought = {"_thinking": split.thinking_text} if split.thinking_text and content is split.content else {}
            if not calls:
                self._add({"role": "assistant", "content": content, "_model": self.label, **thought})
                final = content
                break

            self._add({
                "role": "assistant",
                "content": content or None,
                "_model": self.label, **thought,
                "tool_calls": [{"id": c["id"], "type": "function",
                                "function": {"name": c["name"], "arguments": c["arguments"] or "{}"}}
                               for c in calls],
            })
            for c in calls:
                try:
                    args = json.loads(c["arguments"] or "{}")
                    if not isinstance(args, dict):
                        raise ValueError("arguments must be a JSON object")
                except ValueError as e:
                    await self.emit({"type": "tool_call", "id": c["id"], "name": c["name"],
                                     "arguments": c["arguments"]})
                    result = f"ERROR: tool arguments were not valid JSON: {e}"
                else:
                    await self.emit({"type": "tool_call", "id": c["id"], "name": c["name"], "arguments": args})
                    try:
                        result = await self.registry.call(c["name"], args)
                    except Exception as e:  # noqa: BLE001 - report to the model, don't crash the turn
                        result = f"ERROR: {type(e).__name__}: {e}"
                await self.emit({"type": "tool_result", "id": c["id"], "name": c["name"], "content": result})
                self._add({"role": "tool", "tool_call_id": c["id"], "content": result})
            # A tool call may have changed which tools exist (see ToolRegistry.refresh).
            try:
                added, removed = await self.registry.refresh()
            except Exception as e:  # noqa: BLE001 - keep the tools we had
                print(f"[mcp] could not refresh the tool list: {e}", file=sys.stderr, flush=True)
            else:
                if added or removed:
                    print(f"[mcp] tool list changed: +{len(added)} -{len(removed)}, now {len(self.registry.specs)} tools",
                          file=sys.stderr, flush=True)
        else:
            await self.emit({"type": "error", "message": f"Stopped after {MAX_TOOL_ROUNDS} tool rounds."})
            self.note_failure(f"stopped after {MAX_TOOL_ROUNDS} tool rounds")

        await self.emit({"type": "done", "content": final})
        return final


# --------------------------------------------------------------------------- sync facade for scripts

def _print_events(ev: Event) -> None:
    """Default event sink for scripts: answer text to stdout, tool activity to stderr."""
    t = ev["type"]
    if t == "delta":
        sys.stdout.write(ev["content"]); sys.stdout.flush()
    elif t == "tool_call":
        print(f"\n[tool] {ev['name']}({json.dumps(ev['arguments'])[:300]})", file=sys.stderr, flush=True)
    elif t == "tool_result":
        print(f"[tool] -> {ev['content'][:300]!r}", file=sys.stderr, flush=True)
    elif t == "error":
        print(f"[error] {ev['message']}", file=sys.stderr, flush=True)
    elif t == "done":
        sys.stdout.write("\n"); sys.stdout.flush()


class Agent:
    """Synchronous wrapper: owns a background event loop with the MCP connections open.

        with Agent() as agent:
            print(agent.tools)
            agent.ask("...")
    """

    def __init__(self, config: Config | None = None,
                 on_event: Callable[[Event], None] | None = None) -> None:
        self.cfg = config or Config.load()
        self.on_event = on_event or _print_events
        self._loop = asyncio.new_event_loop()
        self._ready = threading.Event()
        self._error: BaseException | None = None
        self._agent: AsyncAgent | None = None
        self._stop: asyncio.Event | None = None
        self._thread = threading.Thread(target=self._run, name="agent-loop", daemon=True)
        self._thread.start()
        self._ready.wait()
        if self._error:
            raise RuntimeError(f"agent failed to start: {self._error}") from self._error

    def _run(self) -> None:
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._serve())

    async def _serve(self) -> None:
        self._stop = asyncio.Event()
        try:
            async with open_servers(self.cfg.mcp_servers, lambda m: print(m, file=sys.stderr, flush=True)) as reg:
                self._agent = AsyncAgent(self.cfg, reg, self._emit)
                self._ready.set()
                await self._stop.wait()
        except BaseException as e:  # noqa: BLE001 - surfaced to the constructor
            self._error = e
            self._ready.set()

    async def _emit(self, ev: Event) -> None:
        self.on_event(ev)

    @property
    def tools(self) -> list[str]:
        assert self._agent is not None
        return self._agent.registry.names

    def ask(self, text: str, attachments: list[dict[str, Any]] | None = None) -> str:
        assert self._agent is not None
        return asyncio.run_coroutine_threadsafe(self._agent.ask(text, attachments), self._loop).result()

    def reset(self) -> None:
        assert self._agent is not None
        self._loop.call_soon_threadsafe(self._agent.reset)

    def close(self) -> None:
        if self._stop is not None:
            self._loop.call_soon_threadsafe(self._stop.set)
        self._thread.join(timeout=10)

    def __enter__(self) -> "Agent":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


# --------------------------------------------------------------------------- CLI

def _log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


async def _chat_main(cfg: Config) -> None:
    async def emit(ev: Event) -> None:
        sys.stdout.write(json.dumps(ev) + "\n")
        sys.stdout.flush()

    async with open_servers(cfg.mcp_servers, _log) as registry:
        agent = AsyncAgent(cfg, registry, emit)
        await emit({"type": "ready", "model": cfg.model, "tools": registry.names})
        closing = False

        async def run_turn(msg: dict[str, Any]) -> None:
            try:
                await agent.ask(msg.get("content", ""), msg.get("attachments") or None)
            except asyncio.CancelledError:
                if closing:                   # the app went away: nothing to report to
                    raise
                agent.stopped()
                await emit({"type": "stopped"})
                await emit({"type": "done", "content": ""})
            except Exception as e:  # noqa: BLE001 - keep the session alive
                agent.note_failure(f"{type(e).__name__}: {e}")
                await emit({"type": "error", "message": f"{type(e).__name__}: {e}"})
                await emit({"type": "done", "content": ""})

        # A turn runs as a task of its own, so that "stop" can be read (and can
        # cancel it) while it is in progress.
        loop = asyncio.get_running_loop()
        turn: asyncio.Task[None] | None = None
        while True:
            line = await loop.run_in_executor(None, sys.stdin.readline)
            if not line:                      # stdin closed: the app went away
                break
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                await emit({"type": "error", "message": f"bad JSON on stdin: {line[:80]}"})
                continue
            busy = turn is not None and not turn.done()
            if msg.get("type") == "stop":
                if busy:
                    turn.cancel()
            elif busy:
                await emit({"type": "error", "message": "still answering the previous message"})
            elif msg.get("type") == "reset":
                agent.reset()
            elif msg.get("type") == "user":
                turn = asyncio.create_task(run_turn(msg))
        closing = True
        if turn is not None and not turn.done():
            turn.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await turn


async def _tools_main(cfg: Config) -> None:
    async with open_servers(cfg.mcp_servers, _log) as registry:
        for name in registry.names:
            print(name)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=["chat", "tools"])
    p.add_argument("--config", help="session config JSON (default: $AI_CONSOLE_CONFIG)")
    a = p.parse_args(argv)
    cfg = Config.load(a.config)
    if not cfg.model:
        _log("error: no model configured (config file or $MODEL)")
        return 2
    try:
        asyncio.run(_chat_main(cfg) if a.mode == "chat" else _tools_main(cfg))
    except KeyboardInterrupt:
        pass
    finally:
        sys.stdout.flush()
    # The stdin reader thread may still be blocked in readline(); don't let it hold the exit.
    os._exit(0)


if __name__ == "__main__":
    sys.exit(main())
