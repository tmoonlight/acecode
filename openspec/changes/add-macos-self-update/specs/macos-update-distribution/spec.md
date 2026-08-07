## ADDED Requirements

### Requirement: Tagged releases publish dedicated macOS update archives
The packaging workflow SHALL publish separate x64 and arm64 ZIP archives containing a complete top-level `ACECode.app` and a root signed `acecode` payload for self-update, while retaining the existing DMGs and CLI archives.

#### Scenario: Tagged release completes
- **WHEN** a `v*` tag builds both macOS targets with required credentials
- **THEN** the GitHub Release includes `ACECode-<version>-macos-x64-update.zip` and `ACECode-<version>-macos-arm64-update.zip` in addition to the existing assets and checksums

### Requirement: Update archives contain a trusted application
Before creating a public update ZIP, the workflow MUST sign the app and nested code, submit the app to Apple's notary service, wait for acceptance, staple and validate the ticket on `ACECode.app`, and verify Gatekeeper acceptance.

#### Scenario: Apple accepts the app submission
- **WHEN** the signed app notarization completes successfully
- **THEN** the workflow staples the app and creates the final update ZIP from that exact validated bundle

#### Scenario: App notarization fails
- **WHEN** Apple rejects or cannot complete the app submission for a tagged release
- **THEN** the macOS job fails without publishing a public update ZIP or DMG

### Requirement: Update ZIPs preserve bundle permissions
The release workflow MUST create update ZIPs with a macOS metadata-preserving tool and MUST verify the archive by extracting it and checking the resulting app signature and executable permissions.

#### Scenario: Update ZIP is packaged
- **WHEN** the stapled app is archived for self-update
- **THEN** a clean extraction contains an executable, strictly valid `ACECode.app` with valid nested code

### Requirement: Update service records identify exact artifacts
Release documentation SHALL provide `aceupdate.json` examples for both macOS targets and require each record to use the final update ZIP filename, lowercase SHA-256, and exact byte size.

#### Scenario: Release operator publishes a macOS update
- **WHEN** the operator uploads the architecture-specific ZIPs to the configured update service
- **THEN** the manifest records allow each client to select, download, size-check, hash-check, and authenticate its matching bundle
