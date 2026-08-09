#!/usr/bin/env python3
"""
Validate an ACECode expert package.

Adapted from WorkBuddy expert-manager (Apache-2.0).

Usage:
    python validate_expert.py <expert-dir>
    python validate_expert.py <expert-dir> --allow-outside-root
"""

from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path
from typing import Any, Optional


MAX_MANIFEST_BYTES = 128 * 1024
MAX_AGENT_BYTES = 256 * 1024
MAX_TEXT_BYTES = 64 * 1024
MAX_QUICK_PROMPTS = 24
MAX_AGENTS = 32
MAX_CAPABILITIES_PER_CLASS = 256
MAX_CAPABILITY_ID_BYTES = 256
CAPABILITY_KEYS = ("skills", "mcp_servers", "tools")
MAX_AVATAR_BYTES = 8 * 1024 * 1024
STATE_AVATAR_KEYS = ("working", "needs_attention", "idle")
AVATAR_EXTENSIONS = (".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".ico")
ID_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$")


def get_acecode_home() -> Path:
    configured = os.environ.get("ACECODE_HOME", "").strip()
    return Path(configured).expanduser() if configured else Path.home() / ".acecode"


def get_expert_root() -> Path:
    return get_acecode_home() / "experts"


def valid_id(value: str) -> bool:
    return bool(ID_RE.fullmatch(value or ""))


def localized_text(value: Any) -> str:
    if isinstance(value, str):
        return value
    if not isinstance(value, dict):
        return ""
    for key in ("zh-CN", "zh_CN", "en-US", "en_US", "default"):
        item = value.get(key)
        if isinstance(item, str):
            return item
    for item in value.values():
        if isinstance(item, str):
            return item
    return ""


class ValidationResult:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []

    def error(self, message: str) -> None:
        self.errors.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)

    @property
    def is_valid(self) -> bool:
        return not self.errors

    def summary(self) -> str:
        lines: list[str] = []
        if self.errors:
            lines.append(f"FAILED: {len(self.errors)} error(s)")
            lines.extend(f"  - {item}" for item in self.errors)
        if self.warnings:
            lines.append(f"WARNINGS: {len(self.warnings)}")
            lines.extend(f"  - {item}" for item in self.warnings)
        if self.is_valid:
            lines.append("PASS: expert package is valid")
        return "\n".join(lines)


def read_json(path: Path, result: ValidationResult, label: str) -> Optional[dict[str, Any]]:
    try:
        if path.stat().st_size > MAX_MANIFEST_BYTES:
            result.error(f"{label} exceeds {MAX_MANIFEST_BYTES} bytes")
            return None
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        result.error(f"{label} not found")
        return None
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        result.error(f"{label} cannot be read as UTF-8 JSON: {exc}")
        return None
    if not isinstance(value, dict):
        result.error(f"{label} must contain a JSON object")
        return None
    return value


def contained_path(
    package_root: Path,
    raw: Any,
    result: ValidationResult,
    label: str,
    require_directory: bool,
) -> Optional[Path]:
    if not isinstance(raw, str) or not raw.strip():
        result.error(f"{label} must be a non-empty relative path")
        return None
    relative = Path(raw)
    if relative.is_absolute():
        result.error(f"{label} must be relative")
        return None
    try:
        root = package_root.resolve()
        target = (package_root / relative).resolve()
        relation = target.relative_to(root)
    except (OSError, ValueError) as exc:
        result.error(f"{label} escapes the expert package: {exc}")
        return None
    if relation == Path("."):
        result.error(f"{label} cannot be the package root")
        return None
    if require_directory and not target.is_dir():
        result.error(f"{label} directory does not exist: {raw}")
        return None
    if not require_directory and not target.is_file():
        result.error(f"{label} file does not exist: {raw}")
        return None
    return target


