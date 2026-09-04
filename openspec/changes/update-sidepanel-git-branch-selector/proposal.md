## Why

The right-side Git changes panel currently offers only the verified default remote base and `HEAD`, even though the daemon already reports local branches. Users cannot compare the working tree against another fetched remote branch or another local branch from the panel.

## What Changes

- Extend Git workspace metadata with safe remote-tracking branch refs while keeping local branch refs available.
- Build the comparison selector with the verified default remote branch first, followed by the remaining remote branches, local branches, and the existing `HEAD` fallback without duplicates.
- Keep selector changes read-only: choosing any branch changes the comparison base and refreshes the list/detail views without checking out that branch.
- Cover remote/default ordering, local branch comparison, compatibility fallbacks, and API serialization with focused tests and protocol documentation.

## Capabilities

### New Capabilities
- `sidepanel-git-branch-comparison`: Branch discovery, ordering, selection, and read-only comparison behavior for the right-side Git changes panel.

### Modified Capabilities

None.

## Impact

- Git metadata collection and `/api/git/info` response fields.
- Right-side Git changes selector state and candidate construction.
- Git collector and Web pure-state tests.
- Daemon API documentation.
