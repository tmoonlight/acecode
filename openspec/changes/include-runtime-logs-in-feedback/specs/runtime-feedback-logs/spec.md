## ADDED Requirements

### Requirement: Feedback packages support bounded multiple log sources
The feedback package builder SHALL accept zero or more log sources and SHALL
read at most the configured tail limit from each available source.

#### Scenario: Multiple logs are available
- **WHEN** a feedback request identifies multiple readable log files
- **THEN** the package contains one bounded tail entry for each source
- **AND** each source uses its own byte cap or the request default when no per-source cap is set

#### Scenario: Some requested logs are unavailable
- **WHEN** at least one requested log is missing or unreadable
- **AND** at least one other requested log is readable
- **THEN** packaging succeeds with the readable log entries
- **AND** the unavailable source does not remove or invalidate the available diagnostics

#### Scenario: Entry names collide
- **WHEN** multiple readable sources request the same zip entry name
- **THEN** each log is stored under a unique deterministic entry name
- **AND** no earlier log entry is overwritten

### Requirement: Runtime log selection uses the newest available rotations
The runtime log collector SHALL independently select the newest regular
desktop and daemon rotated log files from the configured logs directory.

#### Scenario: Both runtime logs exist
- **WHEN** the logs directory contains one or more `desktop-*.log` files and one or more `daemon-*.log` files
- **THEN** the collector selects the most recently modified matching file for each runtime
- **AND** assigns stable `logs/desktop.log.tail.txt` and `logs/daemon.log.tail.txt` entry names

#### Scenario: One runtime is absent
- **WHEN** the logs directory contains a daemon rotation but no desktop rotation
- **THEN** the collector returns the daemon source
- **AND** does not synthesize or require a desktop source

### Requirement: Feedback callers include the logs relevant to their runtime
Desktop/Web and TUI feedback callers SHALL use the shared multi-log package
builder while retaining their caller-specific diagnostic scope.

#### Scenario: Desktop or Web feedback is submitted
- **WHEN** the Desktop/Web feedback endpoint builds a package
- **THEN** it includes the newest available desktop and daemon runtime log tails
- **AND** a browser-only deployment can submit a daemon-only package

#### Scenario: TUI feedback is submitted
- **WHEN** the user explicitly invokes TUI `/feedback`
- **THEN** the package requests the working-directory `acecode.log`
- **AND** also includes the newest available desktop and daemon runtime log tails
- **AND** no unrelated configuration, memory, workspace, or session files are added

### Requirement: Feedback metadata reports aggregate and per-log results
Feedback package metadata and the Desktop/Web response SHALL preserve aggregate
log fields and SHALL report the inclusion result for each requested source.

#### Scenario: At least one log is included
- **WHEN** one or more requested log tails are added to the package
- **THEN** `log_included` or `log_available` is true
- **AND** `log_tail_bytes` equals the sum of all included tail byte counts
- **AND** `included_files` lists every actual log archive entry

#### Scenario: Per-log metadata is returned
- **WHEN** feedback packaging evaluates requested log sources
- **THEN** `logs[]` records each source path, final entry name, availability, and included tail byte count
- **AND** the same per-log array is present in `feedback.json` and the Desktop/Web success response
