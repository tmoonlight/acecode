"""Windows 本机的快速单测运行器(design.md D26 / §7.4)。

一次全量单测串行要 6.5 分钟(5114 条,2026-09-28 本机实测),其中 20 个套件占 77% 的时间。
这里做三件事,把逐任务验证压到 1 分钟上下:

1. 分片并行:用 gtest 自带的 GTEST_TOTAL_SHARDS / GTEST_SHARD_INDEX 把用例切成 N 个进程同时跑;
2. 隔离:每个分片各有一套 HOME / USERPROFILE / APPDATA / LOCALAPPDATA / TEMP / TMP,放在仓库之外,
   分片之间不共享 state.json、projects 目录与临时文件。目录必须短、且不能经过 junction:
   默认放在二进制所在盘的根下(如 N:/acecode-gtest-iso/121331/s0),试跑时放在
   C:/Users/<user>/AppData/Local/Temp 下曾同时踩中两个坑 —— 种子安装的深层路径超过 MAX_PATH
   (create_directory_failed: 文件名或扩展名太长),以及 C:/Users/<user> 是 N: 的 junction,
   weakly_canonical 后盘符变化让 expert_registry 的越界路径判定失效;
3. 快速档(默认):排除 FAST_EXCLUDED_SUITES 里靠固定等待与真实超时撑起来的慢套件,
   但 design.md §7.3 的守护测试(INVARIANT_GUARDS)无论档位都强制运行;
   需要独占全局资源的套件(SERIAL_SUITES,如桌面单实例的命名互斥体)在分片之后单独串行跑。

输出 JSON 与 gtest_inventory.py 同一 schema(tests / actual_results / run_exit_code / run_mode),
另外记下 profile、被排除而没有运行的用例(not_run)与每个进程的耗时,可以直接交给
compare_snapshots.py --gtest-before / --gtest-after 对照。

用法:
  python scripts/refactor/run_fast_tests.py --binary build-p2/tests/acecode_unit_tests.exe
      [--profile fast|full] [--shards 6] [--filter "HookAgentLoop.*:SkillCommandsReloadTest.*"]
      [--baseline <上一条记录的 gtest.json>] [--output out.json] [--iso-root DIR] [--clean]

退出码:0 = 没有失败(带 --baseline 时是没有新增失败);1 = 有失败、进程没产出结果,
或带 --require-same-inventory 时用例清单有增删。清单增删默认只警告。
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gtest_inventory import parse_list, parse_xml  # noqa: E402

# 本机 Windows 串行实测(2026-09-28,MSVC 2022 Release,5114 条共 390 秒)的 20 个最慢套件,
# 合计约 300 秒(77%)。它们靠固定 sleep、真实网络超时与子进程撑时间,和目录搬迁几乎无关;
# 改动直接命中这些模块时用 --filter 正向补跑对应套件。值是各套件的秒数,只作记录。
FAST_EXCLUDED_SUITES = {
    "SessionChannelBinderIntegration": 139.1,
    "WebServerHttp": 34.0,
    "OpenAiProviderErrorRecovery": 16.4,
    "McpManagerAsync": 15.0,
    "AgentLoopTermination": 9.7,
    "SpawnSubagentTool": 9.2,
    "HeadlessJsonlProcess": 8.9,
    "AgentLoopGoal": 7.7,
    "AgentLoopTurnSteering": 7.2,
    "OpenAiProviderAbortTest": 6.1,
    "GitOpsTest": 5.7,
    "BuiltinCommands": 5.2,
    "HookAgentLoop": 5.1,
    "WorktreeGitTest": 4.9,
    "GitContextCollectorTest": 4.7,
    "ExpertRegistry": 4.5,
    "WorktreeToolTest": 4.3,
    "RemoteControlService": 4.3,
    "DefaultSkillSeederTest": 4.3,
    "TaskSuggestionServiceTest": 4.2,
}

# design.md §7.3 第 1 条的守护测试:prompt cache 前缀字节稳定。所在套件被快速档排除时也单独跑。
INVARIANT_GUARDS = [
    "AgentLoopTermination.RequestPrefixIsByteStableAcrossIterationsInATurn",
    "SystemPromptTest.StaticSystemPromptIsByteStableAcrossCalls",
    "SystemPromptTest.SessionContextIsByteStableForUnchangedInputs",
    "SystemPromptTest.VisionEnvironmentLineIsByteStableForSameCapability",
    "SystemPromptTest.EnvironmentLinesAreByteStable",
    "SystemPromptTest.NonGptModelStateIsByteIdenticalToLegacyPrompt",
    "SystemPromptTest.GptPromptIsByteStableAcrossCalls",
]

# 独占机器级资源、不能和别的分片同时跑的套件,分片结束后单独串行跑(快速档排除表优先于这里):
# - DesktopSingleInstance:命名互斥体;
# - SessionChannelBinderIntegration:每条用例绑定固定端口 286xx,两个分片同时跑就报 already in use;
# - ComputerUsePointerOverlay:创建真实顶层窗口做命中测试,并发时互相遮挡。
# - ManagedRemoteWebProxy:启动真实子进程并等它从 127.0.0.2 回连,6 路并发时 2 秒内等不到,单独跑稳定通过。
# - RemoteControlService:next_port() 是从 28411 起的进程内固定序列,两个分片各自从头数就撞端口。
SERIAL_SUITES = ["DesktopSingleInstance", "SessionChannelBinderIntegration", "ComputerUsePointerOverlay",
                 "ManagedRemoteWebProxy", "RemoteControlService"]

ISOLATED_ENV_KEYS = ("HOME", "USERPROFILE", "APPDATA", "LOCALAPPDATA", "TEMP", "TMP")


def suite_of(name: str) -> str:
    return name.split(".", 1)[0]


def matches_any(name: str, patterns: list[str]) -> bool:
    """gtest 的 --gtest_filter 通配符只有 * 和 ?,与 fnmatch 语义一致。"""
    return any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)


def build_shard_filter(profile: str, positive: str | None, serial: list[str]) -> str:
    """分片进程用的 --gtest_filter:正向部分默认 *,负向部分是快速档排除的套件加串行套件。

    给了显式 --filter 就只排除串行套件:点名要跑的慢套件(比如改了 hooks 之后补跑 HookAgentLoop)
    必须真的跑,否则「--filter 正向补跑」这条路就是假的。
    """
    excluded = set(serial)
    if profile == "fast" and not positive:
        excluded |= set(FAST_EXCLUDED_SUITES)
    negatives = ":".join(f"{suite}.*" for suite in sorted(excluded))
    head = positive or "*"
    return f"{head}-{negatives}" if negatives else head


def partition_inventory(tests: list[str], profile: str, positive: str | None,
                        serial: list[str], guards: list[str]) -> dict:
    """把清单分成四组:分片跑的、串行跑的、单独补跑的守护测试、这次不运行的。

    positive 是 --filter 的正向模式(冒号分隔),没有就是全部。not_run 只在快速档非空:
    它是「被排除的慢套件里、又不是守护测试」的用例,记录里必须如实列出,不能当作 SKIP。
    """
    positives = (positive or "*").split(":")
    excluded = set(FAST_EXCLUDED_SUITES) if profile == "fast" and not positive else set()
    serial_set, guard_set = set(serial), set(guards)
    sharded, serial_tests, guard_tests, not_run = [], [], [], []
    for name in tests:
        suite = suite_of(name)
        if not matches_any(name, positives):
            if name in guard_set:
                guard_tests.append(name)
            continue
        if suite in excluded:
            # 排除优先于串行:快速档下 139 秒的 channel binder 集成测试不能借串行阶段溜回来。
            (guard_tests if name in guard_set else not_run).append(name)
        elif suite in serial_set:
            serial_tests.append(name)
        else:
            sharded.append(name)
    return {"sharded": sharded, "serial": serial_tests, "guards": guard_tests, "not_run": not_run}


def isolated_env(base: dict, root: Path) -> dict:
    home = root / "home"
    for sub in (home / "AppData" / "Roaming", home / "AppData" / "Local", root / "tmp"):
        sub.mkdir(parents=True, exist_ok=True)
    env = dict(base)
    values = {
        "HOME": home, "USERPROFILE": home,
        "APPDATA": home / "AppData" / "Roaming", "LOCALAPPDATA": home / "AppData" / "Local",
        "TEMP": root / "tmp", "TMP": root / "tmp",
    }
    for key in ISOLATED_ENV_KEYS:
        env[key] = str(values[key])
    return env


def run_process(binary: Path, args: list[str], env: dict, cwd: Path, log_path: Path, timeout: int) -> dict:
    started = time.monotonic()
    with open(log_path, "wb") as log:
        try:
            completed = subprocess.run([str(binary), *args], cwd=str(cwd), env=env,
                                       stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
            code = completed.returncode
        except subprocess.TimeoutExpired:
            code = -1
    return {"exit_code": code, "seconds": round(time.monotonic() - started, 1), "log": str(log_path)}


def merge_results(xml_paths: list[Path]) -> dict:
    executed, skipped, failures = set(), {}, {}
    for path in xml_paths:
        part = parse_xml(path)
        executed.update(part["executed"])
        for row in part["skipped"]:
            skipped.setdefault(row["name"], row)
        for row in part["failures"]:
            failures.setdefault(row["name"], row)
    return {"executed": sorted(executed),
            "skipped": sorted(skipped.values(), key=lambda r: r["name"]),
            "failures": sorted(failures.values(), key=lambda r: r["name"])}


def compare_with_baseline(report: dict, baseline: dict) -> dict:
    """和上一条记录比:清单增删;新增失败;SKIP 变化只看这次真的执行了的用例(没跑 ≠ SKIP)。"""
    def names(doc: dict, key: str) -> set[str]:
        return {row["name"] for row in ((doc.get("actual_results") or {}).get(key) or [])}
    before, after = set(baseline.get("tests") or []), set(report.get("tests") or [])
    executed = set(report["actual_results"]["executed"])
    old_failed, new_failed = names(baseline, "failures"), names(report, "failures")
    old_skipped, new_skipped = names(baseline, "skipped"), names(report, "skipped")
    return {
        "tests_added": sorted(after - before),
        "tests_removed": sorted(before - after),
        "failures_new": sorted(new_failed - old_failed),
        "failures_fixed": sorted(name for name in old_failed - new_failed if name in executed),
        "failures_not_rerun": sorted(name for name in old_failed if name not in executed),
        "skipped_added": sorted(new_skipped - old_skipped),
        "skipped_removed": sorted(name for name in old_skipped - new_skipped if name in executed),
    }


def default_iso_root(binary: Path) -> Path:
    """短路径、不经 junction:Windows 放在二进制所在盘的根下,其它平台放系统临时目录。"""
    stamp = time.strftime("%H%M%S")
    if os.name == "nt":
        return Path(binary.anchor) / "acecode-gtest-iso" / stamp
    return Path(tempfile.gettempdir()) / "acecode-gtest-iso" / f"{stamp}-{os.getpid()}"


def default_cwd(binary: Path) -> Path:
    for candidate in [binary.parent, *binary.parents]:
        if (candidate / "CMakeLists.txt").exists() and (candidate / "src").is_dir():
            return candidate
    return binary.parent


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--profile", choices=("fast", "full"), default="fast")
    parser.add_argument("--shards", type=int, default=6)
    parser.add_argument("--filter", help="正向 gtest 模式(冒号分隔),只跑这些;守护测试仍会补跑")
    parser.add_argument("--baseline", help="上一条记录的 gtest JSON,用来报告清单 / 失败 / SKIP 的变化")
    parser.add_argument("--require-same-inventory", action="store_true", help="清单有增删时也以非零退出")
    parser.add_argument("--output")
    parser.add_argument("--cwd", help="运行目录,默认是二进制所在仓库的根")
    parser.add_argument("--iso-root", help="隔离的 HOME/TEMP 与日志根目录,默认在系统临时目录下")
    parser.add_argument("--clean", action="store_true", help="结束后删除隔离目录(默认保留供排查)")
    parser.add_argument("--timeout", type=int, default=1800, help="每个进程的秒数上限")
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.exists():
        parser.error(f"binary not found: {binary}")
    cwd = Path(args.cwd).resolve() if args.cwd else default_cwd(binary)
    iso_root = Path(args.iso_root).resolve() if args.iso_root else default_iso_root(binary)
    iso_root.mkdir(parents=True, exist_ok=True)

    listing = subprocess.run([str(binary), "--gtest_list_tests"], capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=args.timeout, check=True, cwd=str(cwd)).stdout
    tests = parse_list(listing)
    parts = partition_inventory(tests, args.profile, args.filter, SERIAL_SUITES, INVARIANT_GUARDS)

    jobs: list[dict] = []
    wall_started = time.monotonic()

    def launch(label: str, index: int, filter_text: str, shard_env: dict | None = None) -> dict:
        root = iso_root / label
        env = isolated_env(os.environ, root)
        if shard_env:
            env.update(shard_env)
        xml = root / "results.xml"
        result = run_process(binary, [f"--gtest_filter={filter_text}", f"--gtest_output=xml:{xml}"],
                             env, cwd, root / "run.log", args.timeout)
        result.update({"label": label, "index": index, "filter": filter_text, "xml": str(xml) if xml.exists() else None})
        return result

    if parts["sharded"]:
        shard_filter = build_shard_filter(args.profile, args.filter, SERIAL_SUITES)
        with ThreadPoolExecutor(max_workers=args.shards) as pool:
            futures = [pool.submit(launch, f"s{i}", i, shard_filter,
                                   {"GTEST_TOTAL_SHARDS": str(args.shards), "GTEST_SHARD_INDEX": str(i)})
                       for i in range(args.shards)]
            jobs.extend(future.result() for future in futures)
    shards_wall = round(time.monotonic() - wall_started, 1)
    if parts["serial"]:
        jobs.append(launch("serial", -1, ":".join(parts["serial"])))
    if parts["guards"]:
        jobs.append(launch("guards", -2, ":".join(parts["guards"])))
    wall = round(time.monotonic() - wall_started, 1)

    missing = [job["label"] for job in jobs if not job["xml"]]
    actual = merge_results([Path(job["xml"]) for job in jobs if job["xml"]]) if jobs else \
        {"executed": [], "skipped": [], "failures": []}
    # GTest repeat mode writes only the final iteration to XML, while the
    # process keeps a failure exit from any earlier iteration. Crashes/timeouts
    # can also leave a previous XML behind. Only ordinary assertion failures
    # represented in this process's XML may use the known-baseline allowance.
    unexpected_exits = [job for job in jobs if job["exit_code"] != 0 and
                        (job["exit_code"] != 1 or not job["xml"] or
                         not parse_xml(Path(job["xml"]))["failures"])]
    report = {
        "schema": 1,
        "tests": tests,
        "actual_results": actual,
        "ctest_names": None,
        "run_mode": "gtest-shards",
        "profile": args.profile,
        "filter": args.filter,
        "shards": args.shards,
        "excluded_suites": sorted(FAST_EXCLUDED_SUITES) if args.profile == "fast" else [],
        "not_run": parts["not_run"],
        "jobs": jobs,
        "wall_seconds": wall,
        "shards_wall_seconds": shards_wall,
        "run_exit_code": next((job["exit_code"] for job in jobs if job["exit_code"] != 0), 0),
        "binary": str(binary),
        "cwd": str(cwd),
        "iso_root": str(iso_root),
    }
    comparison = None
    if args.baseline:
        comparison = compare_with_baseline(report, json.loads(Path(args.baseline).read_text(encoding="utf-8")))
        report["baseline"] = str(Path(args.baseline).resolve())
        report["comparison"] = comparison
    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(f"profile={args.profile} shards={args.shards} inventory={len(tests)} "
          f"executed={len(actual['executed'])} skipped={len(actual['skipped'])} failed={len(actual['failures'])} "
          f"not_run={len(parts['not_run'])} wall={wall}s (shards {shards_wall}s)")
    for job in jobs:
        print(f"  {job['label']:>8}: exit={job['exit_code']} {job['seconds']}s")
    for row in actual["failures"]:
        print(f"  FAILED {row['name']}")
    problems = [f"{job['label']}: unexplained process exit {job['exit_code']}"
                for job in unexpected_exits]
    if missing:
        problems.append(f"no XML from: {', '.join(missing)}")
    if comparison:
        for key in ("tests_added", "tests_removed", "failures_new", "failures_fixed", "skipped_added", "skipped_removed"):
            if comparison[key]:
                print(f"  {key}: {len(comparison[key])} -> {', '.join(comparison[key][:8])}")
        if comparison["failures_new"]:
            problems.append(f"{len(comparison['failures_new'])} new failure(s)")
        if args.require_same_inventory and (comparison["tests_added"] or comparison["tests_removed"]):
            problems.append("inventory changed")
    elif actual["failures"]:
        problems.append(f"{len(actual['failures'])} failure(s)")
    if args.clean:
        shutil.rmtree(iso_root, ignore_errors=True)
    else:
        print(f"  logs: {iso_root}")
    if problems:
        print("RESULT: FAIL (" + "; ".join(problems) + ")")
        return 1
    print("RESULT: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