def parse_agent_document(path: Path, result: ValidationResult, agent_id: str) -> None:
    try:
        if path.stat().st_size > MAX_AGENT_BYTES:
            result.error(f"Agent '{agent_id}' exceeds {MAX_AGENT_BYTES} bytes")
            return
        content = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        result.error(f"Agent '{agent_id}' cannot be read as UTF-8: {exc}")
        return

    body = content.strip()
    frontmatter = ""
    if content.startswith("---"):
        first_end = content.find("\n")
        closing = content.find("\n---", first_end + 1)
        if closing >= 0:
            frontmatter = content[first_end + 1 : closing]
            body = content[closing + 4 :].strip()

    if not body:
        result.error(f"Agent '{agent_id}' has no instruction body")
    if re.search(r"(?m)^\s*(tools|permissions|mcp)\s*:", frontmatter):
        result.error(
            f"Agent '{agent_id}' frontmatter must not declare tools, permissions, or MCP"
        )
    match = re.search(r"(?m)^\s*name\s*:\s*['\"]?([^'\"#\r\n]+)", frontmatter)
    if match and match.group(1).strip() != agent_id:
        result.warn(
            f"Agent '{agent_id}' frontmatter name is '{match.group(1).strip()}'"
        )


def validate_display_fields(manifest: dict[str, Any], result: ValidationResult) -> None:
    display_name = localized_text(manifest.get("displayName"))
    profession = localized_text(manifest.get("profession"))
    description = localized_text(manifest.get("displayDescription"))
    default_prompt = localized_text(manifest.get("defaultInitPrompt"))

    if not display_name:
        result.error("expert.json: displayName is required")
    if not profession:
        result.warn("expert.json: profession is empty")
    if not description:
        result.warn("expert.json: displayDescription is empty")
    if not default_prompt:
        result.warn("expert.json: defaultInitPrompt is empty")

    for label, text, limit in (
        ("displayName", display_name, 512),
        ("profession", profession, 512),
        ("displayDescription", description, MAX_TEXT_BYTES),
        ("defaultInitPrompt", default_prompt, MAX_TEXT_BYTES),
    ):
        if len(text.encode("utf-8")) > limit:
            result.error(f"expert.json: {label} exceeds {limit} UTF-8 bytes")

    prompts = manifest.get("quickPrompts", [])
    if not isinstance(prompts, list) or len(prompts) > MAX_QUICK_PROMPTS:
        result.error(
            f"expert.json: quickPrompts must be an array with at most {MAX_QUICK_PROMPTS} items"
        )
        return
    for index, value in enumerate(prompts):
        text = localized_text(value)
        if not text or len(text.encode("utf-8")) > 4096:
            result.error(f"expert.json: quickPrompts[{index}] is empty or too large")
    if prompts and default_prompt and localized_text(prompts[0]) != default_prompt:
        result.warn("expert.json: first quick prompt differs from defaultInitPrompt")


def validate_avatar_and_skills(
    package_root: Path, manifest: dict[str, Any], result: ValidationResult
) -> None:
    avatar = manifest.get("avatar")
    if avatar is not None:
        contained_path(package_root, avatar, result, "expert.json: avatar", False)

    state_avatars = manifest.get("stateAvatars")
    if state_avatars is not None:
        if not isinstance(state_avatars, dict):
            result.error("expert.json: stateAvatars must be an object")
        else:
            unknown = sorted(set(state_avatars) - set(STATE_AVATAR_KEYS))
            if unknown:
                result.error(
                    "expert.json: unknown stateAvatars keys: " + ", ".join(unknown)
                )
            for state in STATE_AVATAR_KEYS:
                if state not in state_avatars:
                    continue
                path = contained_path(
                    package_root,
                    state_avatars[state],
                    result,
                    f"expert.json: stateAvatars.{state}",
                    False,
                )
                if not path:
                    continue
                if path.suffix.lower() not in AVATAR_EXTENSIONS:
                    result.error(
                        f"expert.json: stateAvatars.{state} uses an unsupported image type"
                    )
                try:
                    if path.stat().st_size > MAX_AVATAR_BYTES:
                        result.error(
                            f"expert.json: stateAvatars.{state} exceeds {MAX_AVATAR_BYTES} bytes"
                        )
                except OSError as exc:
                    result.error(
                        f"expert.json: stateAvatars.{state} size cannot be read: {exc}"
                    )

    skills = manifest.get("skills", [])
    if not isinstance(skills, list):
        result.error("expert.json: skills must be an array")
        return
    for index, raw in enumerate(skills):
        skill_root = contained_path(
            package_root, raw, result, f"expert.json: skills[{index}]", True
        )
        if skill_root and not any(skill_root.rglob("SKILL.md")):
            result.warn(f"expert.json: skills[{index}] contains no SKILL.md")


