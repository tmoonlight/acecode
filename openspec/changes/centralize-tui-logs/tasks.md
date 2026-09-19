# Tasks: centralize-tui-logs

## 1. TUI logger initialization

- [x] 1.1 Replace the interactive TUI startup use of workspace-local `Logger::init(.../acecode.log)` with `Logger::init_with_rotation(get_logs_dir(), "tui", false)` while preserving debug log level and startup diagnostics.
- [x] 1.2 Reconcile the duplicate initialization helper in `src/tui/tui_init.*` so it cannot retain or reintroduce workspace-local primary logging.
- [x] 1.3 Add focused native coverage proving the TUI logger uses the shared logs directory, `tui-YYYY-MM-DD.log` naming, no stderr mirror, and does not select the workspace as the primary log destination.

## 2. FTXUI input-trace destination

- [x] 2.1 Add a TUI-startup environment bridge that supplies FTXUI the effective ACECode logs directory before input processing starts.
- [x] 2.2 Change the opt-in FTXUI trace writer to create and append `tui-input-trace-YYYY-MM-DD.log` below that directory, with local-date rollover and no relative-path fallback.
- [x] 2.3 Bump the FTXUI overlay port version so vcpkg rebuilds the source-backed dependency.
- [ ] 2.4 Add focused coverage for trace-path selection and local-date filename generation without requiring an interactive terminal; verify an input-trace-enabled build does not create workspace/worktree `acecode.log`.

## 3. Feedback runtime-log collection

- [x] 3.1 Preserve the GUI/Desktop runtime-log selector as Desktop + daemon only; add a TUI feedback selector that returns latest TUI + daemon logs and excludes Desktop logs.
- [x] 3.2 Remove the TUI `/feedback` command's explicit `<cwd>/acecode.log` source and use the TUI centralized selector.
- [x] 3.3 Add feedback-package tests covering latest TUI log selection, TUI exclusion of Desktop logs, GUI/Desktop exclusion of TUI logs, missing TUI logs, and exclusion of a workspace-local legacy `acecode.log`.

## 4. Documentation

- [x] 4.1 Update the user-manual logging FAQ to describe `<数据目录>/logs/tui-YYYY-MM-DD.log`, the opt-in `tui-input-trace-YYYY-MM-DD.log`, and the preservation of legacy workspace files.
- [x] 4.2 Update MCP failure troubleshooting text to direct users to the centralized TUI dated log.
- [x] 4.3 Update daemon API feedback-package documentation if its runtime-log attachment listing needs to name `logs/tui.log.tail.txt`.

## 5. Verification

- [ ] 5.1 Run the focused logger, FTXUI trace-path, and feedback native tests.
- [ ] 5.2 Configure and build an input-trace-enabled TUI executable with the bumped FTXUI port, then verify normal-workspace and `--worktree` runs leave no trace-created `acecode.log`.
- [x] 5.3 Review the documentation diff and verify normal-TUI and FTXUI input-trace documentation no longer directs users to workspace-local `acecode.log`.
