# Proposal: centralize-tui-logs

## Why

Interactive TUI startup currently writes a non-rotating `acecode.log` into the active workspace. This pollutes project directories, including startup worktrees, and differs from Desktop, daemon, and headless modes, which write dated logs in the ACECode data directory. The TUI feedback command also packages the workspace-local file, so it would omit the relevant diagnostics after the logging destination changes unless its source is updated together.

## What Changes

- Move the interactive TUI primary logger from `<workspace>/acecode.log` to `<data-dir>/logs/tui-YYYY-MM-DD.log`.
- Reuse the existing rotating logger behavior: create the shared logs directory as needed, rotate at local midnight, and do not mirror TUI records to stderr.
- Preserve existing workspace `acecode.log` files without migrating or deleting them; future normal TUI runs no longer create or append to them.
- Change TUI `/feedback` to attach the latest rotated TUI log as `logs/tui.log.tail.txt` instead of attaching `<workspace>/acecode.log`; it continues to attach the daemon and recent upgrade diagnostics but no longer attaches Desktop logs.
- Keep GUI/Desktop feedback limited to Desktop, daemon, and recent upgrade diagnostics; it must not attach TUI logs.
- Move opt-in FTXUI input tracing from a relative `acecode.log` in the active workspace to an independent centralized daily file, `<data-dir>/logs/tui-input-trace-YYYY-MM-DD.log`.
- Update user documentation and test coverage for the centralized TUI location and feedback source.

## Capabilities

### New Capabilities

- `tui-runtime-logging`: Centralized, dated TUI runtime logs and their feedback-package inclusion.

### Modified Capabilities

(None.)

## Impact

- **TUI startup:** `src/main.cpp` and the duplicate TUI logger initialization helper in `src/tui/tui_init.cpp`.
- **FTXUI input tracing:** `external/ftxui/src/ftxui/component/app.cpp`, its CMake definitions, and `ports/ftxui/` overlay-version metadata.
- **Feedback:** `src/commands/builtin_commands.cpp` and the rotated-log discovery helpers in `src/feedback/feedback_upload.*`.
- **Tests:** logger, feedback-package, and a focused TUI initialization seam if needed for testability.
- **Documentation:** `docs/user-manual.md`; update `docs/daemon-api.md` if its feedback attachment contract is expanded to name the TUI runtime log.
- **Compatibility:** existing FTXUI trace message format remains unchanged; only its output destination and daily rotation behavior change.