def validate_capabilities(
    manifest: dict[str, Any], expert_type: str, result: ValidationResult
) -> None:
    if "capabilities" not in manifest:
        return
    capabilities = manifest.get("capabilities")
    if not isinstance(capabilities, dict):
        result.error("expert.json: capabilities must be an object")
        return
    if expert_type == "team":
        result.error(
            "expert.json: Team must not declare capabilities; "
            "referenced experts keep their own scopes"
        )
        return

    unknown = sorted(set(capabilities) - set(CAPABILITY_KEYS))
    if unknown:
        result.warn(
            "expert.json: unknown capabilities keys are ignored by ACECode: "
            + ", ".join(unknown)
        )

    for key in CAPABILITY_KEYS:
        if key not in capabilities:
            continue
        values = capabilities[key]
        if not isinstance(values, list) or len(values) > MAX_CAPABILITIES_PER_CLASS:
            result.error(
                f"expert.json: capabilities.{key} must be an array with at most "
                f"{MAX_CAPABILITIES_PER_CLASS} items"
            )
            continue
        seen: set[str] = set()
        for index, value in enumerate(values):
            if not isinstance(value, str):
                result.error(
                    f"expert.json: capabilities.{key}[{index}] must be a string"
                )
                continue
            normalized = value.strip()
            if (
                not normalized
                or len(normalized.encode("utf-8")) > MAX_CAPABILITY_ID_BYTES
            ):
                result.error(
                    f"expert.json: capabilities.{key}[{index}] is empty or too large"
                )
                continue
            if normalized in seen:
                result.error(
                    f"expert.json: capabilities.{key} contains duplicate "
                    f"'{normalized}'"
                )
                continue
            seen.add(normalized)


def validate_agent(
    package_root: Path, manifest: dict[str, Any], result: ValidationResult
) -> None:
    entries = manifest.get("agents")
    if not isinstance(entries, list) or not entries or len(entries) > MAX_AGENTS:
        result.error(f"expert.json: agents must contain 1 to {MAX_AGENTS} entries")
        return

    seen: set[str] = set()
    declared: list[str] = []
    for index, entry in enumerate(entries):
        if isinstance(entry, str):
            agent_id = Path(entry).stem
            raw_path = entry
        elif isinstance(entry, dict):
            agent_id = localized_text(entry.get("id"))
            raw_path = localized_text(entry.get("path"))
        else:
            result.error(f"expert.json: agents[{index}] must be a path or object")
            continue

        if not valid_id(agent_id):
            result.error(f"expert.json: agents[{index}] has invalid ID '{agent_id}'")
            continue
        if agent_id in seen:
            result.error(f"expert.json: duplicate Agent ID '{agent_id}'")
            continue
        seen.add(agent_id)
        declared.append(agent_id)
        path = contained_path(
            package_root, raw_path, result, f"Agent '{agent_id}' path", False
        )
        if path:
            parse_agent_document(path, result, agent_id)

    lead = localized_text(manifest.get("agentName")) or (declared[0] if declared else "")
    if lead not in declared:
        result.error(f"expert.json: agentName '{lead}' is not declared in agents")


def validate_reference(
    expert_root: Path, reference_id: str, result: ValidationResult
) -> None:
    package = expert_root / reference_id
    manifest = read_json(
        package / "expert.json", result, f"referenced expert '{reference_id}'"
    )
    if not manifest:
        return
    if localized_text(manifest.get("name")) != reference_id:
        result.error(f"referenced expert '{reference_id}' has a mismatched name")
    if localized_text(manifest.get("expertType")) != "agent":
        result.error(f"referenced expert '{reference_id}' is not an Agent expert")


