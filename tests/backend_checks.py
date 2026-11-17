#!/usr/bin/env python3
"""Checks on the two chat backends, driven the way the app drives them.

    python3 backend_checks.py <project root> <temp dir>

Starts python/agent.py and python/claude_agent.py as the app does (JSON lines
on stdin/stdout) against tests/fake_model.py and tests/fake_claude.py, and
checks stopping a reply, how reasoning is reported, changing the model and the
effort level between messages, and what ends up in the saved conversation. Prints one "ok"/"FAILED" line per check; exit code 1 if any
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
    check("reasoning_effort" not in json.load(open(capture)) and "chat_template_kwargs" not in json.load(open(capture)),
          "local: with no effort level chosen, nothing about thinking is sent")

    # another model and effort level, in the same conversation
    b.send(type="configure", model="org/other-model", label="other-model", effort="medium")
    done = b.until("configured")[-1]
    check(done["model"] == "org/other-model" and done["effort"] == "medium", "local: configure is confirmed")
    b.send(type="user", content="who is this now")
    b.until("done")
    body = json.load(open(capture))
    check(body["model"] == "org/other-model" and len([m for m in body["messages"] if m["role"] == "user"]) == 3,
          "local: the next message goes to the new model, with the whole conversation")
    check(body.get("reasoning_effort") == "medium" and body.get("chat_template_kwargs") == {"enable_thinking": True}
          and body.get("thinking_token_budget") == 2048, "local: effort medium asks for thinking, with a budget")
    check(saved(history)[-1].get("_model") == "other-model", "local: the reply is saved under the new model's name")
    b.send(type="configure", effort="off")
    b.until("configured")
    b.send(type="user", content="no thinking please")
    b.until("done")
    body = json.load(open(capture))
    check(body.get("reasoning_effort") == "none" and body.get("chat_template_kwargs") == {"enable_thinking": False}
          and "thinking_token_budget" not in body, "local: effort off switches thinking off")
    b.send(type="configure", effort="")
    b.until("configured")
    b.send(type="user", content="back to the default")
    b.until("done")
    check("reasoning_effort" not in json.load(open(capture)), "local: back to the default, nothing is sent again")

    # a server that refuses fields it does not know
    b.send(type="configure", model="strict-model", effort="high")
    b.until("configured")
    b.send(type="user", content="strict server")
    events = b.until("done")
    body = json.load(open(capture))
    check(events[-1]["content"] == "ok, seen." and not any(e["type"] == "error" for e in events)
          and body["model"] == "strict-model" and "reasoning_effort" not in body
          and open(capture + ".refused").read() == "1",
          "local: a server that refuses the effort fields is asked again without them")
    b.send(type="user", content="strict server again")
    b.until("done")
    check(open(capture + ".refused").read() == "1", "local: ...and not sent them again")
    b.close()
finally:
    server.kill()

# a model server that is still starting: the first message waits for it
port = free_port()
config, history = write_config("local-wait", base_url=f"http://127.0.0.1:{port}/v1", model="test-model",
                               wait_for_server=True)
b = Backend("agent.py", config)
b.until("ready")
b.send(type="user", content="are you there yet")
waiting = b.until("status", timeout=10)
server = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "fake_model.py"), str(port), capture])
try:
    events = b.until("done", timeout=20)
    check(waiting[-1]["message"].startswith("waiting for the model server")
          and events[-1]["content"] == "ok, seen." and not any(e["type"] == "error" for e in events),
          "local: a message sent while the model server is starting waits for it")
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
check("--effort" not in argv and "--model" not in argv, "claude: no effort or model chosen, none passed")
b.close()

# another model, then another effort level, in the same conversation
config, history = write_config("claude-switch", model="unused", effort="high")
b = Backend("claude_agent.py", config, env, ["--claude", fake, "--model", "claude-sonnet-5-5"])
b.until("ready")
argv = json.load(open(claude_cap))["argv"]
check(argv[argv.index("--effort") + 1] == "high" and argv[argv.index("--model") + 1] == "claude-sonnet-5-5",
      "claude: started with the chosen model and effort")
b.send(type="user", content="hello")
b.until("done")
b.send(type="configure", model="claude-opus-5-5", label="Claude Opus 5.5", effort="high")
done = b.until("configured")[-1]
cap = json.load(open(claude_cap))
asked = [m["request"] for m in cap["stdin"] if m["type"] == "control_request" and m["request"]["subtype"] == "set_model"]
check(done["model"] == "claude-opus-5-5" and asked == [{"subtype": "set_model", "model": "claude-opus-5-5"}]
      and cap["argv"] == argv, "claude: another model is handed to the running Claude Code")
b.send(type="user", content="hello again")
b.until("done")
check(saved(history)[-1].get("_model") == "Claude Opus 5.5", "claude: the reply is saved under the new model's name")
b.send(type="configure", effort="max")
done = b.until("configured")[-1]
argv2 = json.load(open(claude_cap))["argv"]
check(done["effort"] == "max" and argv2[argv2.index("--effort") + 1] == "max"
      and argv2[argv2.index("--model") + 1] == "claude-opus-5-5" and "--resume" in argv2
      and argv2[argv2.index("--resume") + 1] == argv[argv.index("--session-id") + 1],
      "claude: another effort level restarts Claude Code on the same conversation")
b.send(type="user", content="and once more")
check(b.until("done")[-1]["content"] == "ok, seen.", "claude: the chat carries on after the restart")
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

config, history = write_config("claude-old-effort", model="unused", effort="high")
b = Backend("claude_agent.py", config, {**env, "FAKE_CLAUDE_OLD": "1"}, ["--claude", fake])
b.until("ready")
check("--effort" not in json.load(open(claude_cap))["argv"], "claude: a version without --effort is started without it")
b.send(type="user", content="hello")
b.until("done")
b.send(type="configure", model="claude-haiku-5-5", effort="high")
b.until("configured")
argv = json.load(open(claude_cap))["argv"]
check(argv[argv.index("--model") + 1] == "claude-haiku-5-5" and "--resume" in argv,
      "claude: a version without set_model is restarted with the new model")
b.close()

print(f"backend checks: {failures} failure(s)")
sys.exit(1 if failures else 0)
