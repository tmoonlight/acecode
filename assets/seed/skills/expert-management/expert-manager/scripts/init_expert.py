#!/usr/bin/env python3
"""
Create an ACECode expert package skeleton in the global expert directory.

Adapted from WorkBuddy expert-manager (Apache-2.0).

Usage:
    python init_expert.py <name> --type agent
    python init_expert.py <name> --type team \
        --lead-expert <id> --member-expert <id>
"""

from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path
from typing import Any


ID_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$")


def get_acecode_home() -> Path:
    configured = os.environ.get("ACECODE_HOME", "").strip()
    return Path(configured).expanduser() if configured else Path.home() / ".acecode"


def get_expert_root() -> Path:
    return get_acecode_home() / "experts"


def valid_id(value: str) -> bool:
    return bool(ID_RE.fullmatch(value or ""))


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def reference_is_agent(root: Path, expert_id: str) -> tuple[bool, str]:
    manifest_path = root / expert_id / "expert.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return False, f"referenced expert does not exist: {expert_id}"
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return False, f"cannot read referenced expert '{expert_id}': {exc}"
    if manifest.get("name") != expert_id:
        return False, f"referenced expert has mismatched name: {expert_id}"
    if manifest.get("expertType") != "agent":
        return False, f"referenced expert is not an Agent: {expert_id}"
    return True, ""


def agent_manifest(name: str) -> dict[str, Any]:
    return {
        "name": name,
        "version": "1.0.0",
        "expertType": "agent",
        "displayName": "[TODO: 展示名称]",
        "profession": "[TODO: 职业定位]",
        "displayDescription": "[TODO: 一句话能力介绍]",
        "quickPrompts": [
            "[TODO: 推荐提问1]",
            "[TODO: 推荐提问2]",
            "[TODO: 推荐提问3]",
        ],
        "defaultInitPrompt": "[TODO: 推荐提问1]",
        "agentName": "lead",
        "agents": [
            {
                "id": "lead",
                "path": "agents/lead.md",
                "displayName": "[TODO: 展示名称]",
                "profession": "[TODO: 职业定位]",
            }
        ],
    }


def agent_document() -> str:
    return """---
name: lead
displayName: [TODO: 展示名称]
profession: [TODO: 职业定位]
---

# [TODO: 职业定位]

你是[TODO: 角色身份和主要职责]。

## 核心职责

- [TODO: 职责1]
- [TODO: 职责2]
- [TODO: 职责3]

## 工作流程

1. [TODO: 步骤1]
2. [TODO: 步骤2]
3. [TODO: 步骤3]

## 输出要求

- [TODO: 输出格式和质量要求]

## 边界

- 不声称拥有未提供的工具、权限或数据。
- 不泄露密钥、私人配置或无关文件内容。
"""


def team_manifest(
    name: str, lead_expert: str, member_experts: list[str]
) -> dict[str, Any]:
    return {
        "name": name,
        "version": "1.0.0",
        "expertType": "team",
        "displayName": "[TODO: 专家团名称]",
        "profession": "[TODO: 团队定位]",
        "displayDescription": "[TODO: 一句话说明团队解决什么问题]",
        "quickPrompts": [
            "[TODO: 推荐提问1]",
            "[TODO: 推荐提问2]",
            "[TODO: 推荐提问3]",
        ],
        "defaultInitPrompt": "[TODO: 推荐提问1]",
        "teamInfo": {
            "leadExpert": lead_expert,
            "memberExperts": member_experts,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name")
    parser.add_argument("--type", choices=("agent", "team"), required=True)
    parser.add_argument("--path", default=str(get_expert_root()))
    parser.add_argument("--lead-expert")
    parser.add_argument("--member-expert", action="append", default=[])
    args = parser.parse_args()

    if not valid_id(args.name):
        parser.error(
            "name must use lowercase letters, digits, and hyphens, "
            "must not start/end with a hyphen, and must be <= 64 chars"
        )

    expected_root = get_expert_root().expanduser().resolve()
    output_root = Path(args.path).expanduser().resolve()
    if output_root != expected_root:
        parser.error(f"--path must be the ACECode global expert root: {expected_root}")
    output_root.mkdir(parents=True, exist_ok=True)

    package = output_root / args.name
    if package.exists():
        parser.error(f"expert already exists; refusing to overwrite: {package}")

    if args.type == "team":
        if not args.lead_expert or not valid_id(args.lead_expert):
            parser.error("Team requires a valid --lead-expert")
        if not args.member_expert:
            parser.error("Team requires at least one --member-expert")
        references = [args.lead_expert, *args.member_expert]
        if len(references) > 32:
            parser.error("Team can contain at most 32 experts")
        if args.name in references:
            parser.error("Team cannot reference itself")
        if len(set(references)) != len(references):
            parser.error("Team references must be unique")
        for expert_id in references:
            if not valid_id(expert_id):
                parser.error(f"invalid referenced expert ID: {expert_id}")
            valid, error = reference_is_agent(output_root, expert_id)
            if not valid:
                parser.error(error)

    package.mkdir()
    try:
        if args.type == "agent":
            agents = package / "agents"
            agents.mkdir()
            write_json(package / "expert.json", agent_manifest(args.name))
            (agents / "lead.md").write_text(agent_document(), encoding="utf-8")
        else:
            write_json(
                package / "expert.json",
                team_manifest(args.name, args.lead_expert, args.member_expert),
            )
    except Exception:
        # Leave the small partial directory visible for diagnosis; never remove
        # an existing package because this function only creates new paths.
        raise

    print(f"Created {args.type} expert skeleton: {package}")
    print("Replace every [TODO], then run validate_expert.py.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
