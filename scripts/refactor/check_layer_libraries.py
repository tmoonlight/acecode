#!/usr/bin/env python3
"""Verify P5 against the generated CMake File API graph and link commands."""
import argparse
import json
from pathlib import Path
import re


LAYERS = (
    "base_core", "base_host", "domain", "adapters", "engine", "host",
    "web", "tui", "headless", "daemon", "cli", "desktop_support",
)
LIBRARY_PATTERN = re.compile(r"(?:lib)?(acecode_[A-Za-z_]+)\.(?:lib|a)(?=\s|$|[\"])")


def load_targets(build):
    reply = build / ".cmake/api/v1/reply"
    indexes = sorted(reply.glob("index-*.json"), key=lambda p: p.stat().st_mtime_ns)
    if not indexes:
        raise ValueError("No CMake File API reply; configure the P5 build first")
    index = json.loads(indexes[-1].read_text(encoding="utf-8"))
    model_ref = index["reply"]["codemodel-v2"]["jsonFile"]
    model = json.loads((reply / model_ref).read_text(encoding="utf-8"))
    configurations = []
    for config in model["configurations"]:
        targets = {}
        for ref in config["targets"]:
            target = json.loads((reply / ref["jsonFile"]).read_text(encoding="utf-8"))
            targets[target["name"]] = target
        configurations.append((config["name"], targets))
    return Path(model["paths"]["source"]), configurations


def source_paths(target, root):
    result = set()
    for entry in target.get("sources", []):
        path = Path(entry["path"])
        if not path.is_absolute():
            path = root / path
        result.add(path.resolve())
    return result


def linked_libraries(target):
    command = " ".join(item["fragment"] for item in target.get("link", {}).get("commandFragments", []))
    if re.search(r"/WHOLEARCHIVE|--whole-archive|-force_load", command, re.IGNORECASE):
        raise ValueError(f"{target['name']}: whole-archive masks library ownership")
    return set(LIBRARY_PATTERN.findall(command))


def verify(root, targets):
    names = {"acecode_" + layer for layer in LAYERS}
    for name in sorted(names):
        if name not in targets or targets[name]["type"] != "STATIC_LIBRARY":
            raise ValueError(f"Missing STATIC layer library: {name}")
    if "acecode_testable" in targets:
        raise ValueError("acecode_testable must not generate objects or an archive")
    expected_tui = {p.resolve() for p in (root / "src/apps/tui").rglob("*.cpp")}
    actual_tui = {p for p in source_paths(targets["acecode_tui"], root) if p.suffix == ".cpp"}
    if not expected_tui or actual_tui != expected_tui:
        raise ValueError(f"TUI coverage differs: missing={expected_tui - actual_tui}, extra={actual_tui - expected_tui}")
    primary = names | {"acecode", "acecode-desktop", "acecode-computer-use",
                       "acecode_computer_use_native", "winpty_static", "acecode_deepin_window_effects"}
    asset_owners = []
    for name in sorted(primary & targets.keys()):
        paths = source_paths(targets[name], root)
        if name != "acecode_tui" and paths & expected_tui:
            raise ValueError(f"TUI implementations also compiled in {name}")
        if any(p.name == "static_assets_data.cpp" for p in paths):
            asset_owners.append(name)
    if asset_owners != ["acecode_web"]:
        raise ValueError(f"Embedded Web assets have wrong owners: {asset_owners}")
    consumers = {name: sorted(linked_libraries(targets[name])) for name in (
        "acecode", "acecode-desktop", "acecode_unit_tests", "concurrent_session_writer",
        "state_file_claim_worker", "remote_web_proxy_test_child") if name in targets}
    required_cli = names - {"acecode_desktop_support"}
    if not required_cli.issubset(consumers.get("acecode", [])):
        raise ValueError("CLI link command does not consume the complete production graph")
    if "acecode_unit_tests" in targets and not names.issubset(consumers["acecode_unit_tests"]):
        raise ValueError("Unit tests do not consume the production archives")
    permitted = {
        "acecode-desktop": {"acecode_base_core", "acecode_desktop_support", "acecode_deepin_window_effects"},
        "concurrent_session_writer": {"acecode_base_core", "acecode_domain"},
        "state_file_claim_worker": {"acecode_base_core"},
    }
    for name, allowed in permitted.items():
        if name in consumers and not set(consumers[name]).issubset(allowed):
            raise ValueError(f"{name} pulls unrelated layers: {set(consumers[name]) - allowed}")
    return {"static_libraries": sorted(names), "tui_sources": len(actual_tui),
            "embedded_asset_owners": asset_owners, "consumer_libraries": consumers}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root, configurations = load_targets(args.build_dir.resolve())
    report = {"source": str(root), "configurations": {
        name: verify(root, targets) for name, targets in configurations}}
    output = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")
    print(output, end="")


if __name__ == "__main__":
    main()
