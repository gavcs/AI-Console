#!/usr/bin/env python3
"""Ask Claude Code (`claude -p`) to write a Python script against the agent API.

    python3 gen_script.py --out ~/ai-console/scripts/foo.py --prompt "what the script should do"
                          [--config session.json]   # lets Claude know which MCP tools exist

Uses your existing Claude Code login (no API key). Runs `claude` with cwd set to this
directory so it can *read* agent.py for the API, but with every mutating tool disabled,
so the only output is the script text. The first ```python block in the reply is saved.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

import claude_agent

HERE = Path(__file__).resolve().parent

SYSTEM = """You are writing ONE standalone Python 3 script for the user's local AI console.

Contract the script must follow:
- {here} is on PYTHONPATH, so `from agent import Agent` works from any directory.
  Read agent.py in this directory for the exact API before writing.
- `with Agent() as agent:` connects to the local model server and to any configured MCP tool
  servers. Configuration comes from the AI_CONSOLE_CONFIG environment variable, already set.
- `agent.ask(prompt) -> str` runs one complete turn: the model may call MCP tools as many times as
  it needs; the streamed answer and tool activity are printed automatically. Call it in a loop or
  with different prompts as the task requires. `agent.tools` lists the tool names available.
- Do not call the model server or MCP servers directly; always go through Agent.
- MCP tools available right now: {tools}
- Keep it readable: a short docstring, argparse if it takes inputs, no unnecessary dependencies.

Reply with ONLY the complete script inside a single ```python code block. No prose before or after.
"""

def find_claude() -> str:
    """Path of the `claude` CLI (same search as the chat backend), or exit with what to do."""
    path = claude_agent.find_claude()
    if not path:
        sys.exit("error: the `claude` CLI was not found. Install Claude Code (https://claude.com/claude-code), "
                 "or set its path in Settings.")
    return path


def list_tools(config: str | None) -> str:
    if not config:
        return "none configured (no MCP servers selected)"
    try:
        r = subprocess.run([sys.executable, str(HERE / "agent.py"), "tools", "--config", config],
                           capture_output=True, text=True, timeout=60)
        names = [ln for ln in r.stdout.splitlines() if ln.strip()]
        return ", ".join(names) if names else "none reachable (MCP servers configured but not connected)"
    except (subprocess.TimeoutExpired, OSError) as e:
        return f"unknown ({e})"


def extract_code(reply: str) -> str:
    m = re.search(r"```(?:python|py)?\s*\n(.*?)```", reply, re.DOTALL)
    return (m.group(1) if m else reply).strip() + "\n"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", required=True, help="where to write the generated script")
    p.add_argument("--prompt", required=True, help="what the script should do")
    p.add_argument("--config", help="session config JSON, to tell Claude which tools exist")
    p.add_argument("--model", help="Claude model override for the generation (optional)")
    a = p.parse_args()

    claude = find_claude()
    tools = list_tools(a.config)
    print(f"[gen] using {claude}", file=sys.stderr, flush=True)
    print(f"[gen] tools visible to the script: {tools}", file=sys.stderr, flush=True)

    cmd = [claude, "-p", "--output-format", "text", "--no-session-persistence",
           "--append-system-prompt", SYSTEM.format(here=HERE, tools=tools),
           "--disallowedTools", "Bash", "Edit", "Write", "NotebookEdit", "WebFetch", "WebSearch"]
    if a.model:
        cmd += ["--model", a.model]

    print("[gen] asking Claude...", file=sys.stderr, flush=True)
    r = subprocess.run(cmd, input=a.prompt, capture_output=True, text=True, cwd=HERE)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        return r.returncode or 1

    code = extract_code(r.stdout)
    out = Path(a.out).expanduser()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(code, encoding="utf-8")

    print(code)
    print(f"\n[gen] wrote {out}", file=sys.stderr, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
