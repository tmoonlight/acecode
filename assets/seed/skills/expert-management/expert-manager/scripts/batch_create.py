#!/usr/bin/env python3
"""
Create complete ACECode expert packages from a JSON batch definition.

Each package is built in a temporary directory, validated, and then moved into
the global expert root. Definitions are processed serially so a later Team can
reference Agent experts created earlier in the same batch.

Adapted from WorkBuddy expert-manager (Apache-2.0).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import tempfile
from pathlib import Path
from typing import Any

from init_expert import valid_id, write_json
from validate_expert import (
    AVATAR_EXTENSIONS,
    MAX_AVATAR_BYTES,
    STATE_AVATAR_KEYS,
    get_expert_root,
    validate_expert,
)


COMMON_INPUT_KEYS = {
    "name",
    "type",
    "version",
    "displayName",
    "profession",
    "displayDescription",
    "quickPrompts",
    "defaultInitPrompt",
    "license",
    "homepage",
    "avatar",
    "stateAvatars",
}
AGENT_INPUT_KEYS = COMMON_INPUT_KEYS | {"instructions", "capabilities"}
TEAM_INPUT_KEYS = COMMON_INPUT_KEYS | {"leadExpert", "memberExperts"}


def require_text(item: dict[str, Any], key: str) -> str:
    value = item.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"'{key}' must be a non-empty string")
    return value.strip()


def quick_prompts(item: dict[str, Any]) -> list[str]:
    values = item.get("quickPrompts", [])
    if not isinstance(values, list) or not values or len(values) > 24:
        raise ValueError("'quickPrompts' must contain 1 to 24 strings")
    prompts = []
    for value in values:
        if not isinstance(value, str) or not value.strip():
            raise ValueError("'quickPrompts' contains an empty or non-string item")
        prompts.append(value.strip())
    return prompts


def copy_optional_metadata(item: dict[str, Any], manifest: dict[str, Any]) -> None:
    for key in ("license", "homepage"):
        if key in item:
            manifest[key] = item[key]


def copy_optional_capabilities(
    item: dict[str, Any], manifest: dict[str, Any]
) -> None:
    if "capabilities" not in item:
        return
    capabilities = item["capabilities"]
    if not isinstance(capabilities, dict):
        raise ValueError("'capabilities' must be an object")
    manifest["capabilities"] = capabilities


def checked_asset_path(asset_base: Path, raw: Any, label: str) -> tuple[Path, Path]:
    if not isinstance(raw, str) or not raw.strip():
        raise ValueError(f"'{label}' must be a non-empty relative image path")
    relative = Path(raw.strip())
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError(f"'{label}' must stay inside the batch asset directory")
    source = (asset_base / relative).resolve()
    try:
        source.relative_to(asset_base.resolve())
    except ValueError as exc:
        raise ValueError(f"'{label}' escapes the batch asset directory") from exc
    if not source.is_file():
        raise ValueError(f"'{label}' file does not exist: {relative.as_posix()}")
    if source.suffix.lower() not in AVATAR_EXTENSIONS:
        raise ValueError(f"'{label}' uses an unsupported image type")
    if source.stat().st_size > MAX_AVATAR_BYTES:
        raise ValueError(f"'{label}' exceeds {MAX_AVATAR_BYTES} bytes")
    return relative, source


def copy_optional_avatar_assets(
    item: dict[str, Any],
    manifest: dict[str, Any],
    package: Path,
    asset_base: Path,
) -> None:
    if "avatar" in item:
        relative, source = checked_asset_path(asset_base, item["avatar"], "avatar")
        destination = package / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        manifest["avatar"] = relative.as_posix()

    if "stateAvatars" not in item:
        return
    state_avatars = item["stateAvatars"]
    if not isinstance(state_avatars, dict):
        raise ValueError("'stateAvatars' must be an object")
    unknown = sorted(set(state_avatars) - set(STATE_AVATAR_KEYS))
    if unknown:
        raise ValueError("unknown 'stateAvatars' keys: " + ", ".join(unknown))
    copied: dict[str, str] = {}
    for state in STATE_AVATAR_KEYS:
        if state not in state_avatars:
            continue
        relative, source = checked_asset_path(
            asset_base, state_avatars[state], f"stateAvatars.{state}"
        )
        destination = package / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        copied[state] = relative.as_posix()
    if copied:
        manifest["stateAvatars"] = copied


def reject_unknown_input_keys(item: dict[str, Any], expert_type: str) -> None:
    allowed = AGENT_INPUT_KEYS if expert_type == "agent" else TEAM_INPUT_KEYS
    unknown = sorted(set(item) - allowed)
    if unknown:
        raise ValueError("unknown expert definition keys: " + ", ".join(unknown))


def build_agent(package: Path, item: dict[str, Any], asset_base: Path) -> None:
    name = require_text(item, "name")
    display_name = require_text(item, "displayName")
    profession = require_text(item, "profession")
    description = require_text(item, "displayDescription")
    instructions = require_text(item, "instructions")
    prompts = quick_prompts(item)
    default_prompt = str(item.get("defaultInitPrompt") or prompts[0]).strip()

    manifest: dict[str, Any] = {
        "name": name,
        "version": str(item.get("version") or "1.0.0"),
        "expertType": "agent",
        "displayName": display_name,
        "profession": profession,
        "displayDescription": description,
        "quickPrompts": prompts,
        "defaultInitPrompt": default_prompt,
        "agentName": "lead",
        "agents": [
            {
                "id": "lead",
                "path": "agents/lead.md",
                "displayName": display_name,
                "profession": profession,
            }
        ],
    }
    copy_optional_metadata(item, manifest)
    copy_optional_capabilities(item, manifest)
    copy_optional_avatar_assets(item, manifest, package, asset_base)

    agents = package / "agents"
    agents.mkdir(parents=True)
    write_json(package / "expert.json", manifest)
    (agents / "lead.md").write_text(
        f"---\nname: lead\n---\n\n# {profession}\n\n{instructions.strip()}\n",
        encoding="utf-8",
    )


def build_team(package: Path, item: dict[str, Any], asset_base: Path) -> None:
    name = require_text(item, "name")
    display_name = require_text(item, "displayName")
    profession = require_text(item, "profession")
    description = require_text(item, "displayDescription")
    lead = require_text(item, "leadExpert")
    members = item.get("memberExperts")
    prompts = quick_prompts(item)

    if "capabilities" in item:
        raise ValueError(
            "Team must not declare capabilities; referenced experts keep their own scopes"
        )
    if not valid_id(lead):
        raise ValueError(f"invalid leadExpert ID: {lead}")
    if not isinstance(members, list) or not members:
        raise ValueError("'memberExperts' must contain at least one expert ID")
    if len(members) + 1 > 32:
        raise ValueError("Team can contain at most 32 experts")
    if any(not isinstance(value, str) or not valid_id(value) for value in members):
        raise ValueError("'memberExperts' contains an invalid expert ID")
    if name in [lead, *members] or len(set([lead, *members])) != len(members) + 1:
        raise ValueError("Team references itself or contains duplicate experts")

    manifest: dict[str, Any] = {
        "name": name,
        "version": str(item.get("version") or "1.0.0"),
        "expertType": "team",
        "displayName": display_name,
        "profession": profession,
        "displayDescription": description,
        "quickPrompts": prompts,
        "defaultInitPrompt": str(item.get("defaultInitPrompt") or prompts[0]).strip(),
        "teamInfo": {
            "leadExpert": lead,
            "memberExperts": members,
        },
    }
    copy_optional_metadata(item, manifest)
    copy_optional_avatar_assets(item, manifest, package, asset_base)
    write_json(package / "expert.json", manifest)


def create_one(
    item: dict[str, Any], root: Path, asset_base: Path | None = None
) -> tuple[bool, str]:
    name = str(item.get("name") or "").strip()
    expert_type = str(item.get("type") or "agent").strip()
    if not valid_id(name):
        return False, f"invalid expert ID: {name!r}"
    if expert_type not in ("agent", "team"):
        return False, f"{name}: type must be 'agent' or 'team'"
    try:
        reject_unknown_input_keys(item, expert_type)
    except ValueError as exc:
        return False, f"{name}: {exc}"
    asset_base = (asset_base or Path.cwd()).expanduser().resolve()

    target = root / name
    if target.exists():
        return False, f"{name}: target already exists; refusing to overwrite"

    staging_parent = root.parent
    staging_parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(
            prefix=f".expert-manager-{name}-", dir=staging_parent
        ) as temporary:
            package = Path(temporary) / name
            package.mkdir()
            if expert_type == "agent":
                build_agent(package, item, asset_base)
            else:
                build_team(package, item, asset_base)

            result = validate_expert(
                package, expert_root=root, require_installed=False
            )
            if not result.is_valid:
                return False, f"{name}: validation failed\n{result.summary()}"

            shutil.move(str(package), str(target))
            installed = validate_expert(target, expert_root=root, require_installed=True)
            if not installed.is_valid:
                return False, f"{name}: installed validation failed\n{installed.summary()}"
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        return False, f"{name}: {exc}"

    return True, f"{name}: created"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("batch_config")
    args = parser.parse_args()

    config_path = Path(args.batch_config).expanduser().resolve()
    config = json.loads(config_path.read_text(encoding="utf-8"))
    if not isinstance(config, dict):
        parser.error("batch config must contain a JSON object")
    unknown_config_keys = sorted(set(config) - {"path", "experts"})
    if unknown_config_keys:
        parser.error("unknown batch config keys: " + ", ".join(unknown_config_keys))

    root = Path(config.get("path") or get_expert_root()).expanduser().resolve()
    expected = get_expert_root().expanduser().resolve()
    if root != expected:
        parser.error(f"batch path must be the ACECode global expert root: {expected}")
    root.mkdir(parents=True, exist_ok=True)

    experts = config.get("experts")
    if not isinstance(experts, list) or not experts:
        parser.error("'experts' must be a non-empty array")

    passed: list[str] = []
    failed: list[str] = []
    for raw in experts:
        if not isinstance(raw, dict):
            failed.append("non-object expert definition")
            continue
        ok, message = create_one(raw, root, config_path.parent)
        print(("PASS: " if ok else "FAIL: ") + message)
        (passed if ok else failed).append(str(raw.get("name") or "<unknown>"))

    print(f"RESULT: {len(passed)} created, {len(failed)} failed")
    if failed:
        print("FAILED IDS: " + ", ".join(failed))
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
