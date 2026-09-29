# P5 verification

Baseline: master `fbbc69979a0e8ec2c289cc5bdf4f4f01b6e94ad5`. Implementation changes CMake ownership and dependencies; no production C++ source is changed.

## Structural acceptance

- 12 layer/application libraries are STATIC; `acecode_testable` is a source-free INTERFACE aggregate.
- All 144 TUI implementation files belong to `acecode_tui`; there are no testable TUI subset lists. Headers retain their IDE source groups in the owning libraries.
- Windows File API source comparison: 636 unique production translation units before and after, no additions or removals. Embedded asset code belongs only to `acecode_web`.
- Desktop's project-library link set is exactly base_core plus desktop_support. The session writer links domain/base_core, and the state-file driver links only base_core. Native helpers reuse base_core.
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

Additional CTest checks exposed an unchanged baseline fixture failure: a macOS execute-bit check used chmod on the Windows host. The fixture now simulates os.access only on Windows, retains real chmod/access checks on POSIX, and asserts the executable-access call. Production package verification is unchanged. The corrected helper test is rechecked on both hosts.

## Linux integration and CI

Native Ubuntu 22.04 / GCC 11.4 / CMake 3.25 build is in progress in an isolated build directory, using the current checkout and pinned WebView 0.12.0. Full Linux test, PR CI and cross-platform branch-package results are pending. Branch validation does not publish a version tag, release or npm package.

Existing manual acceptance deferrals and the nine preserved legacy refs are unchanged. Unrelated console-font and agent-office working-tree changes are excluded from this delivery.
