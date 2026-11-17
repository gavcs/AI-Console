#!/usr/bin/env python3
"""AI Console chat backend that uses Claude Code instead of a local model.

    python3 claude_agent.py chat --config session.json [--claude /path/to/claude] [--model opus]

Speaks the same JSON-lines chat protocol as agent.py (see its docstring), so
the chat page does not care which of the two it is talking to. The difference
is who runs the model <-> tool loop: here it is one long-lived `claude -p`
process (Claude Code, your existing login, no API key), which connects to the
session's MCP servers itself. This module only translates:

    chat protocol on our stdin  ->  Claude Code stream-json on its stdin
    Claude Code stream-json     ->  chat protocol events on our stdout

What Claude gets to use is exactly the MCP servers ticked for the session.
Claude Code's own tools (shell, file editing, web) are switched off, and no
other MCP configuration of yours is loaded, so a chat here can do what the same
chat with a local model could do. Nobody can answer a permission prompt from
this window, so the session's MCP tools are approved up front and anything else
is refused rather than asked about.

Thinking: Claude reasons before it answers, but by default Claude Code only
reports *that* it is reasoning (a thinking block with no text), where a local
model sends the reasoning itself. Claude Code is therefore started with
--thinking-display summarized, which makes it send a summary of the reasoning;
that is passed on as "thinking" events, as agent.py does. A Claude Code version
without that option is started without it, and then an empty "thinking" event
still marks that reasoning took place, so the chat shows "Thinking..." either way.

Stopping: {"type":"stop"} interrupts the reply being written (Claude Code's own
interrupt request). What was written so far is kept, here and in Claude Code.

Saved conversations (the session config's "history" file, see history.py):
every turn is written to that file in the same form agent.py uses, so a
conversation can move between a local model and Claude. Claude Code also keeps
the conversation itself (its own session store); when the file holds exactly
what Claude Code already has, the session is resumed there, with full fidelity.
Otherwise - the conversation was started or continued with a local model, or
Claude Code's copy is gone - a new Claude Code session is started and given the
earlier conversation as text in front of the next message.

Needs only the standard library.
"""
from __future__ import annotations

import argparse
import asyncio
import collections
import contextlib
import json
import os
import re
import shutil
import sys
import time
import uuid
from pathlib import Path
from typing import Any

import history as saved

Event = dict[str, Any]

MCP_CONNECT_TIMEOUT = 45.0     # seconds to wait for the MCP servers before starting without them
MAX_TOOL_RESULT_CHARS = 30000  # cap on a tool result passed on to the chat page (it shows a preview)

SYSTEM_NOTE = (
    "You are being used through AI Console, a desktop chat window, not a terminal or an IDE. "
    "The only tools you have in this session are the MCP tools listed to you; you cannot run "
    "commands, edit files or browse the web here. Files the user attaches arrive inline in their "
    "message; a binary file is given as a path that a tool may be able to open."
)


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def emit(ev: Event) -> None:
    sys.stdout.write(json.dumps(ev) + "\n")
    sys.stdout.flush()


# --------------------------------------------------------------------------- finding Claude Code

def find_claude() -> str | None:
    """Path of the `claude` CLI, or None. $AI_CONSOLE_CLAUDE overrides the search."""
    override = os.environ.get("AI_CONSOLE_CLAUDE")
    if override:
        return override
    home = Path.home()
    for c in ("claude", str(home / ".local/bin/claude"), str(home / ".claude/local/claude")):
        path = shutil.which(c) or (c if os.path.isfile(c) and os.access(c, os.X_OK) else None)
        if path:
            return path
    # Last resort: the binary bundled with the VS Code extension (version-specific path).
    ext = sorted((home / ".vscode/extensions").glob("anthropic.claude-code-*/resources/native-binary/claude"))
    return str(ext[-1]) if ext else None


# --------------------------------------------------------------------------- session config -> Claude Code

def _safe_name(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9_-]", "_", s)


