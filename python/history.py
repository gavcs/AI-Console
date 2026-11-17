#!/usr/bin/env python3
"""The saved form of a conversation, shared by both chat backends.

A conversation is one JSON file, written by whichever backend is answering
(agent.py for a local model, claude_agent.py for Claude Code) and read by the
app to show the transcript again:

    {"version": 1, "messages": [ ... ]}

`messages` is the OpenAI chat format (user / assistant with optional
tool_calls / tool), which is what a local model is sent, so a conversation can
be picked up later, by a different model, or with different tools. The system
prompt is not stored; it belongs to the session, not to the conversation.

Keys that start with "_" are ours: kept in the file, never sent to a model.
    user:       "_text"         what the user typed (content may also hold inlined attachments)
                "_attachments"  [{"name", "path", "kind"}]
    assistant:  "_model"        who answered, as shown in the transcript
                "_thinking"     the reasoning that came with this message
                "_thinking_hidden"  the model reasoned, but the text was not provided
                "_stopped"      the user stopped this reply; content is what had been written
                "_error"        this is not an answer: the turn failed or was cut short

Needs only the standard library.
"""
from __future__ import annotations

import json
import os
from typing import Any

Message = dict[str, Any]


# --------------------------------------------------------------------------- building messages

def attachment_text(a: dict[str, Any]) -> str:
    """A text/binary attachment rendered as text."""
    name = a.get("name", "file")
    if a.get("kind") == "text":
        note = " (truncated)" if a.get("truncated") else ""
        return f"Attached file `{name}`{note}:\n\n```\n{a.get('text', '')}\n```"
    mime = a.get("mime", "application/octet-stream")
    size = a.get("size")
    size_s = f"{size} bytes" if isinstance(size, int) else "unknown size"
    where = f" is at {a['path']}" if a.get("path") else ""
    return (f"[Attached file `{name}` ({mime}, {size_s}){where}. "
            "Binary contents are not shown; use a tool to open it if needed.]")


def openai_user_content(text: str, attachments: list[dict[str, Any]] | None) -> Any:
    """A plain string unless an image is attached, in which case OpenAI content parts."""
    attachments = attachments or []
    if not attachments:
        return text
    has_image = any(a.get("kind") == "image" and a.get("data_url") for a in attachments)
    if not has_image:
        # The most widely supported shape for text-only input is one string.
        chunks = ([text] if text else []) + [attachment_text(a) for a in attachments]
        return "\n\n".join(chunks)
    parts: list[dict[str, Any]] = []
    if text:
        parts.append({"type": "text", "text": text})
    for a in attachments:
        if a.get("kind") == "image" and a.get("data_url"):
            parts.append({"type": "image_url", "image_url": {"url": a["data_url"]}})
        else:
            parts.append({"type": "text", "text": attachment_text(a)})
    return parts


def user_message(text: str, attachments: list[dict[str, Any]] | None) -> Message:
    """The user's turn as it is stored (and, minus the "_" keys, sent to a local model)."""
    msg: Message = {"role": "user", "content": openai_user_content(text, attachments), "_text": text}
    if attachments:
        msg["_attachments"] = [{"name": a.get("name", ""), "path": a.get("path", ""), "kind": a.get("kind", "")}
                               for a in attachments]
    return msg


STOPPED = "stopped by the user"


def close_tool_calls(messages: list[Message], why: str) -> list[Message]:
    """Results to add for tool calls at the end of `messages` that never got one."""
    for i in range(len(messages) - 1, -1, -1):
        m = messages[i]
        if m.get("role") == "tool":
            continue
        if m.get("role") == "assistant" and m.get("tool_calls"):
            done = {t.get("tool_call_id") for t in messages[i + 1:]}
            return [{"role": "tool", "tool_call_id": c.get("id"), "content": f"ERROR: {why}."}
                    for c in m["tool_calls"] if c.get("id") not in done]
        break
    return []


