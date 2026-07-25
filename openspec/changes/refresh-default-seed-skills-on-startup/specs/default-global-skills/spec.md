## MODIFIED Requirements

### Requirement: Provide a Curated Default Skill Bundle

ACECode SHALL ship a local default skill seed bundle containing exactly the following
skills:

- `find-skills`
- `skill-installer`
- `skill-creator`
- `native-mcp`
- `mcporter`
- `acecode-tui-usage`
- `acecode-desktop-usage`
- `vision-image-reader`

The bundle SHALL include a canonical `seed.version` revision in `YYYY-MM-DD.N`
format, and `MANIFEST.json.bundle_version` SHALL match that revision.

#### Scenario: Seed bundle is enumerated

- **WHEN** ACECode enumerates the bundled default skills
- **THEN** the result includes all eight named default skills
- **AND** the result does not include any other default seed skill
- **AND** every enumerated skill has a `SKILL.md`

#### Scenario: Bundle version metadata agrees

- **WHEN** ACECode validates its packaged seed assets
- **THEN** `seed.version` contains a valid monotonic bundle revision
- **AND** `MANIFEST.json.bundle_version` equals that revision

### Requirement: Preserve Existing Global Skill Content

ACECode MUST NOT silently overwrite an unknown or user-modified global skill
directory. ACECode SHALL update an existing seeded directory only when persisted
ownership state and a complete directory digest prove that ACECode installed the
current unmodified copy.

#### Scenario: Unknown existing skill conflicts with a seed

- **GIVEN** a target global skill directory already exists
- **AND** ACECode has no state proving ownership of that directory
- **WHEN** ACECode reconciles the default seed bundle
- **THEN** ACECode leaves the existing directory and files unchanged
- **AND** records the outcome as preserved user content

#### Scenario: User modified an ACECode-installed seed

- **GIVEN** state identifies a target as previously installed by ACECode
- **AND** the target directory digest differs from the recorded installed digest
- **WHEN** ACECode reconciles a newer seed bundle
- **THEN** ACECode leaves the modified directory and files unchanged
- **AND** records the outcome as preserved user content

#### Scenario: ACECode-installed seed remains pristine

- **GIVEN** state identifies a target as previously installed by ACECode
- **AND** the complete target directory digest equals the recorded installed digest
- **WHEN** ACECode reconciles a newer seed bundle
- **THEN** ACECode replaces the complete target directory with the newer bundled
  copy
- **AND** records the new installed directory digest

### Requirement: Record Default Seed Installation State

ACECode SHALL record detailed seed reconciliation state under the ACECode home and
SHALL atomically record the last reconciled bundle revision in
`~/.acecode/seed.version`.

#### Scenario: Seed state records reconciliation outcomes

- **WHEN** ACECode completes a default seed reconciliation attempt
- **THEN** `.seed_skills_state.json` records the bundled revision
- **AND** records every seed skill's source id, relative path, and result
- **AND** successful installed or updated entries record complete source and
  installed directory SHA-256 digests

#### Scenario: Successful reconciliation advances the user marker

- **GIVEN** all bundled skills were installed, updated, unchanged, or intentionally
  preserved as user content
- **WHEN** ACECode atomically persists the detailed reconciliation state
- **THEN** ACECode atomically writes the bundled revision to
  `~/.acecode/seed.version`

#### Scenario: Failed reconciliation remains retryable

- **GIVEN** source validation, hashing, copying, rollback, or state persistence fails
- **WHEN** ACECode ends the reconciliation attempt
- **THEN** ACECode does not advance `~/.acecode/seed.version`
- **AND** the next startup can retry the same bundled revision

## ADDED Requirements

### Requirement: Reconcile Default Skills During Startup

ACECode SHALL compare the packaged seed revision with the user's last reconciled
revision during TUI and daemon startup and SHALL finish any required reconciliation
before the first skill registry scan.

#### Scenario: Existing home without a user marker receives defaults

- **GIVEN** the ACECode home already exists
- **AND** `~/.acecode/seed.version` does not exist
- **WHEN** ACECode starts
- **THEN** ACECode installs every missing default seed skill
- **AND** the installed skills are visible in the first registry scan

#### Scenario: Older user revision triggers reconciliation

- **GIVEN** the packaged seed revision is newer than
  `~/.acecode/seed.version`
- **WHEN** ACECode starts
- **THEN** ACECode reconciles every bundled seed according to ownership state
- **AND** advances the user marker after successful reconciliation

#### Scenario: Equal revision is a no-op

- **GIVEN** the packaged and user seed revisions are equal
- **WHEN** ACECode starts
- **THEN** ACECode does not copy or replace seeded skill directories

#### Scenario: Newer user revision prevents downgrade

- **GIVEN** `~/.acecode/seed.version` is newer than the packaged seed revision
- **WHEN** ACECode starts an older executable
- **THEN** ACECode does not copy or replace seeded skill directories
- **AND** leaves the newer user marker unchanged

#### Scenario: Invalid packaged revision is non-destructive

- **GIVEN** the packaged `seed.version` is missing or invalid
- **WHEN** ACECode starts
- **THEN** ACECode logs a seed reconciliation error
- **AND** does not change seeded skills, detailed state, or the user marker
- **AND** continues opening ACECode

### Requirement: Serialize Concurrent Seed Reconciliation

ACECode SHALL serialize the complete seed read/check/reconcile/write transaction
across threads and processes that use the same ACECode home.

#### Scenario: TUI and daemon start concurrently

- **GIVEN** both TUI and daemon observe a missing or stale user marker
- **WHEN** they attempt seed reconciliation concurrently
- **THEN** only one process mutates seed targets or state at a time
- **AND** the later process re-reads the marker after acquiring the lock
- **AND** the final seed state and user marker are valid and consistent

### Requirement: Keep Versioned Seed Reconciliation Offline

ACECode SHALL reconcile default seed skills exclusively from bundled local assets.

#### Scenario: Existing user upgrades without network access

- **GIVEN** the runtime environment has no network access
- **AND** the packaged seed revision is newer than the user revision
- **WHEN** ACECode starts
- **THEN** ACECode can reconcile the bundled defaults
- **AND** does not invoke a remote package manager or repository

## REMOVED Requirements

### Requirement: Install Default Skills During First Initialization

**Reason**: Tying installation to creation of `~/.acecode` permanently excludes
existing users and prevents later bundled updates.

**Migration**: Startup now compares packaged and user seed revisions for both new and
existing homes. Missing targets are installed, pristine ACECode-owned targets are
updated, and unknown or modified targets are preserved.

#### Scenario: Fresh ACECode home receives seeded skills

- **GIVEN** the user does not have an initialized ACECode home directory
- **WHEN** ACECode initializes the home directory
- **THEN** ACECode installs each default seed skill under the global ACECode skill
  root
- **AND** each installed seed skill has a `SKILL.md` file

#### Scenario: Seeded skills are available in the first session

- **GIVEN** ACECode installs default seed skills during first initialization
- **WHEN** the skill registry performs its first scan in that process
- **THEN** `/skills` can list the seeded skills
- **AND** `skills_list` can return their metadata
- **AND** `skill_view` can load their `SKILL.md` content