def mcp_config(servers: list[dict[str, Any]]) -> tuple[dict[str, Any], list[str]]:
    """Claude Code's --mcp-config for the session's servers, and the tools to pre-approve.

    `servers` is the "mcp_servers" list of the session config (see agent.py).
    """
    out: dict[str, Any] = {}
    allowed: list[str] = []
    for s in servers:
        name = _safe_name(s.get("name", "server"))
        transport = s.get("transport", "stdio")
        if transport in ("http", "sse"):
            entry: dict[str, Any] = {"type": transport, "url": s.get("url", "")}
            if s.get("headers"):
                entry["headers"] = s["headers"]
        else:
            command, args = s.get("command", ""), list(s.get("args") or [])
            if s.get("cwd"):
                # Claude Code's config has no working-directory field: change into it first.
                args = ["-c", 'cd "$1" && shift && exec "$@"', "sh", s["cwd"], command, *args]
                command = "/bin/sh"
            entry = {"command": command, "args": args}
            if s.get("env"):
                entry["env"] = s["env"]
        out[name] = entry
        if s.get("tools"):                      # optional allow-list of tool names
            allowed += [f"mcp__{name}__{t}" for t in s["tools"]]
        else:
            allowed.append(f"mcp__{name}")
    return {"mcpServers": out}, allowed


def user_content(text: str, attachments: list[dict[str, Any]] | None) -> list[dict[str, Any]]:
    """The user's message as Anthropic content blocks (images as base64 image blocks)."""
    parts: list[dict[str, Any]] = []
    if text:
        parts.append({"type": "text", "text": text})
    for a in attachments or []:
        m = re.match(r"data:([^;,]+);base64,(.*)", a.get("data_url") or "", re.DOTALL)
        if a.get("kind") == "image" and m:
            parts.append({"type": "image",
                          "source": {"type": "base64", "media_type": m.group(1), "data": m.group(2)}})
        else:
            parts.append({"type": "text", "text": saved.attachment_text(a)})
    return parts or [{"type": "text", "text": ""}]


def _display_tool(name: str) -> str:
    """mcp__files__read_file -> files__read_file, the name a local-model session shows."""
    return name[5:] if name.startswith("mcp__") else name


def _result_text(content: Any) -> str:
    if isinstance(content, list):
        content = "\n".join(c.get("text", "") if isinstance(c, dict) and c.get("type") == "text"
                            else f"[{c.get('type', 'content') if isinstance(c, dict) else 'content'}]"
                            for c in content)
    text = content if isinstance(content, str) else json.dumps(content)
    if len(text) > MAX_TOOL_RESULT_CHARS:
        text = text[:MAX_TOOL_RESULT_CHARS] + f"\n...[truncated {len(text) - MAX_TOOL_RESULT_CHARS} chars]"
    return text


# --------------------------------------------------------------------------- the Claude Code process

class StartFailed(RuntimeError):
    """Claude Code exited before it was ready."""


