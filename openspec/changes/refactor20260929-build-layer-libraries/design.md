## Context

Baseline: master fbbc6997. P5 is specified in the parent refactor design section 8.4. Source include boundaries already pass strict layer lint; the build still combines them in an OBJECT library. Desktop has separate native and application support archives. Linux and MSVC must link the complete production and test targets.

## Goals / Non-Goals

Goals: every production translation unit has one primary owner; layer libraries are STATIC; the full TUI is one STATIC target; `acecode_testable` is INTERFACE; existing executable names, platform flags, resources, test discovery and runtime behavior are preserved.

Non-goals: no directory relocation or application logic changes. The parent P4 snapshot equality rule applied to source relocation; P5 intentionally changes targets and source ownership, so validate the complete source inventory and new graph instead of pretending that target tuples remain identical. Manual acceptance and nine legacy refs retain their existing arrangements.

## Decisions

### 1. Build graph follows ownership

All target names use the `acecode_` prefix. `base_core` owns config, image, ipc, platform, utils and workspace. `base_host` owns network, pty and environment and depends on base_core. Domain depends on base_core; adapters depend on domain and base_host; engine depends on adapters; host depends on engine and adds Crow. Web, TUI and headless depend on host; daemon depends on Web; CLI assembly depends on the application surfaces. Existing source-policy rules remain stricter than group-level visibility.

Each target publishes its own group include root and receives lower roots through declared dependencies. The six-root interface remains only for specialized standalone fixtures. Attach external headers/libraries to their consumers: stb privately to base_core, CPR to network/adapters, MCP to adapters, FTXUI to TUI/CLI, Crow to host/Web. Preserve ASIO_STANDALONE and platform libraries. This exposes dependencies without changing header spellings or moving source files.

### 2. Specialized process boundaries remain explicit

Desktop support owns all reusable desktop sources and depends on base_core; the native-bridge archive's base files move to base_core and its desktop helpers move to desktop support. Desktop does not link domain/adapters/engine/host/Web/TUI or the test aggregate. Its direct WebView/CPR dependencies remain. Computer-use native helpers keep a separate archive with their original platform flags; shared base helpers link from base_core. WinPTY keeps its upstream-specific source ownership and embedding step. Select .mm implementations only on Apple, notification backends by platform, and Deepin effects only in the desktop child directory.

### 3. Static archives provide shared implementations

Compile every TUI implementation into `acecode_tui`; delete the testable subset lists and selector. `acecode_testable` becomes a source-free INTERFACE aggregate of production libraries and desktop support. Production CLI links CLI assembly directly. Small test drivers link the owning layer (domain, base_core or Web) where possible. Embedded Web assets belong only to the Web archive. No whole-archive workaround or circular layer dependencies.

### 4. Configure and test the contract

Configure-time checks reject empty source groups, missing explicit paths, unowned or multiply owned primary translation units, wrong target types and forbidden Desktop link paths. Existing standalone smoke fixtures may deliberately compile their selected sources independently; they are not primary owners. Excluded platform/desktop-off implementations must be explicit. Add negative contract tests for guards and verify the real configured graph through CMake File API, including BUILD_TESTING=OFF. CI writes the query before configuration: CMake 3.20-3.25 loads queries before project evaluation, so a query created inside CMakeLists would miss the first generation. Keep this independent check in the CI configure sequence; ordinary CTest users do not need an extra configure step. HTTP test fixtures explicitly receive their httplib include directory; ZIP/SQLite and WinPTY tests declare their direct library dependencies. Windows validation exposed a pre-existing package-verifier fixture that assumed POSIX execute bits; simulate that OS boundary only on Windows, retaining native POSIX checks and the production verifier.

### 5. Verification and delivery

Capture the existing Windows source/test inventory before reconfiguration. Reuse the MSVC Ninja build directory; rebuild CLI, Desktop, tests and applicable EXCLUDE_FROM_ALL smoke targets. Compare gtest inventory and run the full suite in isolated runtime directories. Verify Linux CLI/Desktop/tests with native GCC/Clang and full tests through available local dependencies or CI. Inspect macOS/Deepin source selection and preserve flags; use native packaging validation if platform-specific changes require it. Run strict refactor guards, OpenSpec validation and diff checks. Commit only P5 files, deliver through the protected master PR path and verify final remote state.

## Risks / Trade-offs

- Static archive ordering or a hidden dependency can fail only on ELF linkers: use explicit downward dependencies and Linux full linking; do not mask with archive grouping.
- Glob filtering can drop a new implementation: enforce complete primary-source ownership at configure time and cover negative cases.
- Moving compile settings can change platform behavior: retain source definitions, Objective-C++/ARC/minimum-version flags and native resources, and audit platform branches.
- A broad helper aggregate can hide unnecessary linkage: link focused drivers to their actual owners and check Desktop's transitive graph.
- Concurrent unrelated work exists in Web/console-font files: use content/mtime-checked writes and exact commit staging; do not change those files.

## Migration Plan

Create the layer targets, move consumers to them, remove obsolete selectors, update docs and guards, validate both required toolchains, then commit and merge. A focused revert of P5 restores the previous build graph without source migration or data conversion.
