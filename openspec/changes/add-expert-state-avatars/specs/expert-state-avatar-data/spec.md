## ADDED Requirements

### Requirement: Expert packages can declare three optional state avatars
Agent and Team expert manifests SHALL accept an optional `stateAvatars` object with the fixed keys `working`, `needs_attention`, and `idle`. Each configured value MUST be a non-empty expert-package-relative image path. API and frontend DTOs SHALL represent the same known values as `state_avatars`. Existing packages that contain only `avatar` or no avatar fields MUST remain valid without migration.

#### Scenario: Load all three state avatars
- **WHEN** an expert manifest declares valid package-relative images for `working`, `needs_attention`, and `idle`
- **THEN** the Registry and expert detail DTO expose the three values with their original state meanings

#### Scenario: Load an old expert package
- **WHEN** an expert package has an existing `avatar` field and no `stateAvatars` object
- **THEN** the package remains discoverable and its main avatar behavior is unchanged

#### Scenario: Configure a team package
- **WHEN** a Team manifest declares valid state avatars
- **THEN** the same state-avatar data contract applies without copying any member expert avatar

### Requirement: Missing state avatars fall back to the main avatar
Resolving an effective avatar for `working`, `needs_attention`, or `idle` SHALL use that state's configured image when present and SHALL otherwise use the expert's existing main `avatar`. If neither the requested state nor the main avatar is configured and readable, the system SHALL preserve the existing no-avatar/404 behavior. A configured state image that becomes unreadable after successful discovery SHALL safely fall back to the validated main avatar for that read.

#### Scenario: Resolve an unconfigured state
- **WHEN** an expert has a main avatar but does not configure `needs_attention`
- **THEN** the effective `needs_attention` avatar is the main avatar

#### Scenario: Resolve a configured state
- **WHEN** an expert configures a readable `working` avatar
- **THEN** the effective `working` avatar is that state-specific image rather than the main avatar

#### Scenario: Resolve with no images
- **WHEN** an expert has neither a state-specific image nor a main avatar
- **THEN** the resolver returns the existing no-avatar result without inventing an asset

#### Scenario: State file disappears after discovery
- **WHEN** a validated state image becomes unreadable before an HTTP read and the main avatar remains readable
- **THEN** the request returns the main avatar through the same safe serving path

### Requirement: State avatar paths and files are strictly validated
Every configured state avatar SHALL resolve to an existing regular file contained by the expert package root and MUST NOT escape through absolute paths, parent traversal, or links to outside the package. State avatars SHALL accept PNG, JPEG, GIF, WebP, BMP, and ICO under the existing avatar size limit. Unknown state keys in a current API write SHALL be rejected, while unknown keys already owned by a future-compatible manifest SHALL remain preserved during unrelated updates.

#### Scenario: Reject path traversal
- **WHEN** a state avatar path resolves outside the expert package
- **THEN** create/update or package validation fails without reading or serving the outside file

#### Scenario: Reject a missing configured file
- **WHEN** a manifest explicitly references a state image that does not exist at validation time
- **THEN** the package reports an invalid state-avatar configuration instead of silently accepting the broken reference

#### Scenario: Reject an unsupported type
- **WHEN** a configured state avatar uses a file type outside the supported image allowlist
- **THEN** validation fails and the unsupported bytes are never served as an avatar

#### Scenario: Preserve a future manifest key
- **WHEN** an existing manifest contains an unknown `stateAvatars` extension key and an older client updates an unrelated expert field without sending `state_avatars`
- **THEN** the unknown key and its package asset remain unchanged

### Requirement: GIF state avatars retain their original animation bytes
GIF SHALL be a supported state-avatar type. Persisting, copying, updating, previewing, or serving a GIF state avatar MUST NOT decode, resize, transcode, flatten, or replace the original file. The avatar response SHALL use `Content-Type: image/gif` and return the original byte sequence so a future `<img>` consumer can play its animation.

#### Scenario: Serve an animated working avatar
- **WHEN** `working` references a valid animated GIF and that state is requested
- **THEN** the response body matches the original GIF bytes and its content type is `image/gif`

#### Scenario: Update another expert field
- **WHEN** an expert with a GIF state avatar changes its description or instructions
- **THEN** the GIF file remains byte-for-byte present in the updated package

### Requirement: CRUD round-trips state avatars without losing package data
Expert create/update parsing, `ExpertDefinition`, detail serialization, and atomic package materialization SHALL carry the three known state-avatar references. An update payload that omits `state_avatars` MUST preserve the existing object and files. A payload that includes the object SHALL be authoritative for the three known keys: supplied keys are set, omitted known keys are cleared, and an empty object clears all three known references without deleting copied image files or unknown manifest data.

#### Scenario: Omit state avatars during update
- **WHEN** an older or unrelated editor updates an expert without a `state_avatars` field
- **THEN** all existing state-avatar references and files remain unchanged