class ClaudeSession:
    """One `claude -p` process holding one conversation."""

    def __init__(self, claude: str, cfg: dict[str, Any], config_path: str, model: str) -> None:
        self.claude = claude
        self.cfg = cfg
        self.model = model
        self.state_dir = Path(config_path).resolve().parent
        self.proc: asyncio.subprocess.Process | None = None
        self._req = 0
        self.label = cfg.get("label") or ("Claude Code" + (f" ({model})" if model else ""))
        self.history = saved.History(cfg.get("history") or "")
        # Which Claude Code session holds this conversation, and how many of the
        # saved messages it has seen (kept next to the history file).
        self.link_path = (os.path.join(os.path.dirname(self.history.path), "claude.json")
                          if self.history.path else "")
        self.session_id = ""
        self.carry_over = ""                # earlier conversation to put in front of the next message
        self.ask_for_reasoning = True       # pass --thinking-display (see "Thinking" in the module docstring)
        self._in_turn = False
        self._stopping = False              # the user asked to stop the reply being written
        self._stderr: collections.deque[str] = collections.deque(maxlen=20)
        self._stderr_task: asyncio.Task[None] | None = None

    def _link(self) -> dict[str, Any]:
        try:
            with open(self.link_path, encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def _save_link(self) -> None:
        if self.link_path:
            with contextlib.suppress(OSError), open(self.link_path, "w", encoding="utf-8") as f:
                json.dump({"session_id": self.session_id, "synced": len(self.history.messages)}, f)

    async def start(self) -> list[str]:
        """Start Claude Code (continuing the saved conversation, if any). Returns the tool names."""
        link = self._link()
        resume = ""
        if link.get("session_id") and link.get("synced") == len(self.history.messages) and self.history.messages:
            resume = link["session_id"]
        while True:
            try:
                tools = await self._launch(resume)
                break
            except StartFailed:
                await self.stop()                       # also collects the rest of what it printed
                if self.ask_for_reasoning and "thinking-display" in " ".join(self._stderr):
                    self.ask_for_reasoning = False
                    log("[claude] this Claude Code version cannot be asked for its reasoning; "
                        "replies will show that it thought, without the text")
                elif resume:
                    resume = ""
                    log("[claude] Claude Code no longer has this conversation; starting it again from the saved copy")
                else:
                    raise
        if resume:
            log("[claude] resumed the conversation in Claude Code")
        elif self.history.messages:
            self.carry_over = saved.transcript(self.history.messages)
            log(f"[claude] continuing a saved conversation ({len(self.history.messages)} messages) "
                "in a new Claude Code session")
        return tools

    async def _launch(self, resume: str) -> list[str]:
        """Start the process and wait for its MCP servers. Returns the tool names."""
        self.session_id = resume or str(uuid.uuid4())
        self.carry_over = ""
        self._stderr.clear()
        conf, allowed = mcp_config(self.cfg.get("mcp_servers") or [])
        mcp_path = self.state_dir / "claude_mcp.json"
        mcp_path.write_text(json.dumps(conf, indent=2), encoding="utf-8")
        # Claude Code reads CLAUDE.md and project settings from where it runs:
        # give it a folder of its own so it picks up nothing by accident.
        workdir = self.state_dir / "claude_workspace"
        workdir.mkdir(exist_ok=True)

        system = SYSTEM_NOTE + ("\n\n" + self.cfg["system"] if self.cfg.get("system") else "")
        cmd = [self.claude, "-p",
               "--input-format", "stream-json", "--output-format", "stream-json",
               "--verbose", "--include-partial-messages",      # stream text as it is written
               "--mcp-config", str(mcp_path), "--strict-mcp-config",
               "--tools", "",                                  # none of Claude Code's own tools
               "--permission-mode", "dontAsk",                 # refuse, never prompt
               "--append-system-prompt", system]
        if self.ask_for_reasoning:
            cmd += ["--thinking-display", "summarized"]        # send a summary of the reasoning, not just its size
        if allowed:
            cmd += ["--allowedTools", ",".join(allowed)]
        if self.model:
            cmd += ["--model", self.model]
        if not self.history.path:
            cmd += ["--no-session-persistence"]                # nothing to come back to
        elif resume:
            cmd += ["--resume", resume]
        else:
            cmd += ["--session-id", self.session_id]
        self.proc = await asyncio.create_subprocess_exec(
            *cmd, cwd=workdir, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            limit=64 * 1024 * 1024)                            # one JSON line can carry a large tool result
        self._stderr_task = asyncio.create_task(self._forward_stderr(self.proc.stderr))
        return await self._wait_for_servers()

    async def _forward_stderr(self, stream: asyncio.StreamReader | None) -> None:
        """Claude Code's stderr goes to ours (the log pane); the last lines are kept to explain a failed start."""
        while stream is not None:
            try:
                line = await stream.readline()
            except ValueError:                                 # a line longer than the limit
                continue
            if not line:
                return
            text = line.decode("utf-8", errors="replace").rstrip()
            self._stderr.append(text)
            log(text)

    async def _send(self, obj: dict[str, Any]) -> None:
        assert self.proc and self.proc.stdin
        self.proc.stdin.write((json.dumps(obj) + "\n").encode())
        await self.proc.stdin.drain()

    async def _read(self) -> dict[str, Any] | None:
        """Next JSON message from Claude Code; None when it has exited."""
        assert self.proc and self.proc.stdout
        while True:
            line = await self.proc.stdout.readline()
            if not line:
                return None
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                log(line.decode("utf-8", errors="replace"))    # not protocol: show it
                continue
            if isinstance(msg, dict):
                return msg

    def _request(self, subtype: str) -> tuple[str, dict[str, Any]]:
        self._req += 1
        rid = f"console-{self._req}"
        return rid, {"type": "control_request", "request_id": rid, "request": {"subtype": subtype}}

    async def _control(self, subtype: str) -> dict[str, Any] | None:
        """Ask Claude Code something outside the conversation. None if it exited."""
        rid, request = self._request(subtype)
        try:
            await self._send(request)
        except (BrokenPipeError, ConnectionResetError):
            return None
        while True:
            msg = await self._read()
            if msg is None:
                return None
            resp = msg.get("response") or {}
            if msg.get("type") == "control_response" and resp.get("request_id") == rid:
                return resp.get("response") or {}

    async def _wait_for_servers(self) -> list[str]:
        # Claude Code says nothing until the first message, so ask it how its
        # MCP connections are doing until none is still connecting.
        deadline = time.monotonic() + MCP_CONNECT_TIMEOUT
        servers: list[dict[str, Any]] = []
        while True:
            status = await self._control("mcp_status")
            if status is None:
                raise StartFailed("Claude Code exited while starting (see the log)")
            servers = status.get("mcpServers") or []
            if not any(s.get("status") == "pending" for s in servers):
                break
            if time.monotonic() > deadline:
                log("[mcp] still connecting after %d s; starting anyway" % MCP_CONNECT_TIMEOUT)
                break
            await asyncio.sleep(0.3)
        tools: list[str] = []
        for s in servers:
            names = [t.get("name", "") for t in s.get("tools") or []]
            if s.get("status") == "connected":
                log(f"[mcp] {s.get('name')}: connected, {len(names)} tools")
                tools += [f"{s.get('name')}__{n}" for n in names]
            else:
                log(f"[mcp] {s.get('name')}: FAILED to connect ({s.get('status')}"
                    + (f": {s['error']}" if s.get("error") else "") + ")")
        return tools

    async def interrupt(self) -> None:
        """Stop the reply being written. The turn then ends through ask() as usual."""
        if not self._in_turn:
            return
        self._stopping = True
        await self._send(self._request("interrupt")[1])

    async def ask(self, text: str, attachments: list[dict[str, Any]] | None) -> None:
        """Run one user turn, emitting chat protocol events up to and including "done"."""
        self._in_turn, self._stopping = True, False
        try:
            await self._turn(text, attachments)
        finally:
            self._in_turn = False

    async def _turn(self, text: str, attachments: list[dict[str, Any]] | None) -> None:
        content = user_content(text, attachments)
        if self.carry_over:
            content.insert(0, {"type": "text", "text":
                               "[This chat is being continued from a saved conversation. What was said so far:]\n\n"
                               + self.carry_over + "\n\n[End of the saved conversation. The user's new message follows.]"})
            self.carry_over = ""
        self.history.append(saved.user_message(text, attachments))
        await self._send({"type": "user", "message": {"role": "user", "content": content}})
        tool_names: dict[str, str] = {}     # tool_use id -> display name
        # What to save (see history.py): text and reasoning collect, as they are
        # streamed, until a tool call or the end of the turn closes the message
        # they belong to.
        text_buf = think_buf = ""
        thought = False                         # reasoning happened (its text may not have been sent)
        calling: dict[str, Any] | None = None   # the saved message still collecting tool calls

        def extras() -> dict[str, Any]:
            extra: dict[str, Any] = {"_model": self.label}
            if think_buf:
                extra["_thinking"] = think_buf
            elif thought:
                extra["_thinking_hidden"] = True
            return extra

        while True:
            msg = await self._read()
            if msg is None:
                raise RuntimeError("Claude Code exited in the middle of a reply (see the log)")
            kind = msg.get("type")
            if kind == "stream_event":
                ev = msg.get("event") or {}
                if ev.get("type") == "content_block_start":
                    block_type = (ev.get("content_block") or {}).get("type")
                    if block_type == "text" and text_buf:
                        emit({"type": "delta", "content": "\n\n"})   # a new paragraph, not a run-on
                        text_buf += "\n\n"
                    elif block_type in ("thinking", "redacted_thinking"):
                        # Say so even if no text follows: Claude Code may only
                        # report that reasoning is going on (see "Thinking").
                        thought = True
                        emit({"type": "thinking", "content": "\n\n" if think_buf else ""})
                        think_buf += "\n\n" if think_buf else ""
                elif ev.get("type") == "content_block_delta":
                    delta = ev.get("delta") or {}
                    if delta.get("type") == "text_delta" and delta.get("text"):
                        emit({"type": "delta", "content": delta["text"]})
                        text_buf += delta["text"]
                    elif delta.get("type") == "thinking_delta" and delta.get("thinking"):
                        emit({"type": "thinking", "content": delta["thinking"]})
                        think_buf += delta["thinking"]
            elif kind == "assistant":
                # Text was already streamed; the complete message is where a
                # tool call has all of its arguments.
                for block in (msg.get("message") or {}).get("content") or []:
                    if block.get("type") != "tool_use":
                        continue
                    name = _display_tool(block.get("name", ""))
                    call = {"id": block.get("id", ""), "type": "function",
                            "function": {"name": name, "arguments": json.dumps(block.get("input") or {})}}
                    if calling is None:         # several calls in a row belong to one message
                        calling = {"role": "assistant", "content": text_buf or None, "tool_calls": [], **extras()}
                        self.history.messages.append(calling)
                        text_buf = think_buf = ""
                        thought = False
                    calling["tool_calls"].append(call)
                    self.history.save()
                    tool_names[block.get("id", "")] = name
                    emit({"type": "tool_call", "id": block.get("id", ""), "name": name,
                          "arguments": block.get("input") or {}})
            elif kind == "user":
                content = (msg.get("message") or {}).get("content")
                for block in content if isinstance(content, list) else []:
                    if block.get("type") == "tool_result":
                        tid = block.get("tool_use_id", "")
                        result = _result_text(block.get("content"))
                        if block.get("is_error") and not result.startswith("ERROR"):
                            result = "ERROR: " + result
                        emit({"type": "tool_result", "id": tid, "name": tool_names.get(tid, ""),
                              "content": result})
                        calling = None
                        self.history.append({"role": "tool", "tool_call_id": tid, "content": result})
            elif kind == "result":
                final = msg.get("result") if isinstance(msg.get("result"), str) else ""
                if self._stopping and msg.get("is_error"):
                    # The user stopped it: keep what was written, and leave the
                    # conversation in a state that can be continued.
                    for missing in saved.close_tool_calls(self.history.messages, saved.STOPPED):
                        self.history.append(missing)
                    self.history.append(saved.stopped_message(text_buf, extras()))
                    emit({"type": "stopped"})
                    final = ""
                elif msg.get("is_error"):
                    # e.g. not logged in, usage limit reached, turn limit hit
                    why = final or f"Claude Code: {msg.get('subtype', 'error')}"
                    emit({"type": "error", "message": why})
                    self.history.append({"role": "assistant", "content": f"[No reply: {why}]", "_error": why})
                    final = ""
                elif text_buf or self.history.messages[-1].get("role") != "assistant":
                    self.history.append({"role": "assistant", "content": text_buf or final, **extras()})
                self._save_link()               # Claude Code and the file now hold the same conversation
                emit({"type": "done", "content": final})
                return

    async def stop(self) -> None:
        """End the Claude Code process."""
        if self.proc and self.proc.returncode is None:
            with contextlib.suppress(Exception):
                assert self.proc.stdin
                self.proc.stdin.close()                 # EOF on stdin: Claude Code exits
            try:
                await asyncio.wait_for(self.proc.wait(), 3)
            except asyncio.TimeoutError:
                self.proc.kill()
                await self.proc.wait()
        if self._stderr_task:
            with contextlib.suppress(asyncio.TimeoutError, asyncio.CancelledError):
                await asyncio.wait_for(self._stderr_task, 1)
            self._stderr_task = None


# --------------------------------------------------------------------------- CLI

async def chat_main(claude: str, cfg: dict[str, Any], config_path: str, model: str) -> int:
    session = ClaudeSession(claude, cfg, config_path, model)
    try:
        tools = await session.start()
    except Exception as e:  # noqa: BLE001 - reported to the log; the page shows that the agent ended
        log(f"error: {e}")
        await session.stop()
        return 1
    emit({"type": "ready", "model": model or "claude", "tools": tools})

    async def fail(e: Exception) -> None:       # Claude Code is gone: say so and end
        emit({"type": "error", "message": f"{type(e).__name__}: {e}"})
        emit({"type": "done", "content": ""})
        await session.stop()
        sys.stdout.flush()
        os._exit(1)

    async def run_turn(msg: dict[str, Any]) -> None:
        try:
            await session.ask(msg.get("content", ""), msg.get("attachments") or None)
        except Exception as e:  # noqa: BLE001
            await fail(e)

    # A turn runs as a task of its own, so that "stop" can be read while it is
    # in progress.
    loop = asyncio.get_running_loop()
    turn: asyncio.Task[None] | None = None
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:                          # stdin closed: the app went away
            break
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            emit({"type": "error", "message": f"bad JSON on stdin: {line[:80]}"})
            continue
        busy = turn is not None and not turn.done()
        try:
            if msg.get("type") == "stop":
                if busy:
                    await session.interrupt()
            elif busy:
                emit({"type": "error", "message": "still answering the previous message"})
            elif msg.get("type") == "reset":  # a new conversation is a new Claude Code process
                await session.stop()
                session.history.clear()
                await session.start()
            elif msg.get("type") == "user":
                turn = asyncio.create_task(run_turn(msg))
        except Exception as e:  # noqa: BLE001
            await fail(e)
    if turn is not None and not turn.done():
        turn.cancel()
    await session.stop()
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=["chat"])
    p.add_argument("--config", help="session config JSON (default: $AI_CONSOLE_CONFIG)")
    p.add_argument("--claude", help="path of the claude CLI (default: searched for)")
    p.add_argument("--model", default="", help="Claude model or alias, e.g. opus (default: Claude Code's own)")
    a = p.parse_args(argv)

    config_path = a.config or os.environ.get("AI_CONSOLE_CONFIG")
    if not config_path:
        log("error: no session config (--config or $AI_CONSOLE_CONFIG)")
        return 2
    with open(config_path, encoding="utf-8") as f:
        cfg = json.load(f)
    claude = a.claude or find_claude()
    if not claude:
        log("error: the `claude` CLI was not found. Install Claude Code (https://claude.com/claude-code), "
            "or set its path in Settings (or in $AI_CONSOLE_CLAUDE).")
        return 2
    log(f"[claude] using {claude}" + (f", model {a.model}" if a.model else ""))

    code = 1
    try:
        code = asyncio.run(chat_main(claude, cfg, config_path, a.model))
    except KeyboardInterrupt:
        pass
    finally:
        sys.stdout.flush()
    # The stdin reader thread may still be blocked in readline(); don't let it hold the exit.
    os._exit(code)


if __name__ == "__main__":
    sys.exit(main())
