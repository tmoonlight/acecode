#!/usr/bin/env python3
"""
Confirm that an ACECode expert is valid and installed in the dynamic scan root.

ACECode does not use WorkBuddy's marketplace.json registration step. This command
keeps the original lifecycle checkpoint while performing a read-only validation.

Adapted from WorkBuddy expert-manager (Apache-2.0).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from validate_expert import get_expert_root, validate_expert


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("expert_dir")
    parser.add_argument("--expert-root", default=str(get_expert_root()))
    args = parser.parse_args()

    package = Path(args.expert_dir).expanduser().resolve()
    root = Path(args.expert_root).expanduser().resolve()
    result = validate_expert(package, expert_root=root, require_installed=True)
    print(result.summary())
    if not result.is_valid:
        return 1

    manifest = json.loads((package / "expert.json").read_text(encoding="utf-8"))
    print(
        f"DISCOVERABLE: '{manifest['name']}' is installed under {root}. "
        "ACECode will show it when the expert list is refreshed."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
