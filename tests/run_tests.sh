#!/usr/bin/env bash
# run_tests.sh — drive the app headlessly and check what it does.
#
# The real widgets and the real python/ backends run against a scripted fake
# model server (fake_model.py) and a fake claude CLI (fake_claude.py), which
# record what the app sent them. Covers attachments, both backends, stopping a
# reply, changing the model and the effort level in a chat, saved sessions, the
# settings, and that closing the app stops what it started.
#
#   make test            # builds the driver, then runs this
#   bash tests/run_tests.sh [path-to-chat_driver]
#
# No real model is needed. Nothing writes to the user's real settings.

set -u
TESTS="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$TESTS/.." && pwd)"
DRIVER="${1:-$ROOT/bin/chat_driver}"
PYTHON="${PYTHON:-python3}"

if [ ! -x "$DRIVER" ]; then
    echo "driver not found: $DRIVER  (run: make test)"; exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null' EXIT
PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

# ---- fixtures ------------------------------------------------------------
"$PYTHON" - "$TMP" <<'PY'
import sys, os, struct, zlib
tmp = sys.argv[1]
def png_rgba(path, w, h, rgba=(200,60,60,255)):
    row = b"\x00" + bytes(rgba) * w
    raw = row * h
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t+d) & 0xffffffff)
    data = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    open(path, "wb").write(data)
png_rgba(os.path.join(tmp, "small.png"), 8, 8)         # within the cap: sent as-is
png_rgba(os.path.join(tmp, "big.png"), 2400, 1600)     # over the cap: must be downscaled
open(os.path.join(tmp, "code.py"), "w").write("def add(a, b):\n    return a + b\n")
open(os.path.join(tmp, "blob.bin"), "wb").write(os.urandom(4096))
print("fixtures ready")
PY