#### Scenario: Clear one known state
- **WHEN** the editor submits `working` and `idle` but omits a previously configured `needs_attention` key inside an explicit `state_avatars` object
- **THEN** the `needs_attention` reference is removed while the other known values and image assets are preserved

#### Scenario: Clear all known states
- **WHEN** the editor submits an explicit empty `state_avatars` object
- **THEN** the three known manifest references are removed without treating that action as permission to delete package files

#### Scenario: Preserve unrelated package resources
- **WHEN** state-avatar values are edited in a package containing bundled Skills, resources, a main avatar, and unknown manifest fields
- **THEN** every unedited resource and field remains present after the atomic update

### Requirement: HTTP DTOs expose safe state avatar data and effective URLs
Expert responses SHALL retain the existing `avatar_url` and SHALL expose `state_avatar_urls` for the fixed states when an effective main or state image exists. Managed expert detail SHALL expose configured `state_avatars` as package-relative references for editing. Responses MUST NOT disclose resolved host absolute paths. The existing avatar endpoint SHALL accept an optional `state` query for the three known states, preserve its authentication, workspace, size, MIME, cache, and `nosniff` protections, and reject unknown state values.

#### Scenario: Read state avatar DTO data
- **WHEN** a managed expert with state images is requested through the detail API
- **THEN** the response contains safe relative configuration and authenticated state-avatar URLs but no absolute filesystem path

#### Scenario: Request a known state URL
- **WHEN** a client requests the expert avatar endpoint with `state=idle`
- **THEN** the endpoint returns the effective idle image using the same security headers and limits as the main avatar endpoint

#### Scenario: Request an unknown state
- **WHEN** a client requests the avatar endpoint with an unsupported state value
- **THEN** the endpoint returns a client error and does not fall back to an arbitrary file

### Requirement: Managed editing can configure and clear existing package assets
The managed Expert Editor SHALL represent the three optional state-avatar paths for both Agent and Team forms, show their configured/effective previews when available, and let the user clear a known reference. The UI SHALL identify the values as package-relative paths to existing image files and MUST NOT imply that this data-layer phase uploads, generates, or converts files. Server validation errors SHALL keep the editor and unsaved values open.

#### Scenario: Edit existing state paths
- **WHEN** a user opens a managed expert whose package already contains state image files
- **THEN** the editor shows the configured paths and effective previews and round-trips them on save

#### Scenario: Clear a state path
- **WHEN** a user clears the `idle` path and saves
- **THEN** the explicit update removes the known `idle` reference and future idle resolution falls back to the main avatar

#### Scenario: Enter an invalid package path
- **WHEN** a user saves a state-avatar path that the server rejects
- **THEN** the editor remains open with the entered value and displays the save error

### Requirement: Expert Manager understands the same state-avatar contract
The versioned `expert-manager` Skill, JSON/avatar references, and validation scripts SHALL document and validate `stateAvatars` with the same three states, path containment, supported image types, main-avatar fallback, and GIF preservation semantics as the runtime. Initialization MUST NOT create placeholder images or write state paths before real files exist. Seed manifest metadata SHALL be refreshed through the existing ownership-safe upgrade mechanism.

#### Scenario: Validate an Expert Manager package
- **WHEN** `expert-manager` validates a package with valid PNG and GIF state avatars
- **THEN** validation succeeds using the same state names and type rules as ACECode runtime discovery

#### Scenario: Initialize without state images
- **WHEN** an expert skeleton is initialized without real state image assets
- **THEN** no `stateAvatars` paths or placeholder image files are fabricated

#### Scenario: Preserve a user-modified seeded Skill
- **WHEN** the bundled Expert Manager version is refreshed and the installed copy was modified by the user
- **THEN** the ownership-safe Seed transaction does not overwrite that user-modified Skill

### Requirement: Runtime state switching remains explicitly deferred
This change MUST NOT infer or persist a current avatar state from Agent, subagent, question, confirmation, or idle events and MUST NOT add automatic avatar switching UI. After the data-layer implementation and verification complete, the project's single TODO entry point, `docs/ACECode 待办.md`, SHALL receive a new unfinished item describing future runtime selection of `working`, `needs_attention`, and `idle` avatars from subagent state. That item MUST remain unfinished until the separate dynamic behavior is implemented and verified.

#### Scenario: Complete the data-layer change
- **WHEN** state-avatar data, editing, persistence, API, GIF, and Expert Manager verification pass
- **THEN** the TODO list records dynamic subagent-state avatar switching as unfinished work

#### Scenario: Observe current runtime behavior
- **WHEN** an Agent or subagent changes between running, waiting-for-user, and idle during this phase
- **THEN** no new automatic avatar switch occurs solely because this data-layer change is installed
