"""Generate reproducible synthetic session data in a NEW isolated profile."""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import uuid


def encode(value):
    return (json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


def project_hash(cwd):
    value = str(cwd.resolve()).replace("\\", "/").lower().rstrip("/")
    result = 14695981039346656037
    for byte in value.encode("utf-8"):
        result = ((result ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{result:016x}"


def identity(label):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "acecode-session-bench/" + label))


def category(record):
    subtype = record.get("subtype", "")
    if subtype in ("compact_checkpoint", "file_checkpoint"):
        return subtype
    if "turn_net_diff" in record.get("metadata", {}):
        return "turn_net_diff"
    return {"user": "user", "assistant": "assistant", "tool": "tool"}.get(record.get("role"), "other")


def records(session_id, turn, tool_bytes, diff_bytes, checkpoint_every):
    user_id = identity(f"{session_id}/user/{turn}")
    stamp = (datetime(2026, 1, 1, tzinfo=timezone.utc) + timedelta(seconds=turn)).isoformat()
    common = {"timestamp": stamp}
    yield dict(common, role="user", uuid=user_id, content=f"Synthetic request {turn}")
    yield dict(common, role="system", is_meta=True, subtype="file_checkpoint",
               uuid=identity(f"{session_id}/file/{turn}"),
               content="[File checkpoint]", metadata={"message_uuid": user_id, "tracked_files": {}})
    call_id = f"bench-call-{turn}"
    yield dict(common, role="assistant", content=f"Inspecting synthetic sample {turn}.",
               tool_calls=[{"id": call_id, "type": "function", "function":
                            {"name": "shell", "arguments": '{"command":"echo synthetic"}'}}])
    yield dict(common, role="tool", tool_call_id=call_id,
               content=("synthetic tool output\n" * ((tool_bytes // 22) + 1))[:tool_bytes])
    yield dict(common, role="assistant", content=f"Completed synthetic request {turn}.")
    yield dict(common, role="system", content="[Turn net diff]",
               metadata={"transcript_only": True, "turn_net_diff": {
                   "user_message_uuid": user_id, "complete": True, "errors": [],
                   "files": [{"file": "synthetic.txt", "additions": 1, "deletions": 0,
                              "hunks": [{"old_start": 1, "old_count": 0, "new_start": 1,
                                         "new_count": 1, "lines": [{"kind": "added",
                                         "text": "x" * diff_bytes, "new_line_no": 1}]}]}]}})
    if checkpoint_every and (turn + 1) % checkpoint_every == 0:
        window = (turn + 1) // checkpoint_every
        # Bounded replacement history: never embed the prior transcript.
        summary = {"role": "user", "content": "Synthetic compact summary. " * 512}
        yield dict(common, role="system", is_meta=True, subtype="compact_checkpoint",
                   content="[Compact checkpoint]", uuid=identity(f"{session_id}/compact/{turn}"),
                   metadata={"version": 2, "window_number": window,
                             "window_id": f"window-{window}", "first_window_id": "window-1",
                             "previous_window_id": f"window-{window - 1}" if window > 1 else "",
                             "replacement_history": [summary], "summary": summary["content"]})


def write_session(project, cwd, sid, target_bytes, tool_bytes, diff_bytes, checkpoint_every, ordinal):
    counts, sizes = Counter(), Counter()
    turns, total = 0, 0
    transcript = project / (sid + ".jsonl")
    with transcript.open("xb") as output:
        while total < target_bytes or turns == 0:
            for record in records(sid, turns, tool_bytes, diff_bytes, checkpoint_every):
                raw = encode(record)
                output.write(raw)
                key = category(record)
                counts[key] += 1
                sizes[key] += len(raw)
                total += len(raw)
            turns += 1
    stamp = (datetime(2026, 1, 1, tzinfo=timezone.utc) + timedelta(seconds=ordinal)).isoformat()
    meta = {"id": sid, "cwd": str(cwd), "title": f"Synthetic session {ordinal}",
            "summary": "Synthetic benchmark fixture", "created_at": stamp, "updated_at": stamp,
            "message_count": sum(counts.values()), "turn_count": turns,
            "provider": "openai", "model": "benchmark", "model_preset": "benchmark",
            "permission_mode": "default"}
    (project / (sid + ".meta.json")).write_bytes(encode(meta))
    return {"id": sid, "bytes": total, "turns": turns, "records": dict(counts), "bytes_by_type": dict(sizes)}


def generate(profile, sessions=1500, large_mib=(100,), tool_bytes=32768, diff_bytes=4096, checkpoint_every=50):
    profile = Path(profile).resolve()
    if profile.exists():
        raise ValueError("Profile must not exist; refusing to overwrite existing data")
    if sessions < len(large_mib) or sessions < 1 or min(large_mib, default=1) <= 0:
        raise ValueError("Invalid session count or large-session size")
    profile.mkdir(parents=True)
    cwd = profile / "workspace"
    cwd.mkdir()
    root = profile / ".acecode"
    project = root / "projects" / project_hash(cwd)
    project.mkdir(parents=True)
    (project / "workspace.json").write_bytes(encode({"cwd": str(cwd), "name": "Session benchmark", "desktop_visible": True}))
    # Loopback-only dummy model; the benchmark never sends a user turn.
    (root / "config.json").write_bytes(encode({
        "default_model_name": "benchmark",
        "saved_models": [{"name": "benchmark", "provider": "openai", "model": "benchmark",
                          "api_key": "synthetic-not-a-secret", "base_url": "http://127.0.0.1:1/v1"}],
        "web": {"enabled": True, "host": "127.0.0.1"},
        "logging": {"level": "debug"},
    }))
    entries = []
    for index in range(sessions):
        sid = f"20260101-{index // 3600:02d}{index // 60 % 60:02d}{index % 60:02d}-{index:04x}"
        target = int(large_mib[index] * 1024 * 1024) if index < len(large_mib) else 1
        entries.append(write_session(project, cwd, sid, target,
                                     tool_bytes if target > 1 else 128,
                                     diff_bytes if target > 1 else 64, checkpoint_every, sessions - index))
    manifest = {"schema": 1, "synthetic": True, "profile": str(profile), "cwd": str(cwd),
                "project_hash": project.name, "parameters": {"sessions": sessions, "large_mib": list(large_mib),
                "tool_bytes": tool_bytes, "diff_bytes": diff_bytes, "checkpoint_every": checkpoint_every},
                "sessions": entries}
    (profile / "manifest.json").write_bytes(encode(manifest))
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--sessions", type=int, default=1500)
    parser.add_argument("--large-mib", type=float, nargs="+", default=[100])
    parser.add_argument("--tool-bytes", type=int, default=32768)
    parser.add_argument("--diff-bytes", type=int, default=4096)
    parser.add_argument("--checkpoint-every", type=int, default=50)
    args = parser.parse_args()
    result = generate(args.profile, args.sessions, args.large_mib, args.tool_bytes, args.diff_bytes, args.checkpoint_every)
    print(json.dumps({"profile": result["profile"], "session_count": len(result["sessions"]),
                      "large_sessions": result["sessions"][:len(args.large_mib)]}, indent=2))
