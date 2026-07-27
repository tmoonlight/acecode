## ADDED Requirements

### Requirement: Expert editor separates basic and advanced configuration
The expert editor SHALL provide tabs named `基础信息` and `高级功能`. Changing tabs SHALL preserve unsaved values in both tabs until the user saves or explicitly cancels.

#### Scenario: Edit across both tabs
- **WHEN** a user changes basic fields, switches to advanced configuration, and returns
- **THEN** all unsaved basic and advanced selections remain intact

### Requirement: Basic information uses the correct expert field semantics
The basic tab SHALL allow editing one expert display name, multiple Tags, introduction, expertise, opening prompts, and work style/system instructions. It MUST NOT expose or persist a separate author/call-name value. Tags SHALL be multi-valued. Expertise and opening prompts SHALL be distinct ordered lists, with the editor presenting one item per line rather than a comma-delimited category field.

#### Scenario: Save line-separated expertise and prompts
- **WHEN** a user enters three expertise lines and two opening-prompt lines
- **THEN** the saved expert contains three expertise items and two separate opening prompts in the entered order

#### Scenario: Reopen the expert
- **WHEN** the saved expert is edited again
- **THEN** Tags, expertise, opening prompts, introduction, and instructions round-trip without changing meaning

### Requirement: Expert authoring validates required and structured values
The editor SHALL validate required identity fields, duplicate or invalid expert identifiers, blank list items, and member references before submitting. Validation errors SHALL be associated with the relevant field and a failed request SHALL keep the editor and unsaved data open.

#### Scenario: Save an invalid expert
- **WHEN** required identity data is missing or the identifier is invalid
- **THEN** the editor blocks submission and identifies the field that must be corrected

#### Scenario: Server-side save fails
- **WHEN** the server rejects a create or update request
- **THEN** the editor remains open with the user’s current values and presents the error

### Requirement: Updating an expert is lossless for package data
Updating a managed expert SHALL change only fields represented by the submitted draft and SHALL preserve avatar files, packaged resources, bundled Skill content, and unknown forward-compatible manifest fields that the editor did not modify. Workspace-owned read-only experts SHALL not be silently converted into global managed copies.

#### Scenario: Update an expert with packaged resources
- **WHEN** an expert with an avatar, resources, packaged Skills, and unknown manifest extensions is edited
- **THEN** those unedited files and fields remain present after the update

#### Scenario: Edit a workspace expert
- **WHEN** a user attempts to edit an expert whose source is read-only workspace data
- **THEN** the UI explains the restriction and the API does not overwrite that package

### Requirement: Team editor manages referenced members and one lead
The expert-team editor SHALL let users add existing experts, remove members, view each member’s identity and profession, and designate exactly one selected member as `主理人`. The lead SHALL also count as a team member, and duplicate, missing, self-referential, out-of-scope, or nested-team references MUST be rejected.

#### Scenario: Create a valid team
- **WHEN** the user selects multiple experts and designates one selected expert as lead
- **THEN** the saved team references those expert IDs once and identifies exactly that lead

#### Scenario: Remove the current lead
- **WHEN** the user removes the lead from the member list
- **THEN** the editor requires or selects a valid remaining lead before save

#### Scenario: Add members from the picker
- **WHEN** the user opens `添加专家`
- **THEN** a searchable expert-only picker shows selected state and supports confirming multiple members

### Requirement: Expert and team lifecycle actions are consistent
The catalog SHALL provide create, edit, delete, detail, and `派遣` actions where allowed. Destructive deletion SHALL require explicit confirmation, and successful mutations SHALL refresh catalog state without forcing a full-page navigation.

#### Scenario: Delete a managed expert
- **WHEN** the user confirms deletion of a deletable global expert
- **THEN** the expert is removed and the catalog updates in place

#### Scenario: Cancel deletion
- **WHEN** the user dismisses the delete confirmation
- **THEN** no expert package or catalog state is changed

### Requirement: Expert terminology is consistent across surfaces
User-facing expert UI SHALL use `Tag` for non-exclusive filters, `擅长领域` for capability topics, `开场白` for reusable conversation starters, `派遣` for applying a component, and `专家团` for a referenced team. These terms MUST NOT be substituted with category semantics or used interchangeably.

#### Scenario: Compare catalog, detail, and editor
- **WHEN** the same expert is shown across the catalog card, detail dialog, and editor
- **THEN** each field uses the same defined term and content semantics on every surface