free_port() { "$PYTHON" -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'; }

# run_driver <capture_out> <attach-args...> ; prompt is fixed.
run_driver() {
    local cap="$1"; shift
    local port; port="$(free_port)"
    rm -f "$cap"
    "$PYTHON" "$TESTS/fake_model.py" "$port" "$cap" & SRV=$!
    # wait for the fake server to accept connections
    for _ in $(seq 1 40); do
        "$PYTHON" -c "import socket,sys;socket.create_connection(('127.0.0.1',$port),0.2).close()" 2>/dev/null && break
        sleep 0.1
    done
    printf '{ "base_url": "http://127.0.0.1:%s/v1", "model": "test-model", "mcp_servers": [] }\n' "$port" > "$TMP/session.json"
    QT_QPA_PLATFORM=offscreen AI_CONSOLE_PYTHON_DIR="$ROOT/python" \
        "$DRIVER" --cfg "$TMP/session.json" --shot "$TMP/shot.png" "$@" >/dev/null 2>>"$TMP/driver.log"
    { kill "$SRV" && wait "$SRV"; } 2>/dev/null; SRV=""
}

# check_capture <cap> <python-asserts>: runs python with `uc`=last user content.
check_capture() {
    local cap="$1" code="$2"
    "$PYTHON" - "$cap" <<PY
import sys, json, base64, struct
cap = sys.argv[1]
try:
    body = json.load(open(cap))
except Exception as e:
    print("NOCAP", e); sys.exit(3)
uc = body["messages"][-1]["content"]
def img_dims(data_url):
    raw = base64.b64decode(data_url.split(",", 1)[1])
    if raw[:8] == b"\x89PNG\r\n\x1a\n":
        w, h = struct.unpack(">II", raw[16:24]); return ("png", w, h)
    if raw[:2] == b"\xff\xd8":
        i = 2
        while i < len(raw) - 9:
            if raw[i] != 0xFF: i += 1; continue
            m = raw[i+1]
            if m in (0xC0, 0xC1, 0xC2, 0xC3):
                h, w = struct.unpack(">HH", raw[i+5:i+9]); return ("jpeg", w, h)
            i += 2 + struct.unpack(">H", raw[i+2:i+4])[0]
        return ("jpeg", None, None)
    return ("?", None, None)
$code
print("ASSERTS_OK")
PY
}

echo "== unit: agent._user_content =="
if "$PYTHON" - "$ROOT" <<'PY' 2>/dev/null | grep -q UNIT_OK
import sys; sys.path.insert(0, sys.argv[1] + "/python")
import agent
uc = agent.AsyncAgent._user_content
img = {"kind":"image","name":"p.png","mime":"image/png","path":"/x/p.png","data_url":"data:image/png;base64,AAAA"}
txt = {"kind":"text","name":"main.py","mime":"text/x-python","path":"/x/main.py","text":"print(1)\n"}
bn  = {"kind":"binary","name":"a.out","mime":"application/x-mach-binary","path":"/x/a.out","size":2048}
s = uc("look", [txt, bn])
assert isinstance(s, str) and "print(1)" in s and "/x/a.out" in s and "2048 bytes" in s, s
parts = uc("cap", [img, txt, bn])
assert [p["type"] for p in parts] == ["text","image_url","text","text"], parts
assert uc("hi", None) == "hi"
t2 = dict(txt); t2["truncated"] = True
assert "(truncated)" in uc("", [t2])
print("UNIT_OK")
PY
then ok "content builder: text inlined, binary referenced, image as image_url, truncation noted"
else bad "content builder unit checks"; fi

echo "== e2e: small image (no downscale) =="
run_driver "$TMP/cap.json" --attach "$TMP/small.png"
if check_capture "$TMP/cap.json" '
assert isinstance(uc, list), "image message should be a parts array"
iu = [p for p in uc if p["type"]=="image_url"][0]["image_url"]["url"]
fmt, w, h = img_dims(iu)
assert (fmt, w, h) == ("png", 8, 8), (fmt, w, h)
' | grep -q ASSERTS_OK; then ok "8x8 image sent unchanged (8x8 png)"; else bad "small image unchanged"; fi

echo "== e2e: large image (downscaled) =="
run_driver "$TMP/cap.json" --attach "$TMP/big.png"
if check_capture "$TMP/cap.json" '
iu = [p for p in uc if p["type"]=="image_url"][0]["image_url"]["url"]
fmt, w, h = img_dims(iu)
assert max(w, h) == 1568, "long edge should be capped at 1568, got %r" % ((w, h),)
assert (w, h) == (1568, 1045) or abs(w/h - 1.5) < 0.02, (w, h)
' | grep -q ASSERTS_OK; then ok "2400x1600 image downscaled to long-edge 1568"; else bad "large image downscaled"; fi

echo "== e2e: text + binary (no image -> single string) =="
run_driver "$TMP/cap.json" --attach "$TMP/code.py" --attach "$TMP/blob.bin"
if check_capture "$TMP/cap.json" '
assert isinstance(uc, str), "text-only message should be one string"
assert "def add(a, b)" in uc, "text file not inlined"
assert "blob.bin" in uc and "4096 bytes" in uc and "/blob.bin" in uc, "binary not referenced by path"
' | grep -q ASSERTS_OK; then ok "text inlined + binary referenced, flattened to a string"; else bad "text+binary string"; fi

echo "== e2e: image + text (-> parts array) =="
run_driver "$TMP/cap.json" --attach "$TMP/small.png" --attach "$TMP/code.py"
if check_capture "$TMP/cap.json" '
assert isinstance(uc, list), "should be a parts array when an image is present"
kinds = [p["type"] for p in uc]
assert kinds.count("image_url") == 1 and kinds.count("text") >= 2, kinds
assert any("def add(a, b)" in p.get("text","") for p in uc if p["type"]=="text")
' | grep -q ASSERTS_OK; then ok "image as image_url + text file as a text part"; else bad "image+text parts"; fi

# ---- Claude Code backend ---------------------------------------------------
# The real ChatPage and the real python/claude_agent.py; the `claude` CLI is
# replaced by fake_claude.py, which records how it was started and what it was sent.
echo "== unit: claude_agent config + message builders =="
if "$PYTHON" - "$ROOT" <<'PY' 2>/dev/null | grep -q UNIT_OK
import sys; sys.path.insert(0, sys.argv[1] + "/python")
import claude_agent as ca
conf, allowed = ca.mcp_config([
    {"name": "files", "transport": "stdio", "command": "python3", "args": ["s.py"], "env": {"A": "1"}},
    {"name": "in dir", "transport": "stdio", "command": "srv", "args": ["-x"], "cwd": "/w d"},
    {"name": "web", "transport": "http", "url": "http://h/mcp", "headers": {"Authorization": "Bearer t"}, "tools": ["a", "b"]},
])
s = conf["mcpServers"]
assert s["files"] == {"command": "python3", "args": ["s.py"], "env": {"A": "1"}}, s
assert s["in_dir"]["command"] == "/bin/sh" and s["in_dir"]["args"][-3:] == ["/w d", "srv", "-x"], s
assert s["web"] == {"type": "http", "url": "http://h/mcp", "headers": {"Authorization": "Bearer t"}}, s
assert allowed == ["mcp__files", "mcp__in_dir", "mcp__web__a", "mcp__web__b"], allowed
img = {"kind":"image","name":"p.png","mime":"image/png","path":"/x/p.png","data_url":"data:image/png;base64,AAAA"}
txt = {"kind":"text","name":"main.py","mime":"text/x-python","path":"/x/main.py","text":"print(1)\n"}
parts = ca.user_content("cap", [img, txt])
assert [p["type"] for p in parts] == ["text", "image", "text"], parts
assert parts[1]["source"] == {"type": "base64", "media_type": "image/png", "data": "AAAA"}, parts
assert "print(1)" in parts[2]["text"]
assert ca._display_tool("mcp__files__read_file") == "files__read_file"
print("UNIT_OK")
PY
then ok "MCP config translated (stdio, cwd, http, allow-list); image as base64 block"
else bad "claude_agent unit checks"; fi

echo "== e2e: chat through claude_agent.py (fake claude) =="
printf '{ "model": "unused", "mcp_servers": [ {"name": "demo", "transport": "stdio", "command": "demo-server", "args": []} ] }\n' > "$TMP/session.json"
rm -f "$TMP/claude_cap.json"
QT_QPA_PLATFORM=offscreen AI_CONSOLE_PYTHON_DIR="$ROOT/python" \
    AI_CONSOLE_CLAUDE="$TESTS/fake_claude.py" FAKE_CLAUDE_CAPTURE="$TMP/claude_cap.json" \
    "$DRIVER" --backend claude --cfg "$TMP/session.json" --shot "$TMP/shot.png" \
    --attach "$TMP/small.png" --attach "$TMP/code.py" >/dev/null 2>>"$TMP/driver.log"
DRIVER_RC=$?
if [ "$DRIVER_RC" -eq 0 ] && "$PYTHON" - "$TMP/claude_cap.json" <<'PY' 2>>"$TMP/driver.log" | grep -q ASSERTS_OK
import sys, json
cap = json.load(open(sys.argv[1]))
argv = cap["argv"]
def opt(name): return argv[argv.index(name) + 1]
assert "-p" in argv and "--strict-mcp-config" in argv, argv
assert opt("--input-format") == "stream-json" and opt("--output-format") == "stream-json", argv
assert opt("--tools") == "", "Claude Code's own tools must be off"
assert opt("--permission-mode") == "dontAsk", argv
assert opt("--allowedTools") == "mcp__demo", argv
assert "--dangerously-skip-permissions" not in argv
mcp = json.load(open(opt("--mcp-config")))
assert mcp == {"mcpServers": {"demo": {"command": "demo-server", "args": []}}}, mcp
users = [m for m in cap["stdin"] if m["type"] == "user"]
assert len(users) == 1, "exactly one user turn"
content = users[0]["message"]["content"]
kinds = [p["type"] for p in content]
assert kinds == ["text", "image", "text"], kinds
assert content[1]["source"]["media_type"] == "image/png" and content[1]["source"]["data"]
assert "def add(a, b)" in content[2]["text"]
print("ASSERTS_OK")
PY
then ok "claude started with only the session's MCP tools; turn completed; image + text sent as blocks"
else bad "claude backend e2e (driver exit $DRIVER_RC)"; fi

# ---- the two backends, driven the way the app drives them --------------------
echo "== backends: stopping a reply, reasoning, API key =="
mkdir -p "$TMP/checks"
if "$PYTHON" "$TESTS/backend_checks.py" "$ROOT" "$TMP/checks" > "$TMP/backend_checks.log" 2>&1
then ok "$(grep -c '^  ok' "$TMP/backend_checks.log") backend checks (stop keeps what was written; Claude's reasoning shown; key sent)"
else bad "backend checks"; grep -E "FAILED|Error|error" "$TMP/backend_checks.log" | head -10; fi

# ---- saved sessions ---------------------------------------------------------
echo "== unit: history.py =="
if "$PYTHON" - "$ROOT" "$TMP" <<'PY' 2>>"$TMP/driver.log" | grep -q UNIT_OK
import sys, json; sys.path.insert(0, sys.argv[1] + "/python")
import history
path = sys.argv[2] + "/h.json"
h = history.History(path)
h.append(history.user_message("hi", [{"kind": "text", "name": "a.txt", "path": "/x/a.txt", "text": "T"}]))
h.append({"role": "assistant", "content": None, "_model": "m", "_thinking": "hm",
          "tool_calls": [{"id": "1", "type": "function", "function": {"name": "t", "arguments": "{}"}}]})
again = history.History(path)            # as if the backend had been stopped mid-turn
roles = [m["role"] for m in again.messages]
assert roles == ["user", "assistant", "tool", "assistant"], roles
assert again.messages[-1].get("_error") and again.messages[0]["_text"] == "hi"
assert again.messages[0]["_attachments"] == [{"name": "a.txt", "path": "/x/a.txt", "kind": "text"}]
assert json.load(open(path))["messages"] == again.messages, "the repair is saved"
sent = history.for_model(again.messages)
assert not any(k.startswith("_") for m in sent for k in m), sent
text = history.transcript(again.messages)
assert "User: hi" in text and "called the tool t" in text and "Tool result:" in text, text
print("UNIT_OK")
PY
then ok "conversation saved, cut-off turn repaired, private keys kept from the model"
else bad "history.py unit checks"; fi

echo "== e2e: saved sessions through the main window =="
SESSION_DRIVER="$(dirname "$DRIVER")/session_driver"
if [ ! -x "$SESSION_DRIVER" ]; then
    bad "session_driver not built (run: make test)"
else
    port="$(free_port)"
    "$PYTHON" "$TESTS/fake_model.py" "$port" "$TMP/model_cap.json" & SRV=$!
    for _ in $(seq 1 40); do
        "$PYTHON" -c "import socket,sys;socket.create_connection(('127.0.0.1',$port),0.2).close()" 2>/dev/null && break
        sleep 0.1
    done
    echo 'print("hello from the script")' > "$TMP/hello.py"
    mkdir -p "$TMP/history" "${SHOTS:-$TMP/shots}"
    CHAT_ID="$(QT_QPA_PLATFORM=offscreen AI_CONSOLE_PYTHON_DIR="$ROOT/python" \
        AI_CONSOLE_SERVER_PORT="$port" AI_CONSOLE_HISTORY_DIR="$TMP/history" \
        AI_CONSOLE_CLAUDE="$TESTS/fake_claude.py" FAKE_CLAUDE_CAPTURE="$TMP/claude_cap.json" \
        "$SESSION_DRIVER" --script "$TMP/hello.py" --shots "${SHOTS:-$TMP/shots}" 2>"$TMP/session_driver.log")"
    DRIVER_RC=$?
    { kill "$SRV" && wait "$SRV"; } 2>/dev/null; SRV=""
    if [ "$DRIVER_RC" -eq 0 ]; then ok "settings, closing stops what was started, start, leave, switch model and effort, continue, stop a reply, script run, restart, reopen"
    else bad "session walk-through (driver exit $DRIVER_RC)"; grep -E "FAILED|timed out" "$TMP/session_driver.log"; fi

    if "$PYTHON" - "$TMP/history/$CHAT_ID" "$TMP/model_cap.json" "$TMP/claude_cap.json" <<'PY' 2>>"$TMP/driver.log" | grep -q ASSERTS_OK
import sys, json, glob, os
conv, model_cap, claude_cap = sys.argv[1:]
meta = json.load(open(conv + "/meta.json"))
assert meta["kind"] == "chat" and meta["title"] == "first question", meta
assert meta["config"]["backend"] == "local", "settings follow the last switch"
assert meta["config"]["effort"] == "low" and meta["config"]["claudeModel"] == "claude-opus-5-5", meta
msgs = json.load(open(conv + "/messages.json"))["messages"]
users = [m["_text"] for m in msgs if m["role"] == "user"]
assert users == ["first question", "second question", "opus question", "third question", "a slow question",
                 "fourth question"], users
who = [m.get("_model") for m in msgs if m["role"] == "assistant"]
assert "Claude Code" in who and "Claude Opus 5.5" in who, who
assert not who[0].startswith("Claude") and not who[-1].startswith("Claude"), who
cut = [m for m in msgs if m.get("_stopped")]
assert len(cut) == 1 and cut[0]["content"].startswith("word0 ") and "word99" not in cut[0]["content"], cut
assert any(m.get("_thinking") == "Let me look that up." for m in msgs), "Claude's reasoning saved"
# Claude Code was given the local model's turn as text, since it had never seen it
claude = json.load(open(claude_cap))
first = [m for m in claude["stdin"] if m["type"] == "user"][0]["message"]["content"]
assert "first question" in first[0]["text"] and "ok, seen." in first[0]["text"], first
assert first[-1]["text"] == "second question", first
assert "--session-id" in claude["argv"] and "--no-session-persistence" not in claude["argv"]
# the other Claude model was handed to the Claude Code that was already running
asked = [m["request"] for m in claude["stdin"] if m["type"] == "control_request" and m["request"]["subtype"] == "set_model"]
assert asked == [{"subtype": "set_model", "model": "claude-opus-5-5"}], asked
# the local model was sent the whole conversation, Claude's tool call included, and none of our private keys
body = json.load(open(model_cap))
sent = body["messages"]
assert body.get("reasoning_effort") == "low", "the effort level is kept with the chat and used after a restart"
assert [m["content"] for m in sent if m["role"] == "user"] == users, sent
assert any(m.get("tool_calls") for m in sent) and any(m["role"] == "tool" for m in sent), sent
assert not any(k.startswith("_") for m in sent for k in m), sent
# the script run
runs = [d for d in glob.glob(os.path.dirname(conv) + "/*") if json.load(open(d + "/meta.json"))["kind"] == "script"]
assert len(runs) == 1, runs
rm = json.load(open(runs[0] + "/meta.json"))
assert rm["finished"] and rm["exitCode"] == 0 and rm["title"] == "hello.py", rm
assert "hello from the script" in open(runs[0] + "/output.log").read()
print("ASSERTS_OK")
PY
    then ok "conversation handed local -> Claude Code -> another Claude model -> local intact; effort kept; script output saved"
    else bad "saved session contents"; fi
fi

echo
echo "tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
