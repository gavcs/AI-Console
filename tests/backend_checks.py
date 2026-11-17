#!/usr/bin/env python3
"""Checks on the two chat backends, driven the way the app drives them.

    python3 backend_checks.py <project root> <temp dir>

Starts python/agent.py and python/claude_agent.py as the app does (JSON lines
on stdin/stdout) against tests/fake_model.py and tests/fake_claude.py, and
checks stopping a reply, how reasoning is reported, and what ends up in the
saved conversation. Prints one "ok"/"FAILED" line per check; exit code 1 if any
failed.
"""
import json
import os
import queue
import socket
import subprocess
import sys
import threading
import time

ROOT, TMP = sys.argv[1], sys.argv[2]
failures = 0


def check(ok, what):
    global failures
    print(f"  {'ok' if ok else 'FAILED'}: {what}")
    failures += 0 if ok else 1


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Backend:
    """A backend process and the events it writes."""

    def __init__(self, script, config, env=None, args=()):
        self.proc = subprocess.Popen(
            [sys.executable, os.path.join(ROOT, "python", script), "chat", "--config", config, *args],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env={**os.environ, **(env or {})})
        self.events = queue.Queue()
        self.log = []
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=lambda: self.log.extend(self.proc.stderr), daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.events.put(json.loads(line))

    def send(self, **msg):
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()

    def until(self, kind, timeout=20):
        """Events up to and including the first of type `kind` (raises if it does not come)."""
        seen, deadline = [], time.time() + timeout
        while True:
            try:
                ev = self.events.get(timeout=max(0.01, deadline - time.time()))
            except queue.Empty:
                raise AssertionError(f"no {kind!r} within {timeout} s; got {seen}; log: {''.join(self.log)[-800:]}")
            seen.append(ev)
            if ev["type"] == kind:
                return seen

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


def write_config(name, **cfg):
    folder = os.path.join(TMP, name)
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, "session.json")
    cfg.setdefault("history", os.path.join(folder, "messages.json"))
    cfg.setdefault("mcp_servers", [])
    with open(path, "w") as f:
        json.dump(cfg, f)
    return path, cfg["history"]


def saved(path):
    with open(path) as f:
        return json.load(f)["messages"]


def stop_and_continue(backend, history, who):
    """Stop a slow reply half-way, then ask something else."""
    backend.until("ready")
    backend.send(type="user", content="a slow one please")
    backend.until("delta")
    backend.until("delta")
    started = time.time()
    backend.send(type="stop")
    events = backend.until("done", timeout=5)
    check(any(e["type"] == "stopped" for e in events) and time.time() - started < 3, f"{who}: reply stops promptly")
    backend.send(type="user", content="and now a quick one")
    events = backend.until("done")
    check(events[-1]["content"] == "ok, seen.", f"{who}: the chat carries on after a stop")
    msgs = saved(history)
    cut = msgs[1]
    check([m["role"] for m in msgs if m["role"] != "tool" and not m.get("tool_calls")] ==
          ["user", "assistant", "user", "assistant"]
          and cut.get("_stopped") and cut["content"].startswith("word0 ") and "_error" not in cut,
          f"{who}: what was written before the stop is saved")
    return msgs


# ---- local model ---------------------------------------------------------
port = free_port()
capture = os.path.join(TMP, "checks_model_cap.json")
server = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "fake_model.py"), str(port), capture])
try:
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            break
        except OSError:
            time.sleep(0.1)

    print("local model")
    config, history = write_config("local", base_url=f"http://127.0.0.1:{port}/v1", model="test-model",
                                   api_key="sk-test-123")
    b = Backend("agent.py", config)
    stop_and_continue(b, history, "local")
    sent = json.load(open(capture))["messages"]
    check(sent[1]["role"] == "assistant" and sent[1]["content"].startswith("word0 "),
          "local: the model is shown its own cut-off reply next time")
    headers = {k.lower(): v for k, v in json.load(open(capture + ".headers")).items()}
    check(headers.get("authorization") == "Bearer sk-test-123", "local: the API key is sent to the model server")
    b.close()
finally:
    server.kill()

# ---- Claude Code ---------------------------------------------------------
fake = os.path.join(ROOT, "tests", "fake_claude.py")
claude_cap = os.path.join(TMP, "checks_claude_cap.json")
env = {"FAKE_CLAUDE_CAPTURE": claude_cap}

print("Claude Code")
config, history = write_config("claude", model="unused")
b = Backend("claude_agent.py", config, env, ["--claude", fake])
stop_and_continue(b, history, "claude")
argv = json.load(open(claude_cap))["argv"]
check(argv[argv.index("--thinking-display") + 1] == "summarized", "claude: asked to send its reasoning")
b.close()

config, history = write_config("claude-thinking", model="unused")
b = Backend("claude_agent.py", config, env, ["--claude", fake])
b.until("ready")
b.send(type="user", content="hello")
events = b.until("done")
thinking = [e["content"] for e in events if e["type"] == "thinking"]
kinds = [e["type"] for e in events]
check("".join(thinking) == "Let me look that up." and kinds.index("thinking") < kinds.index("tool_call"),
      "claude: the reasoning summary is passed on as thinking, before what it led to")
b.send(type="user", content="hidden reasoning this time")
events = b.until("done")
thinking = [e["content"] for e in events if e["type"] == "thinking"]
check(thinking == [""] and events[-1]["content"] == "ok, seen.",
      "claude: reasoning without text is still reported (an empty thinking event)")
msgs = saved(history)
check(msgs[1].get("_thinking") == "Let me look that up." and msgs[-1].get("_thinking_hidden") is True
      and "_thinking" not in msgs[-1], "claude: both kinds of reasoning are saved with the reply")
b.close()

config, history = write_config("claude-old", model="unused")
b = Backend("claude_agent.py", config, {**env, "FAKE_CLAUDE_OLD": "1"}, ["--claude", fake])
b.until("ready")
b.send(type="user", content="hello")
check(b.until("done")[-1]["content"] == "ok, seen." and "--thinking-display" not in json.load(open(claude_cap))["argv"],
      "claude: a version without --thinking-display is started without it")
b.close()

print(f"backend checks: {failures} failure(s)")
sys.exit(1 if failures else 0)
