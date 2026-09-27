#!/usr/bin/env python3
"""Record gtest names, actual XML SKIPs, failures and registered ctest names."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET

from repo_files import emit_json


def parse_list(text: str) -> list[str]:
    names, suite = [], ""
    for line in text.splitlines():
        content = line.split("#", 1)[0].rstrip()
        if not content or content.startswith(("Running main()", "Note:")):
            continue
        if not line[0].isspace() and content.endswith("."):
            suite = content
        elif line[0].isspace() and suite:
            names.append(suite + content.strip())
    if not names:
        raise ValueError("gtest --gtest_list_tests returned no test cases")
    return sorted(names)


def parse_xml(path: Path) -> dict:
    root = ET.parse(path).getroot()
    skipped, failures, executed = [], [], []
    for suite in root.iter("testsuite"):
        for test in suite.findall("testcase"):
            name = suite.attrib["name"] + "." + test.attrib["name"]
            if test.get("status") == "run":
                executed.append(name)
            skip = test.find("skipped")
            if skip is not None or test.get("result") == "skipped":
                skipped.append({"name": name, "reason": skip.get("message", skip.text or "") if skip is not None else ""})
            for failure in test.findall("failure"):
                failures.append({"name": name, "message": failure.get("message", failure.text or "")})
    return {"executed": sorted(executed), "skipped": sorted(skipped, key=lambda r: r["name"]), "failures": sorted(failures, key=lambda r: r["name"])}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary")
    parser.add_argument("--list-file", help="pre-recorded --gtest_list_tests output")
    parser.add_argument("--xml", help="existing gtest XML result")
    parser.add_argument("--run", action="store_true", help="run all cases and capture actual SKIPs")
    parser.add_argument("--ctest-dir")
    parser.add_argument("--gtest-arg", action="append", default=[])
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--output")
    args = parser.parse_args()
    if not args.binary and not args.list_file:
        parser.error("--binary or --list-file is required")
    if args.run and not args.binary:
        parser.error("--run requires --binary")
    if args.list_file:
        listing = Path(args.list_file).read_text(encoding="utf-8")
    else:
        listing = subprocess.run([args.binary, "--gtest_list_tests", *args.gtest_arg], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=args.timeout, check=True).stdout
    report = {"schema": 1, "tests": parse_list(listing), "actual_results": None, "ctest_names": None}
    if args.xml:
        report["actual_results"] = parse_xml(Path(args.xml))
    if args.run:
        with tempfile.TemporaryDirectory(prefix="acecode-gtest-inventory-") as temporary:
            result_xml = Path(temporary) / "results.xml"
            result = subprocess.run([str(Path(args.binary).resolve()), "--gtest_output=xml:" + str(result_xml), *args.gtest_arg], capture_output=True, timeout=args.timeout, check=False)
            report["run_exit_code"] = result.returncode
            report["actual_results"] = parse_xml(result_xml)
    if args.ctest_dir:
        output = subprocess.run(["ctest", "--test-dir", args.ctest_dir, "--show-only=json-v1"], capture_output=True, text=True, encoding="utf-8", timeout=args.timeout, check=True)
        report["ctest_names"] = sorted(test["name"] for test in json.loads(output.stdout)["tests"])
    emit_json(report, args.output)
    # Baseline failures are evidence, not a capture failure. Missing XML, timeout
    # and an empty inventory do fail rather than fabricating a green baseline.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
