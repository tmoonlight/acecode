## Why

The Changes tab currently presents every changed file as a flat row, so repeated parent paths consume space and make changes across several directories difficult to understand. Users need an optional hierarchy that exposes the affected project structure without losing the compact flat list they already use.

## What Changes

- Add a persistent flat/tree view switch to the right-side Changes tab.
- Build a directory hierarchy from both Git-level change rows and session-level structured change rows.
- Let users expand and collapse directory rows while keeping changed-file selection, navigation, line counts, and Git status badges available on file rows.
- Keep the current flat list as the default and preserve existing loading, empty, error, refresh, base-selection, and file-preview behavior.

## Capabilities

### New Capabilities

- `changes-tree-view`: Covers switching the Changes tab between flat and hierarchical file presentation, directory expansion, and unchanged file-row actions/status information.

### Modified Capabilities

None.

## Impact

The change is confined to the WebUI Changes-tab presentation and preference state under `web/src/`. It adds no daemon/API fields and no third-party dependencies. Both Git and non-Git session change sources will use the same tree projection and list renderer, with focused Node tests plus the existing Web test/build gates.
