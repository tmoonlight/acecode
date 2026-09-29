# P5 verification

Baseline: master `fbbc69979a0e8ec2c289cc5bdf4f4f01b6e94ad5`. Implementation changes CMake ownership and dependencies; no production C++ source is changed.

## Structural acceptance

- 12 layer/application libraries are STATIC; `acecode_testable` is a source-free INTERFACE aggregate.
- All 144 TUI implementation files belong to `acecode_tui`; there are no testable TUI subset lists. Headers retain their IDE source groups in the owning libraries.
- Windows File API source comparison: 636 unique production translation units before and after, no additions or removals. Embedded asset code belongs only to `acecode_web`.
- Desktop's project-library link set is exactly base_core plus desktop_support, and its visible project include roots are only apps/base. The session writer links domain/base_core, and the state-file driver links only base_core. Native helpers reuse base_core.
- MSVC/CMake 4.1.1 configuration and File API checks pass. Native Linux/CMake 3.25 configuration also passes with BUILD_TESTING=OFF and Desktop disabled.
- Configure guards reject missing/duplicate owners, compiling an inactive source, upward library edges, an OBJECT test aggregate and transitive Desktop-to-engine links. Refactor tooling: 99 tests passed.
- Strict layer, size, final ownership, include normalization, layout-map and doc-path checks passed; OpenSpec strict validation and `git diff --check` passed.

File API checks require a query before configuration, as exercised by the updated CI workflow:

```sh
python scripts/refactor/cmake_target_snapshot.py --build-dir <build> --query
cmake -S . -B <build> <existing toolchain and platform options>
python scripts/refactor/check_layer_libraries.py --build-dir <build>
```

## Windows integration

MSVC 19.39, Release/Ninja, x64-windows-static. Reused the phase-one build directory. CLI, Desktop, unit tests and five explicit smoke targets built and linked successfully:

```sh
cmake --build build/refactor-phase1-windows --target acecode acecode-desktop acecode_unit_tests acecode_upgrade_restart_smoke computer_use_broker_smoke computer_use_native_smoke agent_browser_host_smoke agent_browser_pointer_demo --parallel 4
```

The HTTP fixtures now declare their own httplib include path, and WinPTY/ZIP/SQLite tests declare their direct dependencies. This replaces accidental inheritance from the old OBJECT target.

Full C++ profile: 5304 listed, 5303 executed, 5294 passed, 9 skipped, 0 failed, no omitted enabled cases; inventory unchanged. Six isolated shards plus serial suites completed in 221.7 seconds.

Additional CTest checks exposed an unchanged baseline fixture failure: a macOS execute-bit check used chmod on the Windows host. The fixture now simulates os.access only on Windows, retains real chmod/access checks on POSIX, and asserts the executable-access call. Production package verification is unchanged. The corrected helper test passed on both hosts; all five non-GTest Windows CTest checks passed.

## Linux integration and CI

Native Ubuntu 22.04 / GCC 11.4 / CMake 3.25: CLI, Desktop, unit tests and the upgrade/restart smoke target built and linked successfully from this checkout using pinned WebView 0.12.0. The generated File API contract passed. Full C++ profile from the native build/tests directory: 5218 listed, 5217 executed, 5201 passed, 16 skipped, 0 failed, no omitted enabled cases; 43.9 seconds.

Local environment observations were resolved without changing C++ assertions or runtime code:

- A first run from the Windows-mounted source directory failed to enter provider retry before its two-second test deadline. The unrelated Git-context probe timed out after three seconds; direct git status exceeded eight seconds on that mount. The same case passed three times from the native Linux test directory (23/0/0 ms), then the entire suite passed there, matching CTest's working-directory convention.
- The five native Python/architecture/runtime CTest checks passed. Four shell checks initially saw the Windows checkout's CRLF endings. They passed against a Git archive of the same f08cbf69 commit with LF endings. The package-launch check used a private PID namespace because an existing user Desktop process correctly caused its startup probe to skip outside isolation; that user process was preserved.

Final implementation: `d7044c96c707cd67cbd4edb08657fcd58481b431`. Desktop's narrowed apps/base include visibility and the explicit Deepin desktop-support dependency passed MSVC/GCC rebuilds; injecting an engine include root into the generated graph is rejected.

| Final CI run | Evidence |
| --- | --- |
| [test 36577951353](https://github.com/tmoonlight/acecode/actions/runs/36577951353) | Success: strict refactor guards, Web tests/build, macOS installer contracts, Linux CLI/Desktop/unit-test build, File API library contract and models.dev validation. Full CTest registered 5227 cases: 1 disabled, 16 skipped, 0 failures; 288.34 seconds. |
| [package 36577945428](https://github.com/tmoonlight/acecode/actions/runs/36577945428) | Success: Windows x64/ARM64, macOS x64/ARM64, Linux x64/ARM64/ARMv7 and Deepin x64/ARM64/ARMv7 all built and uploaded their packages. Release and npm publication were skipped for branch validation. |

## Delivery

[PR #90](https://github.com/tmoonlight/acecode/pull/90) merged the implementation into protected master on 2026-09-29 as `7e47b719c8a7000011dea895307ca4645af20189`. The local master was fast-forwarded and its HEAD matched the remote master. The subsequent acceptance-record update changes Markdown only; it does not alter the verified build or runtime code.

Existing manual acceptance deferrals and the nine preserved legacy refs are unchanged. Unrelated console-font and agent-office working-tree changes are excluded from this delivery.
