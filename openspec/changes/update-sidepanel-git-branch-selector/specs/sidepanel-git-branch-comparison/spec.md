## ADDED Requirements

### Requirement: Git metadata distinguishes remote and local branches
The daemon SHALL return safe local branch short names in `branches` and safe, locally available remote-tracking branch short names in `remote_branches` for a Git workspace. It MUST omit symbolic remote aliases and MUST NOT contact a remote while collecting this metadata.

#### Scenario: Fetched branches are reported by kind
- **WHEN** a workspace has local branches `main` and `feature/local` plus fetched refs `origin/main` and `origin/feature/remote`
- **THEN** `/api/git/info` reports the local names in `branches` and the remote-tracking names in `remote_branches`

#### Scenario: Symbolic remote HEAD is excluded
- **WHEN** `refs/remotes/origin/HEAD` is a symbolic alias for the remote default branch
- **THEN** `origin/HEAD` is not returned as a selectable remote branch

### Requirement: Comparison bases prefer remote branches and include local branches
The right-side Git changes selector SHALL initially select the backend-verified default remote base when one exists. Its candidate list SHALL place that base first, then the remaining remote-tracking branches, local branches, and the `HEAD` fallback without duplicate entries. When no verified remote default exists, the selector SHALL initially select `HEAD` while still offering discovered local branches.

#### Scenario: Remote default is selected first
- **WHEN** Git metadata reports `origin/main` as `default_base`, additional remote branches, and local branches
- **THEN** `origin/main` is the initial comparison base and appears once at the start of the selector

#### Scenario: Local branches are selectable comparison bases
- **WHEN** Git metadata includes local branches other than the current branch
- **THEN** those local branch names appear in the selector after remote-tracking branch choices

#### Scenario: Pure-local repository keeps HEAD fallback
- **WHEN** Git metadata has local branches but no verified remote default or remote-tracking branches
- **THEN** `HEAD` is initially selected and the local branches remain available as alternate comparison bases

#### Scenario: Older daemon payload remains usable
- **WHEN** Git metadata omits `remote_branches`
- **THEN** the selector continues to render from `default_base`, local `branches`, and `HEAD` without an error

### Requirement: Branch comparison selection is read-only and consistent
Selecting a remote or local branch in the right-side Git changes selector SHALL change only the comparison base. The list and file-detail requests MUST use the selected ref, and the workspace's checked-out branch MUST remain unchanged.

#### Scenario: Selecting a local branch compares without checkout
- **WHEN** the user selects a local branch from the comparison selector
- **THEN** the changes list and subsequently opened file diff use that branch as their base and no checkout request is made

#### Scenario: Selecting a remote branch compares without checkout
- **WHEN** the user selects a remote-tracking branch from the comparison selector
- **THEN** the changes list and subsequently opened file diff use that remote ref as their base and no checkout request is made
