## 1. FTXUI dependency coherence

- [x] 1.1 Bump the FTXUI overlay port-version and update its pinned-submodule documentation
- [x] 1.2 Reconfigure the clean Windows build and prove the current FTXUI hover API compiles

## 2. TUI hyperlink boundary safety

- [x] 2.1 Add regression tests for authority delimiters, userinfo, malformed remote targets, and IPv6 hosts
- [x] 2.2 Implement authority-first host parsing and fail-closed handling for URL-shaped labels
- [x] 2.3 Bound hover tooltip rendering by terminal cell width and preserve valid UTF-8
- [x] 2.4 Reap POSIX URL-opener child processes without changing global signal handling

## 3. Durable skill usage state

- [x] 3.1 Add tests for strict UTC timestamps, fractional seconds, invalid dates, damaged schemas, saturation, and concurrent instances
- [x] 3.2 Implement strict UTC parsing and type-safe schema normalization
- [x] 3.3 Serialize read-modify-write operations with a cross-process sidecar lock

## 4. Safe package verification tooling

- [x] 4.1 Add regression coverage for CMake target mapping, generator choice, staging path guards, and process reaping
- [x] 4.2 Fix verify-package build selection, destructive-path validation, and Desktop process cleanup
- [x] 4.3 Synchronize and hash-check all four verify-package script copies

## 5. Robust Desktop development tooling

- [x] 5.1 Add Python tests for declared-version syntax, Web input freshness, multi-config artifact discovery, and external build paths
- [x] 5.2 Fix Python 3.8 compatibility, Web freshness inputs, build discovery, ANSI output, and path display
- [x] 5.3 Register and run the script test in the repository test suite when Python is available

## 6. Full verification

- [x] 6.1 Run focused Python and GoogleTest regressions
- [x] 6.2 Run the complete Web test and production build
- [x] 6.3 Build TUI, Desktop, and unit tests in Release and run the full CTest suite
- [x] 6.4 Run repository quality/diff checks and confirm unrelated dirty files are excluded
