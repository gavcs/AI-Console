# Tests

Headless, end-to-end checks. They drive the **real** widgets and the **real**
`python/` backends; only the model server and the `claude` CLI are replaced by
stand-ins. No model, login or network is needed, and nothing of yours is read
or written: settings go to a separate "AI Console Test" organisation and
sessions to a temp folder.

## Run

```sh
make test
```

That builds `bin/chat_driver` and `bin/session_driver` from the app's own
objects and runs `tests/run_tests.sh`. You can also run the script directly
against existing drivers: `bash tests/run_tests.sh bin/chat_driver`.

Requires the same toolchain as the app (Qt 6 via Homebrew, `clang++`) plus a
`python3` with the requirements installed (`pip install -r requirements.txt`).
`SHOTS=<dir> make test` keeps a screenshot of each step of the walk-through.

## What's here

- **`run_tests.sh`** — generates fixtures, runs everything below, and asserts
  on what the stand-ins were sent and on what was saved.
- **`chat_driver.cpp`** — constructs a `ChatPage`, starts a backend, attaches
  files, types a prompt, presses Enter, and saves a screenshot. Flags:
  `--cfg <session.json>` (required), `--attach <file>` (repeatable),
  `--prompt <text>`, `--shot <png>`, `--backend claude`,
  `--replay <messages.json>`. The flag is `--cfg`, not `--session`: Qt reserves
  `-session` and would swallow it.
- **`session_driver.cpp`** — walks the real `MainWindow` by clicking its
  widgets: start a chat with the local model, leave it, continue it with Claude
  Code, continue it with the local model, stop a reply half-way, run a script,
  filter the session list, then create a second window (an app restart) and
  reopen both sessions. It also checks the settings: the defaults, the provider
  presets, and how the server command is built from them. Flags:
  `--script <file>`, `--shots <dir>`.
- **`backend_checks.py`** — starts `agent.py` and `claude_agent.py` the way the
  app does (JSON lines on stdin/stdout) and checks stopping a reply, how
  reasoning is reported, and what ends up in the saved conversation.
- **`fake_model.py`** — a stand-in for an OpenAI-compatible model server. It
  answers `/v1/models` and streams a reply to `/v1/chat/completions`, writing
  the request body (and its headers) to a capture file. A message containing
  "slow" is answered one word at a time for ten seconds, so a test can stop it.
- **`fake_claude.py`** — a stand-in for the `claude` CLI, speaking enough of
  Claude Code's stream-json protocol for a turn with reasoning, a tool call and
  an answer; it records its command line and everything it was sent. "hidden"
  in a message makes it reason without sending the text, "slow" makes it
  interruptible, and `FAKE_CLAUDE_OLD=1` makes it refuse `--thinking-display`
  like an older Claude Code.

## Overrides the app honours (used here)

| Variable | Effect |
|---|---|
| `AI_CONSOLE_PYTHON_DIR` | Where `python/` is (the drivers don't live in `bin/` next to it in every layout). |
| `AI_CONSOLE_SERVER_PORT` | Talk to a model server on this local port instead of the one in Settings. |
| `AI_CONSOLE_CLAUDE` | The `claude` program to run. |
| `AI_CONSOLE_HISTORY_DIR` | Where sessions are saved. |

## Coverage

- **Attachments**: text files inlined, binaries referenced by path, images as
  `image_url`; a small image sent unchanged, a large one (2400×1600) scaled to
  a 1568 long edge; text-only messages flattened to one string.
- **Claude Code backend**: the session's MCP servers translated into Claude
  Code's config; Claude Code started with only those tools and no permission
  bypass; a full turn through the real `ChatPage`; attachments sent as content
  blocks.
- **Stopping a reply** (both backends): it stops promptly, what was written is
  kept and shown to the model next time, and the chat carries on.
- **Reasoning**: Claude's reasoning summary is passed on as thinking; reasoning
  without text is still reported; a Claude Code without `--thinking-display` is
  started without it.
- **Saved sessions**: `history.py` (saving, repairing a cut-off turn, keeping
  private keys from the model); the walk-through; that Claude Code is handed
  the local model's turns and the local model is handed Claude's, tool call
  included; that a script run's output and exit code are saved.
- **Settings**: defaults equal the app's original behaviour; provider presets
  fill the dialog; `{model}` and `{port}` are substituted one argument each;
  the chosen Python runs the agent and scripts; an API key is sent as a Bearer
  token.

## Extending

`run_driver` in `run_tests.sh` leaves the captured request at `$TMP/cap.json`
and a screenshot at `$TMP/shot.png`. Add a scenario by calling it with
different `--attach` files and a `check_capture` block (its `uc` is the last
user message's `content`; `img_dims(data_url)` decodes PNG/JPEG dimensions).
For the window as a whole, add steps to `session_driver.cpp`; for a backend's
protocol, add a check to `backend_checks.py`.

Waiting is done by pumping the event loop until a condition holds (the input
is enabled again, a process has finished), not by fixed sleeps.
