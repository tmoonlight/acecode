## ADDED Requirements

### Requirement: Bundled macOS upgrades target the application bundle
When the upgrade engine runs from `ACECode.app/Contents/MacOS/acecode-daemon`, it SHALL treat the enclosing `ACECode.app` as the installation unit and SHALL NOT apply a flat package into the `Contents/MacOS` directory.

#### Scenario: Installed desktop starts an upgrade
- **WHEN** the daemon executable is inside the supported `~/Applications/ACECode.app` bundle
- **THEN** the updater stages and replaces the complete application bundle

#### Scenario: Standalone macOS CLI starts an upgrade
- **WHEN** the running executable is not inside an `ACECode.app/Contents/MacOS` layout
- **THEN** the updater retains the existing flat CLI upgrade behavior

### Requirement: Self-update is restricted to the safe user installation
The macOS bundle updater MUST require the current bundle to resolve to the exact non-symlinked `~/Applications/ACECode.app` destination and MUST refuse privileged, mounted-image, redirected, or arbitrary bundle locations.

#### Scenario: App runs from the supported destination
- **WHEN** the home, Applications directory, and installed bundle resolve to the standard per-user paths without symbolic-link redirection
- **THEN** the updater permits the signed-package preflight to continue

#### Scenario: App runs from an unsupported location
- **WHEN** the current bundle is in `/Applications`, on a mounted DMG, under a redirected Applications directory, or at another path
- **THEN** the updater fails with an actionable installation-path error before replacing any files

### Requirement: Candidate bundles are authenticated before installation
The updater MUST validate the complete candidate with Apple strict, all-architecture, and nested-code signature checks; require bundle identifier `dev.acecode.desktop`; require the same non-empty Developer Team ID as the installed app; and require the bundle version to equal the selected manifest release.

#### Scenario: Valid signed update is staged
- **WHEN** the candidate has a valid Developer ID signature from the installed app's Team ID, the production bundle identifier, valid nested code, and the expected version
- **THEN** the updater accepts it for installation

#### Scenario: Candidate is unsigned or signed by another team
- **WHEN** signature validation fails or the candidate Team ID differs from the installed app
- **THEN** the updater rejects the candidate and leaves the installed bundle unchanged

#### Scenario: Candidate version does not match the manifest
- **WHEN** a valid same-team bundle reports a version other than the selected release version
- **THEN** the updater rejects the candidate as a replay or packaging mismatch

### Requirement: Update archives preserve safe application metadata
The ZIP extractor SHALL preserve stored Unix permission bits for regular files and directories while continuing to reject traversal paths, and it MUST reject symbolic links and unsupported filesystem entry types.

#### Scenario: Signed app executable is extracted
- **WHEN** an archive regular-file entry contains executable permission bits
- **THEN** the staged file retains those executable bits for validation and restart

#### Scenario: Archive contains a symbolic link
- **WHEN** any ZIP entry is identified as a symbolic link
- **THEN** extraction fails before bundle validation or installation

### Requirement: Bundle replacement is recoverable
The updater SHALL copy and re-verify the candidate as a sibling of the installed app before switching paths, SHALL retain the previous app as `.ACECode.previous.app` on success, and SHALL restore it if the new bundle cannot be installed or fails final validation.

#### Scenario: Replacement succeeds
- **WHEN** the verified sibling candidate can replace the current bundle
- **THEN** `~/Applications/ACECode.app` contains the new version, the previous bundle is retained, and the update job requests restart

#### Scenario: Replacement fails after backup
- **WHEN** the current bundle has been moved aside but the candidate move or final verification fails
- **THEN** the updater restores the previous bundle to `~/Applications/ACECode.app` and reports failure

### Requirement: Desktop restart launches the installed replacement
After a successful macOS bundle replacement, the existing desktop restart action SHALL finish owned runtime teardown and launch the executable at the original installed bundle path.

#### Scenario: User accepts restart
- **WHEN** the update job succeeds and the user chooses immediate restart
- **THEN** the old desktop and managed daemons exit before the executable inside the new `ACECode.app` is launched
