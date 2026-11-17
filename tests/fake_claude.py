#!/usr/bin/env python3
"""A stand-in for the `claude` CLI, for tests.

Speaks just enough of Claude Code's stream-json protocol for
python/claude_agent.py to start a session and run turns. It records its command
line and every line it was sent in the file named by $FAKE_CLAUDE_CAPTURE, so a
test can assert on how the app started Claude Code and how it built messages.

What a turn does depends on words in the user's message:
  (default)  reasons (with a summary of the reasoning), calls a tool, answers "ok, seen."
  "hidden"   reasons without sending any reasoning text, then answers
  "slow"     writes one word every 0.1 s for 10 s, and can be interrupted

$FAKE_CLAUDE_OLD=1 makes it refuse --thinking-display and --effort and answer
"set_model" with an error, like a Claude Code version that has none of them.

Use it by pointing the app at it: AI_CONSOLE_CLAUDE=/path/to/fake_claude.py
"""
import json
import os
import queue
import sys
import threading
import time

OLD = bool(os.environ.get("FAKE_CLAUDE_OLD"))
for option in ("--thinking-display", "--effort"):
    if OLD and option in sys.argv:
        sys.stderr.write(f"error: unknown option '{option}'\n")
        sys.exit(1)

capture = {"argv": sys.argv[1:], "cwd": os.getcwd(), "stdin": []}


def save():
    with open(os.environ["FAKE_CLAUDE_CAPTURE"], "w", encoding="utf-8") as f:
        json.dump(capture, f, indent=2)


def out(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def stream(event):
    out({"type": "stream_event", "event": event, "parent_tool_use_id": None})


def start_block(kind):
    stream({"type": "content_block_start", "index": 0, "content_block": {"type": kind}})


def delta(kind, key, text):
    stream({"type": "content_block_delta", "index": 0, "delta": {"type": kind, key: text}})


def result(text="", error=False):
    out({"type": "result", "subtype": "error_during_execution" if error else "success",
         "is_error": error, "result": text})


# stdin is read on a thread, so that an interrupt can arrive in the middle of a reply
inbox: "queue.Queue[dict | None]" = queue.Queue()


def read_stdin():
    for line in sys.stdin:
        inbox.put(json.loads(line))
    inbox.put(None)


threading.Thread(target=read_stdin, daemon=True).start()


def control(msg):
    """Answer a control request. True if it was an interrupt."""
    servers = [{"name": "demo", "status": "connected", "tools": [{"name": "lookup"}]}]
    if OLD and msg["request"]["subtype"] == "set_model":
        out({"type": "control_response",
             "response": {"subtype": "error", "request_id": msg["request_id"], "error": "unsupported request"}})
        return False
    out({"type": "control_response",
         "response": {"subtype": "success", "request_id": msg["request_id"],
                      "response": {"mcpServers": servers}}})
    return msg["request"]["subtype"] == "interrupt"


def slow_reply():
    start_block("text")
    for i in range(100):
        delta("text_delta", "text", f"word{i} ")
        try:
            msg = inbox.get(timeout=0.1)
        except queue.Empty:
            continue
        if msg is None:
            sys.exit(0)
        capture["stdin"].append(msg)
        save()
        if msg["type"] == "control_request" and control(msg):
            out({"type": "user", "message": {"role": "user", "content": [
                {"type": "text", "text": "[Request interrupted by user]"}]}})
            result(error=True)
            return
    result("finished slowly")


def reply(text):
    out({"type": "system", "subtype": "init", "tools": ["mcp__demo__lookup"]})
    if "slow" in text:
        slow_reply()
        return
    start_block("thinking")
    if "hidden" in text:
        delta("thinking_delta", "thinking", "")           # reasoning without its text
    else:
        delta("thinking_delta", "thinking", "Let me look that up.")
        call = {"type": "tool_use", "id": "toolu_1", "name": "mcp__demo__lookup", "input": {"q": "x"}}
        out({"type": "assistant", "message": {"role": "assistant", "content": [call]}})
        out({"type": "user", "message": {"role": "user", "content": [
            {"type": "tool_result", "tool_use_id": "toolu_1", "content": [{"type": "text", "text": "found x"}]}]}})
    start_block("text")
    delta("text_delta", "text", "ok, ")
    delta("text_delta", "text", "seen.")
    out({"type": "assistant", "message": {"role": "assistant",
                                          "content": [{"type": "text", "text": "ok, seen."}]}})
    result("ok, seen.")


save()
while True:
    msg = inbox.get()
    if msg is None:
        break
    capture["stdin"].append(msg)
    save()
    if msg["type"] == "control_request":
        control(msg)
    elif msg["type"] == "user":
        reply(" ".join(p.get("text", "") for p in msg["message"]["content"] if p.get("type") == "text"))