def validate_team(
    package_root: Path,
    manifest: dict[str, Any],
    expert_root: Path,
    result: ValidationResult,
) -> None:
    team_info = manifest.get("teamInfo")
    if not isinstance(team_info, dict):
        result.error("expert.json: Team requires teamInfo")
        return

    lead = localized_text(team_info.get("leadExpert"))
    members_raw = team_info.get("memberExperts")
    if not valid_id(lead):
        result.error(f"expert.json: invalid teamInfo.leadExpert '{lead}'")
    if (
        not isinstance(members_raw, list)
        or not members_raw
        or len(members_raw) + 1 > MAX_AGENTS
    ):
        result.error(
            f"expert.json: memberExperts must contain 1 to {MAX_AGENTS - 1} items"
        )
        return

    team_id = localized_text(manifest.get("name"))
    seen: set[str] = set()
    members: list[str] = []
    for index, raw in enumerate(members_raw):
        member = localized_text(raw)
        if not valid_id(member):
            result.error(f"expert.json: memberExperts[{index}] is invalid")
            continue
        if member in (team_id, lead) or member in seen:
            result.error(
                f"expert.json: member expert '{member}' is self, lead, or duplicated"
            )
            continue
        seen.add(member)
        members.append(member)

    if lead == team_id:
        result.error("expert.json: Team cannot reference itself as lead")
    if "agents" in manifest or "agentName" in manifest:
        result.error(
            "expert.json: reference Team must not copy agents or declare agentName"
        )
    if "skills" in manifest and manifest.get("skills"):
        result.warn("expert.json: Team skills do not replace member experts' own skills")

    if valid_id(lead):
        validate_reference(expert_root, lead, result)
    for member in members:
        validate_reference(expert_root, member, result)


def validate_expert(
    expert_path: Path | str,
    expert_root: Path | str | None = None,
    require_installed: bool = True,
) -> ValidationResult:
    package_root = Path(expert_path).expanduser().resolve()
    root = (
        Path(expert_root).expanduser().resolve()
        if expert_root is not None
        else get_expert_root().resolve()
    )
    result = ValidationResult()

    if not package_root.is_dir():
        result.error(f"expert directory does not exist: {package_root}")
        return result
    if require_installed and package_root.parent != root:
        result.error(f"expert must be installed directly under: {root}")

    manifest = read_json(package_root / "expert.json", result, "expert.json")
    if not manifest:
        return result

    expert_id = localized_text(manifest.get("name"))
    if not valid_id(expert_id):
        result.error(f"expert.json: invalid name '{expert_id}'")
    elif package_root.name != expert_id:
        result.error(
            f"expert directory '{package_root.name}' must match manifest name '{expert_id}'"
        )

    expert_type = localized_text(manifest.get("expertType"))
    if expert_type not in ("agent", "team"):
        result.error("expert.json: expertType must be 'agent' or 'team'")

    version = localized_text(manifest.get("version"))
    if version and len(version) > 128:
        result.error("expert.json: version is too long")

    serialized = json.dumps(manifest, ensure_ascii=False).encode("utf-8")
    if b"[TODO" in serialized:
        result.error("expert.json still contains [TODO] placeholders")
    if "author" in manifest:
        result.warn(
            "expert.json: legacy author metadata is ignored; use NOTICE.md, "
            "license, or homepage for source attribution"
        )

    validate_display_fields(manifest, result)
    validate_avatar_and_skills(package_root, manifest, result)
    validate_capabilities(manifest, expert_type, result)

    if expert_type == "agent":
        validate_agent(package_root, manifest, result)
    elif expert_type == "team":
        validate_team(package_root, manifest, root, result)

    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("expert_dir")
    parser.add_argument("--expert-root", default=str(get_expert_root()))
    parser.add_argument(
        "--allow-outside-root",
        action="store_true",
        help="validate a staging package without requiring installation",
    )
    args = parser.parse_args()

    result = validate_expert(
        args.expert_dir,
        expert_root=args.expert_root,
        require_installed=not args.allow_outside_root,
    )
    print(result.summary())
    return 0 if result.is_valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
