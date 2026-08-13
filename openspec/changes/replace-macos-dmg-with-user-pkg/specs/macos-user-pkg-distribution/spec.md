## ADDED Requirements

### Requirement: macOS graphical releases use PKG only
The release workflow SHALL publish architecture-specific macOS Installer packages named `ACECode-<version>-macos-<arch>.pkg` and SHALL NOT build or publish DMG assets.

#### Scenario: Tagged macOS release succeeds
- **WHEN** a `v*` tag completes both macOS architecture jobs
- **THEN** the GitHub Release contains x64 and arm64 ACECode PKG installers
- **THEN** the GitHub Release contains no ACECode DMG installer

#### Scenario: Release assets are checksummed
- **WHEN** the release job collects platform artifacts
- **THEN** both PKG installers are included in `SHA256SUMS.txt`
- **THEN** existing macOS CLI archives and self-update ZIPs remain included

### Requirement: Installation is restricted to the current user
The ACECode PKG SHALL enable only the macOS current-user home installation domain and SHALL install the application at `~/Applications/ACECode.app` without writing to system `/Applications`.

#### Scenario: Standard user installs ACECode
- **WHEN** a logged-in user with normal write access to their home directory opens the PKG and confirms installation
- **THEN** macOS Installer places ACECode at that user's `~/Applications/ACECode.app`
- **THEN** installation does not request administrator authorization

#### Scenario: Installer domains are inspected
- **WHEN** macOS Installer evaluates the PKG installation domains
- **THEN** `CurrentUserHomeDirectory` is the only available domain
- **THEN** local-system and arbitrary-volume domains are disabled

### Requirement: The package is non-privileged and location-stable
The PKG MUST contain no install scripts or privileged helpers, MUST NOT permit payload relocation, and MUST require the running ACECode application to close before replacement.

#### Scenario: Package structure is expanded
- **WHEN** the release PKG is expanded for inspection
- **THEN** no preinstall, postinstall, or other package script is present
- **THEN** the component is non-relocatable and its application payload location is `/Applications` relative to the selected current-user domain

#### Scenario: Existing user installation is upgraded
- **WHEN** a newer PKG is installed and `~/Applications/ACECode.app` already exists
- **THEN** Installer replaces the application at that exact user-domain location
- **THEN** a running ACECode instance must be closed before replacement proceeds

### Requirement: Tagged PKGs are trusted Apple distribution artifacts
Tagged releases MUST sign executable payloads with Developer ID Application, sign the outer PKG with Developer ID Installer, submit the exact PKG to Apple's notary service, staple the accepted ticket, and validate it before upload.

#### Scenario: All macOS release credentials are available
- **WHEN** a tagged macOS job packages ACECode
- **THEN** the app and outer PKG signatures are verified
- **THEN** the exact PKG passes notarization, stapling validation, and Gatekeeper install assessment before upload

#### Scenario: A required credential is missing
- **WHEN** any application-signing, installer-signing, or notarization credential is absent for a tagged build
- **THEN** the macOS job fails before an unsigned or unnotarized PKG is uploaded

#### Scenario: Manual packaging has no trust credentials
- **WHEN** a non-tagged manual workflow runs without complete macOS credentials
- **THEN** it may produce a clearly suffixed `-unsigned.pkg` for structural inspection
- **THEN** that unsigned artifact is not presented as a trusted tagged release

### Requirement: DMG-only installation components are removed
The product SHALL NOT build or ship the custom `Applications.app` drop receiver, Finder disk-image layout, DMG artwork, or disk-image notarization path.

#### Scenario: macOS desktop is built
- **WHEN** CMake builds the ACECode desktop application on macOS
- **THEN** it builds `ACECode.app` without a separate fake Applications bundle

#### Scenario: Release implementation is inspected
- **WHEN** packaging scripts, workflow steps, resources, and documentation are searched
- **THEN** no active DMG creation, signing, notarization, upload, or drag-target behavior remains

### Requirement: Existing installation and update compatibility is preserved
The change SHALL preserve the current macOS self-update ZIP format and SHALL retain updater compatibility with exact existing `~/Applications/ACECode.app` and `/Applications/ACECode.app` installations.

#### Scenario: Installed user-domain copy checks for updates
- **WHEN** ACECode runs from `~/Applications/ACECode.app`
- **THEN** native self-update continues replacing that exact bundle without administrator authorization

#### Scenario: Legacy system-domain copy checks for updates
- **WHEN** ACECode already runs from `/Applications/ACECode.app`
- **THEN** the existing guarded system-location update policy remains available
- **THEN** new PKG installation does not relocate or delete that legacy copy

#### Scenario: Unsupported location requests self-update
- **WHEN** ACECode runs outside either supported Applications location
- **THEN** the error directs the user to reinstall with the signed PKG rather than an obsolete DMG
