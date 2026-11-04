## Purpose

Provide native Windows installation downloads that correspond exactly to each published ACECode version and architecture, with verifiable package integrity.

## ADDED Requirements

### Requirement: Complete Windows installer assets
Every full stable release SHALL provide versioned native setup executables for Windows x64 and ARM64 in addition to its portable ZIPs. Both installers SHALL be listed in the release checksum file and satisfy explicit size budgets.

#### Scenario: One installer is absent
- **WHEN** either architecture's installer is missing or empty
- **THEN** release completeness verification rejects publication

### Requirement: Installer payload identity
A published installer SHALL contain the exact matching release ZIP and embedded metadata identifying its version, architecture, payload size and hashes. Verification SHALL reject any mismatch before publication.

#### Scenario: Wrong payload or metadata
- **WHEN** an installer contains another version, architecture, altered payload or uninstaller
- **THEN** package verification fails

### Requirement: Separate installer downloads
Verified installers SHALL be available as versioned files and latest aliases on the update download server. Public downloads SHALL match release sizes and hashes. Installer executables SHALL remain outside the six-platform ZIP self-update manifest.

#### Scenario: Full release mirroring
- **WHEN** both installers are mirrored successfully
- **THEN** their versioned and alias downloads match GitHub release hashes and the updater manifest still contains only the six ZIP targets

### Requirement: Installation lifecycle validation
Windows x64 release validation SHALL exercise the actual setup and uninstaller in an isolated profile, checking installed version and installation records while preserving user data.

#### Scenario: Install and uninstall
- **WHEN** the release installer installs and then uninstalls within the test sandbox
- **THEN** runtime files and installation records are created and removed as expected, while the sandbox's user-data sentinel remains intact

#### Scenario: Installation and temporary directories are on different volumes
- **WHEN** a silent installation is removed from a different disk than the temporary directory
- **THEN** uninstallation succeeds and removes the installation directory, deferring cleanup of a still-running launcher on its own volume
