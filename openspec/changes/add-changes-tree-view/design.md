## Context

`SidePanel.jsx` owns the right-side Files/Changes tabs. Git repositories render `GitChangesPanel`, while non-Git sessions render `ChangeCompactList`; both currently build their own flat file rows. `usePreference` is the established safe local-storage abstraction, and the existing SidePanel toolbar and file tree define the visual and accessibility conventions for compact view controls and expandable directories.

The backend already returns the complete path and Git status/stat data required for a hierarchy. This change therefore stays in the WebUI and projects existing rows into a tree without changing the API.

## Goals / Non-Goals

**Goals:**

- Preserve the current flat Changes list while making the tree the default.
- Offer one persistent flat/tree choice per working directory, shared by Git and session-level Changes data within that directory.
- Represent every path segment as a named directory or file row, with directory-first deterministic ordering and expandable directories.
- Preserve file selection, open-review/open-preview actions, desktop context metadata, line counts, and Git status presentation.
- Keep the hierarchy logic pure and covered independently of React rendering.

**Non-Goals:**

- Changing Git collection, diff fetching, refresh/cache behavior, or backend response shapes.
- Adding filtering, staging, bulk actions, rename history, or a third presentation mode.
- Persisting the expansion state of individual directories across component remounts.
- Changing the full diff/details panel or the Files tab tree.

## Decisions

### One preference owner at the SidePanel boundary

`SidePanel` will own one dedicated `usePreference` map keyed by normalized working directory and pass the current directory's selected mode to both Changes adapters. A missing or invalid entry resolves to `tree`. This follows the repository rule that a storage key has one hook owner, prevents one workspace's choice from leaking into another, and avoids duplicate Git/session preferences. A compact two-button group in the existing SidePanel toolbar will use the same token colors, focus treatment, and `aria-pressed` convention as other view controls.

Working-directory keys normalize path separators and trailing separators, and compare Windows drive/UNC paths case-insensitively. This keeps equivalent spellings in one cache bucket without attempting filesystem canonicalization or symlink resolution.

An alternative was adding this field to the global `uiPrefs` object in `App`. That would require plumbing through `App` and `ChatView` even though only `SidePanel` consumes it, increasing the change surface without improving synchronization.

### Pure tree projection with original rows retained

A new pure library will normalize separators, make paths workspace-relative when possible, create explicit directory/file nodes, and recursively sort directories before files by case-insensitive display name. File nodes retain the original row and original path, so callbacks, Git diff lookups, absolute-path metadata, and status/stat information do not depend on display normalization.

Separate directory and file collections allow an unusual same-name file/directory pair to remain representable. Empty/invalid paths fall back to a usable file label instead of being dropped.

### Shared compact file-list renderer

Git and session adapters will map their existing data into one `ChangeFileList` renderer. Flat mode will preserve the current filename/parent-path layout. Tree mode will render directory rows with chevrons and folder icons, and file rows at their path depth without the redundant parent subtitle. Git rows retain the status letter and deletion reveal guard; session rows retain their file icon and addition/deletion counts.

Directories start expanded so switching modes never hides existing changes unexpectedly. The renderer records only explicitly collapsed paths. Activating an externally selected file removes its ancestor paths from that set so the selected row remains revealable and scrollable.

### Scoped styles and stable desktop metadata

New styles will be scoped to the Changes list, reuse existing CSS variables, inherit the application sans font, and avoid changes to the Files tree or review details. File rows in either mode will retain `data-change-compact-file` and `data-desktop-review-*` attributes so selection scrolling and native context actions continue to work.

## Risks / Trade-offs

- **[Deep paths can leave little horizontal room in a 280px panel]** -> Bound indentation per level, keep the filename column shrinkable, and preserve full paths in row titles.
- **[A refreshed list can remove directories that remain in collapsed state]** -> Treat collapse state as an inert set; stale entries do not render and disappear naturally when the component remounts.
- **[Extracting a shared renderer could regress Git-only status or preview actions]** -> Keep those behaviors explicit row fields/props and add architecture assertions that both adapters use the shared renderer and retain the required desktop metadata.
- **[Tree ordering differs from source insertion order]** -> Limit sorting to tree mode; flat mode continues to use the source order.

## Migration Plan

The working-directory map uses a new dedicated storage-key version because the previous scalar preference cannot be assigned reliably to one workspace. Existing unscoped values are ignored, so every workspace starts from the new `tree` default until the user chooses a mode there. Rollback ignores the harmless scoped map.

## Open Questions

None.
