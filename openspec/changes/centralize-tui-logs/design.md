# Design: centralize-tui-logs

## Context

The normal interactive startup path calls a file-static `initialize_logger_for_working_dir()` in `src/main.cpp`. It selects `Logger::init(working_dir + "/acecode.log")`, the legacy single-file mode. That explicit choice, rather than a logger fallback, causes project-local logs. `src/tui/tui_init.cpp` contains a duplicate helper with the same legacy behavior, although the current main path uses the file-static copy.

`Logger::init_with_rotation(get_logs_dir(), base_name, mirror_stderr)` is already used by desktop (`desktop`), daemon (`daemon`), and headless (`headless`). It creates the directory, names files `<base>-YYYY-MM-DD.log`, and reopens a new file when the local date changes. `get_logs_dir()` resolves from `get_acecode_dir()`, preserving the application-wide user/service data-root behavior.

TUI `/feedback` separately adds `ctx.cwd/acecode.log`, while the shared runtime-log collector finds desktop and daemon logs for GUI/Desktop feedback. The selected feedback boundary is origin-aware: TUI feedback must attach the centralized TUI log and must not attach Desktop logs; GUI/Desktop feedback must retain Desktop and daemon logs but must not attach TUI logs. This must move with the primary logger so TUI diagnostics remain attached after centralization.

## Goals / Non-Goals

**Goals:**

- Make normal interactive TUI runtime logs follow the same centralized data-directory convention as other runtime surfaces.
- Keep TUI records separate from desktop, daemon, and headless records via a `tui` filename prefix.
- Preserve legacy workspace files without filesystem migration or deletion.
- Ensure each feedback package contains its own surface log but not an unrelated interface's surface log.
- Keep the change localized, testable, and independent of frontend surfaces.

**Non-Goals:**

- Do not change the logger's legacy `init(file)` API; tests or specialized callers may still use it.
- Do not change which input events the opt-in `ACECODE_TUI_INPUT_TRACE` instrumentation records or its trace message format.
- Do not add retention, compression, or a user-configurable logging directory.
- Do not merge TUI logs with desktop or daemon logs.

## Decisions

### D1. Use the existing rotated logger with the `tui` base name

Both TUI logger initialization helpers will use:

```cpp
Logger::instance().init_with_rotation(get_logs_dir(), "tui", false);
```

This gives TUI logs the path `<data-dir>/logs/tui-YYYY-MM-DD.log`, automatically creates the directory, rotates at local midnight, and retains the current behavior of not writing log records to stderr. `working_dir` remains useful for the startup log message and later workspace setup, but no longer determines the log file path.

**Alternative considered:** keep a fixed `<data-dir>/logs/acecode.log`. Rejected because the existing runtime convention is per-surface dated files, and a TUI-specific prefix keeps simultaneous surfaces diagnosable.

### D2. Reconcile the duplicated TUI initialization helpers

The implementation must prevent `src/main.cpp` and `src/tui/tui_init.cpp` from continuing to encode divergent logger destinations. Prefer one shared TUI initialization helper when its existing dependency boundaries permit it; otherwise update both helpers to the same centralized rotated initialization and retain a focused regression test for the active startup seam.

The work must not leave a future path capable of silently restoring workspace-local primary logs.

### D3. Make runtime-log discovery feedback-origin-aware

Keep the existing GUI/Desktop runtime-log collector limited to `desktop` and `daemon`, preserving its documented REST contract. Add a TUI-specific collector (or an equivalent explicit source selector) that returns the latest `tui-YYYY-MM-DD.log` as `logs/tui.log.tail.txt` together with the daemon log, but excludes the Desktop log. Remove the TUI command's bespoke `ctx.cwd/acecode.log` source.

Both selectors use `latest_rotated_log_path` so matching and newest-file rules remain centralized. Missing logs remain non-fatal. This yields two explicit source sets:

