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


def parse_junit(path: Path) -> dict:
    """CTest `--output-junit`:每个 ctest 条目一个 testcase,名字就是 gtest 名;`<skipped>` 来自
    gtest_discover_tests 的 SKIP_REGULAR_EXPRESSION,`<failure>` 覆盖断言失败、崩溃与超时。"""
    root = ET.parse(path).getroot()
    skipped, failures, executed = [], [], []
    for test in root.iter("testcase"):
        name = test.attrib["name"]
        skip = test.find("skipped")
        failure = test.find("failure")
        if skip is not None:
            skipped.append({"name": name, "reason": skip.get("message", skip.text or "")})
            continue
        if test.get("status") == "run" or failure is not None:
            executed.append(name)
        if failure is not None:
            failures.append({"name": name, "message": failure.get("message", failure.text or "")})
    return {"executed": sorted(executed), "skipped": sorted(skipped, key=lambda r: r["name"]), "failures": sorted(failures, key=lambda r: r["name"])}


def write_run_log(output: str | None, stdout: bytes | None, stderr: bytes | None) -> str | None:
    """完整运行输出落在 <output>-run.log:进程崩溃或挂起时 XML 不会写出,这是唯一的现场。"""
    if not output:
        return None
    log_path = Path(output).with_name(Path(output).stem + "-run.log")
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_bytes((stdout or b"") + b"\n--- stderr ---\n" + (stderr or b""))
    return log_path.name


def run_capturing(command: list[str], timeout: int, output: str | None) -> tuple[subprocess.CompletedProcess, str | None]:
    try:
        result = subprocess.run(command, capture_output=True, timeout=timeout, check=False)
    except subprocess.TimeoutExpired as expired:
        log = write_run_log(output, expired.stdout, expired.stderr)
        raise RuntimeError(f"{command[0]} exceeded {timeout}s; partial output kept in {log}") from expired
    return result, write_run_log(output, result.stdout, result.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary")
    parser.add_argument("--list-file", help="pre-recorded --gtest_list_tests output")
    parser.add_argument("--xml", help="existing gtest XML result")
    parser.add_argument("--run", action="store_true", help="run all cases and capture actual SKIPs")
    parser.add_argument("--via-ctest", action="store_true",
                        help="with --run: execute every registered ctest entry in its own process (crash/hang isolation) and read CTest's JUnit report")
    parser.add_argument("--jobs", type=int, default=1, help="--via-ctest parallelism")
    parser.add_argument("--test-timeout", type=int, default=600, help="--via-ctest per-test timeout in seconds")
    parser.add_argument("--ctest-dir")
    parser.add_argument("--gtest-arg", action="append", default=[])
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--output")
    args = parser.parse_args()
    if not args.binary and not args.list_file:
        parser.error("--binary or --list-file is required")
    if args.run and not args.binary:
        parser.error("--run requires --binary")
    if args.via_ctest and not args.ctest_dir:
        parser.error("--via-ctest requires --ctest-dir")
    if args.list_file:
        listing = Path(args.list_file).read_text(encoding="utf-8")
    else:
        listing = subprocess.run([args.binary, "--gtest_list_tests", *args.gtest_arg], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=args.timeout, check=True).stdout
    report = {"schema": 1, "tests": parse_list(listing), "actual_results": None, "ctest_names": None}
    if args.xml:
        report["actual_results"] = parse_xml(Path(args.xml))
    if args.run:
        with tempfile.TemporaryDirectory(prefix="acecode-gtest-inventory-") as temporary:
            if args.via_ctest:
                junit = Path(temporary) / "junit.xml"
                command = ["ctest", "--test-dir", args.ctest_dir, "-j", str(args.jobs), "--timeout", str(args.test_timeout),
                           "--output-junit", str(junit), "--output-on-failure"]
                report["run_mode"] = "ctest"
                result, log = run_capturing(command, args.timeout, args.output)
                report["run_exit_code"] = result.returncode
                if log:
                    report["run_log"] = log
                if not junit.exists():
                    tail = (result.stdout + result.stderr)[-6000:].decode("utf-8", "replace")
                    raise RuntimeError(f"ctest produced no JUnit report (exit code {result.returncode}); output tail:\n{tail}")
                report["actual_results"] = parse_junit(junit)
            else:
                result_xml = Path(temporary) / "results.xml"
                command = [str(Path(args.binary).resolve()), "--gtest_output=xml:" + str(result_xml), *args.gtest_arg]
                report["run_mode"] = "gtest"
                result, log = run_capturing(command, args.timeout, args.output)
                report["run_exit_code"] = result.returncode
                if log:
                    report["run_log"] = log
                if not result_xml.exists():
                    tail = (result.stdout + result.stderr)[-6000:].decode("utf-8", "replace")
                    raise RuntimeError(f"gtest run produced no XML result (exit code {result.returncode}); output tail:\n{tail}")
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
