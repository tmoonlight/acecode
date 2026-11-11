"""Optional ACECode hook for refreshing a graph after a new Git revision."""
import json
from pathlib import Path
import re
import sys

from ua import data_dir, read, revision, write

LABEL = "Understand Anything graph refresh"


def configure(project, enabled):
    path = project / ".acecode/hooks.json"
    config = read(path) if path.exists() else {}
    handlers = config.setdefault("hooks", {})
    # ACECode launches project hooks in the workspace. Keep the registered command project-relative.
    local = project / ".acecode/understand-anything-refresh.py"
    script = Path(__file__).resolve()
    launcher = "# Managed by Understand Anything for ACECode\nimport runpy\nimport sys\n"
    launcher += f"sys.path.insert(0, {str(script.parent)!r})\nrunpy.run_path({str(script)!r}, run_name='__main__')\n"
    if local.exists() and not local.read_text(encoding="utf-8").startswith("# Managed by Understand Anything for ACECode\n"):
        raise ValueError(f"Another file already exists at {local}")
    for event, matcher in (("SessionStart", "*"), ("PostToolUse", "Bash|bash|exec_command|shell")):
        groups = []
        for group in handlers.get(event, []):
            kept = [h for h in group["hooks"] if h.get("statusMessage") != LABEL]
            if kept:
                groups.append(dict(group, hooks=kept))
        if enabled:
            groups.append({"matcher": matcher, "hooks": [{"type": "command", "statusMessage": LABEL,
                "command": "python3 .acecode/understand-anything-refresh.py",
                "commandWindows": "python .acecode/understand-anything-refresh.py", "timeout": 5}]})
        if groups:
            handlers[event] = groups
        else:
            handlers.pop(event, None)
    if enabled:
        local.parent.mkdir(parents=True, exist_ok=True)
        local.write_text(launcher, encoding="utf-8")
    if enabled or path.exists():
        write(path, config)
    if not enabled:
        local.unlink(missing_ok=True)
    settings = data_dir(project) / "config.json"
    current = read(settings) if settings.is_file() else {}
    write(settings, {**current, "autoUpdate": enabled})
    return {"autoUpdate": enabled, "hooks": str(path)}


def event(payload):
    project = Path(payload["cwd"])
    data = data_dir(project)
    opts = data / "config.json"
    if not opts.is_file() or not read(opts).get("autoUpdate") or not (data / "knowledge-graph.json").is_file():
        return
    event_name = payload.get("hook_event_name")
    if event_name == "PostToolUse":
        args = payload.get("tool_input", {})
        command = args.get("command", args.get("cmd", "")) if isinstance(args, dict) else ""
        if not re.search(r"\bgit\s+(?:commit|merge|cherry-pick|rebase|pull)\b", command):
            return
    elif event_name != "SessionStart":
        return
    meta = data / "meta.json"
    commit = revision(project)
    if commit and meta.is_file() and read(meta).get("gitCommitHash") != commit:
        print(json.dumps({"hookSpecificOutput": {"hookEventName": event_name, "additionalContext":
              "This project's Understand Anything graph predates HEAD. Run /understand for an incremental update."}}))


if __name__ == "__main__":
    event(json.load(sys.stdin))