def stopped_message(partial: str, extras: dict[str, Any]) -> Message:
    """The assistant message that records a reply the user stopped."""
    if partial:
        return {"role": "assistant", "content": partial, **extras, "_stopped": True}
    return {"role": "assistant", "content": "[The user stopped this reply before anything was written.]",
            **extras, "_stopped": True, "_error": STOPPED}


def for_model(messages: list[Message]) -> list[Message]:
    """`messages` without our private keys."""
    return [{k: v for k, v in m.items() if not k.startswith("_")} for m in messages]


# --------------------------------------------------------------------------- the file

def _repair(messages: list[Message]) -> list[Message]:
    """Make a conversation that was cut off mid-turn valid to continue.

    The file is written after every message, so a backend that was stopped in
    the middle of a reply (the app quit, the settings were changed) leaves a
    tool call without its result or a question without an answer.
    """
    out: list[Message] = []
    i = 0
    while i < len(messages):
        m = messages[i]
        out.append(m)
        i += 1
        if m.get("role") == "assistant" and m.get("tool_calls"):
            answered = set()
            while i < len(messages) and messages[i].get("role") == "tool":
                answered.add(messages[i].get("tool_call_id"))
                out.append(messages[i])
                i += 1
            for call in m["tool_calls"]:
                if call.get("id") not in answered:
                    out.append({"role": "tool", "tool_call_id": call.get("id"),
                                "content": "ERROR: interrupted before this tool call finished."})
    if out and out[-1].get("role") in ("user", "tool"):
        out.append({"role": "assistant", "content": "[No reply: the session was interrupted here.]",
                    "_error": "interrupted before a reply was finished"})
    return out


class History:
    """The messages of one saved conversation. `path` empty = keep nothing on disk."""

    def __init__(self, path: str = "") -> None:
        self.path = path
        self.messages: list[Message] = []
        if path and os.path.exists(path):
            try:
                with open(path, encoding="utf-8") as f:
                    raw = json.load(f)
                loaded = raw.get("messages", []) if isinstance(raw, dict) else []
            except (OSError, ValueError):
                loaded = []
            self.messages = _repair([m for m in loaded if isinstance(m, dict)])
            if self.messages != loaded:
                self.save()

    def append(self, msg: Message) -> None:
        self.messages.append(msg)
        self.save()

    def clear(self) -> None:
        self.messages = []
        self.save()

    def save(self) -> None:
        if not self.path:
            return
        tmp = self.path + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump({"version": 1, "messages": self.messages}, f)
            os.replace(tmp, self.path)      # readers never see a half-written file
        except OSError:
            pass


# --------------------------------------------------------------------------- as plain text

def _text_of(content: Any) -> str:
    if isinstance(content, list):
        return "\n".join(p.get("text", "") if p.get("type") == "text" else "[image]"
                         for p in content if isinstance(p, dict))
    return content if isinstance(content, str) else ""


def transcript(messages: list[Message], max_chars: int = 150000, max_tool_chars: int = 3000) -> str:
    """The conversation as text, for a backend that cannot be handed the messages themselves.

    Long tool results are shortened; if it is still too long the oldest part goes.
    """
    lines: list[str] = []
    for m in messages:
        role = m.get("role")
        if role == "user":
            lines.append("User: " + _text_of(m.get("content")))
        elif role == "assistant":
            if m.get("_error"):
                lines.append(f"(No reply to the previous message: {m['_error']}.)")
                continue
            if _text_of(m.get("content")):
                lines.append("Assistant: " + _text_of(m.get("content"))
                             + (" [the user stopped the reply here]" if m.get("_stopped") else ""))
            for call in m.get("tool_calls") or []:
                fn = call.get("function") or {}
                lines.append(f"Assistant called the tool {fn.get('name')} with {fn.get('arguments')}")
        elif role == "tool":
            text = _text_of(m.get("content"))
            if len(text) > max_tool_chars:
                text = text[:max_tool_chars] + f" ...[{len(text) - max_tool_chars} more characters not shown]"
            lines.append("Tool result: " + text)
    text = "\n\n".join(lines)
    if len(text) > max_chars:
        text = "[The beginning of the conversation is not shown.]\n\n" + text[-max_chars:]
    return text
