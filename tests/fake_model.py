#!/usr/bin/env python3
"""A stand-in for an OpenAI-compatible model server, for tests.

Speaks just enough of the /v1/chat/completions streaming API for the agent to
complete one turn. It writes the exact request body it received to a capture
file (and the request headers next to it, as <capture>.headers), so a test can
assert on how the app built the message (content shape, image_url parts,
downscaled dimensions, ...).

    python3 fake_model.py <port> <capture_file> [reply_text]

A user message containing the word "slow" is answered one word every 0.1 s for
10 s, so that a test can stop the reply half-way.

A request for the model "strict-model" that carries any field outside the plain
OpenAI ones is refused with HTTP 400, as a strict server would; such a refusal
is not written to the capture file but counted in <capture>.refused.
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])
CAPTURE = sys.argv[2]
REPLY = sys.argv[3] if len(sys.argv) > 3 else "ok, seen."
PLAIN_FIELDS = {"model", "messages", "stream", "tools", "tool_choice"}
refused = 0


def last_user_text(body):
    for m in reversed(body.get("messages", [])):
        if m.get("role") == "user":
            c = m.get("content")
            return c if isinstance(c, str) else " ".join(p.get("text", "") for p in c if isinstance(p, dict))
    return ""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):          # keep the test output clean
        pass

    def do_GET(self):                   # /v1/models, for the app's "is the server up" check
        data = json.dumps({"data": [{"id": "test-model"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n) or b"{}")
        if body.get("model") == "strict-model" and set(body) - PLAIN_FIELDS:
            global refused
            refused += 1
            with open(CAPTURE + ".refused", "w", encoding="utf-8") as f:
                f.write(str(refused))
            data = json.dumps({"error": {"message": "Unrecognized request argument"}}).encode()
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        with open(CAPTURE, "w", encoding="utf-8") as f:
            json.dump(body, f, indent=2)
        with open(CAPTURE + ".headers", "w", encoding="utf-8") as f:
            json.dump(dict(self.headers.items()), f, indent=2)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()

        def ev(d):
            self.wfile.write(b"data: " + json.dumps(d).encode() + b"\n\n")
            self.wfile.flush()

        try:
            if "slow" in last_user_text(body):
                for i in range(100):
                    ev({"choices": [{"delta": {"content": f"word{i} "}}]})
                    time.sleep(0.1)
            else:
                ev({"choices": [{"delta": {"content": REPLY}}]})
            ev({"choices": [{"delta": {}, "finish_reason": "stop"}]})
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass                        # the client stopped listening: the reply was stopped


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
