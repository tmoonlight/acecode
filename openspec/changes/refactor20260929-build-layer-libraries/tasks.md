## 1. Layer libraries

- [x] 1.1 Capture pre-P5 source/test inventory and create base_core/base_host STATIC libraries with platform resources; verify source ownership and successful MSVC configure.
- [x] 1.2 Create domain/adapters/engine/host STATIC libraries with scoped dependencies; verify target types and an acyclic downward graph.
- [x] 1.3 Create Web/TUI/headless/daemon/CLI libraries, make acecode_testable INTERFACE and remove TUI subset selection; verify all TUI sources have one library owner and generated Web assets stay in Web.
- [x] 1.4 Move Desktop/native-helper shared sources to their proper owners and narrow test drivers; verify Desktop's link graph excludes agent/TUI/Crow/Web assets and each driver links successfully.

## 2. Guards and documentation

- [x] 2.1 Add configure-time and File API contract checks for coverage, duplication, target types and forbidden links; verify positive and negative fixtures and BUILD_TESTING=OFF configuration.
- [x] 2.2 Update current architecture, test/build instructions and parent P5 roadmap; verify document paths, strict refactor guards and OpenSpec validation.

## 3. Integration and delivery

- [x] 3.1 Complete MSVC Release builds for CLI/Desktop/unit tests and applicable smoke targets; run full C++ tests and confirm the baseline inventory is unchanged, recording results.
- [x] 3.2 Complete native Linux CLI/Desktop/unit-test linking and full tests; verify the configured library graph and record evidence.
- [ ] 3.3 Review the complete diff, commit only P5 changes, deliver through protected master and verify CI plus final local/remote state; retain unrelated changes.
