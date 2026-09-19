# tui-runtime-logging Spec

## ADDED Requirements

### Requirement: Interactive TUI logs use the ACECode data directory

The interactive TUI SHALL initialize its primary logger with the effective ACECode data directory's `logs` subdirectory and the `tui` base name. It MUST create or append to a local-date file named `tui-YYYY-MM-DD.log`, rotate to a new file after local midnight, and MUST NOT mirror normal TUI log records to stderr.

The effective data directory MUST follow the existing `get_acecode_dir()` run-mode resolution, so normal user launches use the user ACECode data directory rather than the active workspace or startup worktree.

#### Scenario: Normal TUI startup does not pollute the workspace

- **WHEN** a user starts the interactive TUI in a workspace that has no `acecode.log`
- **THEN** the TUI creates or appends `<data-dir>/logs/tui-YYYY-MM-DD.log`
- **AND THEN** it does not create `<workspace>/acecode.log`

#### Scenario: Startup worktree does not become the log destination

- **WHEN** a user starts the interactive TUI with `--worktree <name>`
- **THEN** the TUI log is written below the effective ACECode data directory's `logs` directory
- **AND THEN** no primary TUI log is created inside the newly selected worktree

#### Scenario: Existing workspace log is preserved

- **WHEN** `<workspace>/acecode.log` already exists before interactive TUI startup
- **THEN** ACECode does not move, delete, or append to that file as part of logger initialization

### Requirement: Opt-in FTXUI input tracing uses a separate centralized log

When `ACECODE_TUI_INPUT_TRACE` is enabled, FTXUI SHALL write its input-event trace to `<data-dir>/logs/tui-input-trace-YYYY-MM-DD.log`. It MUST create the logs directory when needed and MUST rotate to the local-date file after local midnight. It MUST NOT create or append a relative `acecode.log` in the active workspace or startup worktree.

The input-trace file SHALL remain distinct from `tui-YYYY-MM-DD.log`, so high-frequency mouse and selection diagnostics do not obscure normal TUI runtime logs. It SHALL NOT be attached to TUI or GUI/Desktop feedback packages. Existing trace record text and its `DBG [ftxui-app]` prefix SHALL remain unchanged.

#### Scenario: Input tracing does not write inside a normal workspace

- **WHEN** TUI input tracing is enabled and a user starts the TUI in a workspace
- **THEN** trace records are written to `<data-dir>/logs/tui-input-trace-YYYY-MM-DD.log`
- **AND THEN** no trace-created `<workspace>/acecode.log` exists

#### Scenario: Input tracing does not write inside a startup worktree

- **WHEN** TUI input tracing is enabled and a user starts the TUI with `--worktree <name>`
- **THEN** trace records are written to the effective ACECode data directory's `logs` subdirectory
- **AND THEN** no trace-created `acecode.log` exists within the selected worktree

#### Scenario: Input tracing remains separate from primary TUI logs

- **WHEN** TUI input tracing and normal TUI logging both emit records
- **THEN** FTXUI trace records appear only in `tui-input-trace-YYYY-MM-DD.log`
- **AND THEN** primary runtime records continue to appear in `tui-YYYY-MM-DD.log`

### Requirement: TUI feedback includes the centralized TUI log

The TUI `/feedback` workflow SHALL find the most recently modified rotated TUI log in the effective ACECode data directory's `logs` subdirectory and include it in the feedback package as `logs/tui.log.tail.txt` when available. It MUST NOT add `<workspace>/acecode.log` as a normal TUI feedback source.

Failure to find a rotated TUI log MUST remain non-fatal and MUST NOT prevent collection of daemon or upgrade logs. TUI feedback MUST NOT attach a Desktop surface log, and GUI/Desktop feedback MUST NOT attach a TUI surface log.

#### Scenario: Feedback package includes the latest TUI log

- **WHEN** the logs directory contains multiple `tui-YYYY-MM-DD.log` files
- **AND WHEN** a user runs `/feedback`
- **THEN** the package includes the most recently modified matching file under `logs/tui.log.tail.txt`

#### Scenario: Legacy workspace log is not attached

- **WHEN** the workspace contains an old `acecode.log`
- **AND WHEN** a rotated TUI log is available in the data directory
- **THEN** the feedback package attaches the rotated TUI log
- **AND THEN** it does not attach the workspace-local legacy log

#### Scenario: TUI feedback excludes Desktop logs

- **WHEN** a TUI user runs `/feedback`
- **AND WHEN** a `desktop-YYYY-MM-DD.log` file is available
- **THEN** the package does not attach `logs/desktop.log.tail.txt`

#### Scenario: Desktop feedback excludes TUI logs

- **WHEN** a GUI/Desktop user submits feedback
- **AND WHEN** a `tui-YYYY-MM-DD.log` file is available
- **THEN** the package does not attach `logs/tui.log.tail.txt`

#### Scenario: Missing TUI log does not block feedback

- **WHEN** no `tui-YYYY-MM-DD.log` exists in the logs directory
- **THEN** feedback packaging continues without `logs/tui.log.tail.txt`
- **AND THEN** other available daemon or upgrade log sources continue to be collected

### Requirement: User documentation describes the unified runtime log location

User-facing documentation SHALL describe the interactive TUI log as `<data-dir>/logs/tui-YYYY-MM-DD.log` and direct TUI troubleshooting, including MCP connection troubleshooting, to the centralized runtime logs directory. It MUST NOT describe normal TUI runtime logging as writing `<workspace>/acecode.log`.

#### Scenario: Log-location documentation is consistent

- **WHEN** a user reads the logging FAQ or MCP troubleshooting instructions
- **THEN** both identify the centralized TUI dated log location
- **AND THEN** neither instructs the user to inspect a workspace-local `acecode.log` for normal TUI runtime logs
