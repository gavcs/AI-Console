#!/usr/bin/env python3
"""Read-only files MCP server (stdio). Tools: list_dir, read_file, search_files.

Every path is confined to --root (default: your home folder). No backing
service: this process *is* the tool, so the app starts it with the session.
"""
from __future__ import annotations

import argparse
import os
import re
from pathlib import Path

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("files")
ROOT = Path.home()
MAX_CHARS = 20000


def _resolve(p: str) -> Path:
    path = Path(p).resolve() if os.path.isabs(p) else (ROOT / p).resolve()
    if path != ROOT and ROOT not in path.parents:
        raise ValueError(f"path is outside the allowed root {ROOT}")
    return path


@mcp.tool()
def list_dir(path: str = ".") -> str:
    """List a directory (relative to the root, or absolute inside it). Directories end with '/'."""
    d = _resolve(path)
    if not d.is_dir():
        return f"ERROR: not a directory: {d}"
    entries = sorted(d.iterdir(), key=lambda e: (not e.is_dir(), e.name.lower()))
    lines = [f"{e.name}/" if e.is_dir() else f"{e.name}  ({e.stat().st_size} bytes)"
             for e in entries if not e.name.startswith(".")]
    return f"{d}\n" + "\n".join(lines[:500]) + ("\n...(truncated)" if len(lines) > 500 else "")


@mcp.tool()
def read_file(path: str, offset: int = 0, limit: int = 400) -> str:
    """Read a text file. `offset`/`limit` are line numbers, for paging through large files."""
    f = _resolve(path)
    if not f.is_file():
        return f"ERROR: not a file: {f}"
    try:
        lines = f.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as e:
        return f"ERROR: {e}"
    chunk = "\n".join(lines[offset:offset + limit])
    if len(chunk) > MAX_CHARS:
        chunk = chunk[:MAX_CHARS] + "\n...(truncated)"
    return f"{f} (lines {offset}-{min(offset + limit, len(lines))} of {len(lines)})\n{chunk}"


@mcp.tool()
def search_files(pattern: str, path: str, glob: str = "*", max_results: int = 50) -> str:
    """Regex-search text files under `path` whose names match `glob`. Returns file:line: text."""
    d = _resolve(path)
    rx = re.compile(pattern)
    out: list[str] = []
    for f in d.rglob(glob):
        rel = f.relative_to(d).parts
        if not f.is_file() or any(part.startswith(".") for part in rel):
            continue
        try:
            if f.stat().st_size > 2_000_000:
                continue
            for i, line in enumerate(f.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if rx.search(line):
                    out.append(f"{f}:{i}: {line.strip()[:200]}")
                    if len(out) >= max_results:
                        return "\n".join(out) + "\n...(max results reached)"
        except OSError:
            continue
    return "\n".join(out) or "no matches"


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=str(Path.home()))
    args = ap.parse_args()
    ROOT = Path(args.root).resolve()
    mcp.run(transport="stdio")