- TUI `/feedback`: TUI + daemon + recent upgrade diagnostics.
- GUI/Desktop feedback: Desktop + daemon + recent upgrade diagnostics.

**Alternative considered:** add `tui` to `collect_runtime_log_sources(logs_dir)`. Rejected because that shared function backs the GUI/Desktop REST endpoint; it would silently broaden GUI diagnostic collection with unrelated terminal logs. Another alternative was a bespoke TUI path lookup in the command; rejected because it duplicates date-file matching and latest-file selection.

### D4. Configure FTXUI tracing through the process environment

FTXUI is built as a vcpkg dependency and cannot depend directly on ACECode's C++ configuration helpers. Add a dedicated environment variable, set by interactive TUI startup before FTXUI processes input, whose value is the UTF-8 absolute logs directory. FTXUI's trace writer reads that directory and creates/appends `tui-input-trace-YYYY-MM-DD.log` for the current local date on each trace write. If the variable is absent or the target file cannot be opened, tracing remains best-effort and creates no relative fallback file.

This preserves a clear dependency boundary: ACECode owns effective data-directory resolution, while FTXUI owns only local-date filename generation and appending trace records below the explicitly supplied directory. The trace writer should create the target directory before opening its file. An explicit absolute directory prevents worktree cwd changes from affecting the destination.

**Alternative considered:** link FTXUI against ACECode's logger/configuration utilities. Rejected because the dependency direction reverses the build boundary and would make the reusable FTXUI target depend on the application. Another alternative was a compile-time path definition; rejected because the effective data directory may be redirected at runtime and the date changes while a TUI process remains open.

### D5. Bump the FTXUI overlay port version

The overlay port builds `external/ftxui` from `SOURCE_PATH`, but vcpkg's ABI does not hash that source tree. Increment `ports/ftxui/vcpkg.json` `port-version` whenever the FTXUI source changes, ensuring existing local builds and CI rebuild the patched static library rather than reusing one that still writes `acecode.log` in the cwd.

### D6. Document the effective data-directory placeholder

Update the user manual's FAQ and MCP troubleshooting text to use `<数据目录>/logs/tui-YYYY-MM-DD.log`. Document the input-trace file separately as an opt-in debug artifact. Both descriptions use the effective data directory, which is accurate across supported platforms and data-root redirection, unlike hardcoding a Windows-only home path.

## Risks / Trade-offs

- **Feedback archive name change:** consumers that recognize `logs/acecode.log.tail.txt` will no longer see a TUI legacy entry. The old file was workspace-specific and is no longer a valid runtime source; a distinct `logs/tui.log.tail.txt` accurately identifies the new source. The high-volume FTXUI input trace remains a local diagnostic artifact and is not attached by either feedback origin.
- **Startup ordering:** TUI logger initialization remains before normal configuration loading, as it is today. `get_logs_dir()` only uses existing data-directory resolution and does not require loaded application configuration.
- **Old docs and external scripts:** workspace-local `acecode.log` may be referenced externally. The selected migration policy intentionally preserves old files but does not maintain an active compatibility copy, preventing further workspace pollution.

## Migration Plan

On the first upgraded interactive TUI launch, ACECode writes to the centralized dated TUI file. Existing workspace `acecode.log` files are untouched. `/feedback` begins attaching the latest centralized TUI log when available. No configuration or session migration is required.

## Validation

- Unit-test logger rotation behavior using the established `Logger` test fixture and add a targeted assertion for `tui` file naming.
- Unit-test feedback runtime source discovery with TUI, desktop, and daemon files, including latest-file selection and missing-TUI behavior.
- Add or adjust a focused TUI startup helper test so the production initialization chooses `get_logs_dir()` and the `tui` base name without needing an interactive terminal.
- Run the focused native tests and the repository's relevant build target.
- Verify documentation contains no remaining normal-TUI instructions to inspect workspace-local `acecode.log`.
