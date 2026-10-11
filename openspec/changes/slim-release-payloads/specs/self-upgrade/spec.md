## ADDED Requirements

### Requirement: Deduplicated macOS update payload
The macOS self-update archive SHALL contain a single complete signed application payload and a lightweight CLI forwarding entry, without a second copy of the daemon, Computer Use runtime, model catalog, or seed tree outside the application. It MUST remain consumable by the existing desktop and flat CLI updater, preserve executable permissions and argument boundaries, and keep strict signature and notarization checks for official releases.

#### Scenario: Existing desktop installation updates
- **WHEN** an existing desktop client consumes the deduplicated archive
- **THEN** it finds, verifies and atomically installs the complete app through the existing app update path

#### Scenario: Existing flat CLI installation updates
- **WHEN** a client expecting a root acecode executable consumes the archive
- **THEN** it installs the forwarding entry and app, and subsequent CLI invocations execute the bundled daemon with unchanged arguments and its bundled resources

#### Scenario: Forwarding entry is invoked through PATH or a symbolic link
- **WHEN** a user launches the CLI outside the installation directory
- **THEN** it locates the app relative to the real launcher location and returns the daemon's exit behavior

#### Scenario: Incomplete archive
- **WHEN** the application, forwarding entry, required runtime, or bundled resources are missing
- **THEN** package verification fails before publication
