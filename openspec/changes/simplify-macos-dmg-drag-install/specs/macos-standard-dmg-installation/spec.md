## ADDED Requirements

### Requirement: Standard drag-install disk image
The macOS DMG SHALL present `ACECode.app` and an `Applications` destination as its only visible installation items, and the destination SHALL resolve exactly to `/Applications`.

#### Scenario: User opens the disk image
- **WHEN** Finder opens the mounted ACECode DMG
- **THEN** `ACECode.app` SHALL appear on the left and an `Applications` folder destination SHALL appear on the right
- **AND** the image SHALL communicate that the application is dragged from left to right

#### Scenario: User drags the application to the destination
- **WHEN** the user drags `ACECode.app` onto the displayed `Applications` item
- **THEN** Finder SHALL copy the application into `/Applications/ACECode.app`

### Requirement: Uncluttered installation contents
The macOS DMG SHALL NOT expose a custom installer application or a visible installation instructions file.

#### Scenario: Mounted image contents are inspected
- **WHEN** the release DMG is mounted and its visible root items are enumerated
- **THEN** `Install ACECode.app` SHALL be absent
- **AND** installation README or instructions files SHALL be absent

### Requirement: Release integrity is preserved
The application and disk image SHALL continue through the release workflow's Developer ID signing, Apple notarization, and stapling checks after the packaging layout changes.

#### Scenario: A macOS release is published
- **WHEN** GitHub Actions produces the macOS DMG
- **THEN** the contained `ACECode.app` SHALL pass strict deep code-signature verification
- **AND** the application and DMG SHALL pass Gatekeeper assessment and stapling validation required by the release workflow

### Requirement: Supported native self-update locations
The macOS native updater SHALL accept a correctly signed ACECode bundle only when the running installation resolves exactly to `~/Applications/ACECode.app` or `/Applications/ACECode.app`.

#### Scenario: Existing per-user installation updates
- **WHEN** a valid signed update is requested from `~/Applications/ACECode.app`
- **THEN** the updater SHALL retain the existing secure replacement behavior for that bundle

#### Scenario: Standard system installation updates
- **WHEN** a valid signed update is requested from `/Applications/ACECode.app` and `/Applications` is writable by the current process
- **THEN** the updater SHALL replace that bundle using the same-volume staging and rollback behavior

#### Scenario: Unsupported installation location requests an update
- **WHEN** the running bundle is outside the two exact supported locations, is renamed, or traverses a symlink
- **THEN** the updater SHALL reject native replacement before modifying the installed bundle

#### Scenario: System Applications directory is not writable
- **WHEN** a valid update is requested from `/Applications/ACECode.app` but the current process cannot modify `/Applications`
- **THEN** the updater SHALL fail before modifying the installed bundle
- **AND** the error SHALL direct the user to install the update manually from the DMG

### Requirement: Legacy per-user installations are not relocated
The change SHALL NOT automatically move or delete an existing `~/Applications/ACECode.app` installation.

#### Scenario: Existing user receives the new release
- **WHEN** ACECode is currently running from `~/Applications/ACECode.app`
- **THEN** update operations SHALL continue to target that running bundle
- **AND** `/Applications/ACECode.app` SHALL NOT be created as a migration side effect
