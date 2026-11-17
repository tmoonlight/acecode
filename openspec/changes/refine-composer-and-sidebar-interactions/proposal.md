# Proposal

## Why

Users need control over predicted messages and how messages sent during a reply are handled. Folder creation needs a dedicated localized dialog, and a released mouse button must not leave sidebar rows attached to the pointer.

This change records the already implemented drafts being integrated for the next release. It does not claim that the implementation was preceded by these artifacts; final integration validation remains pending.

## What Changes

- Persist predicted-message visibility and the preferred busy-send action in daemon UI preferences, with immediate settings updates and compatible defaults.
- Create folders through a localized dialog with a blank name, a 50 UTF-16-unit limit, explicit confirmation/cancellation, and recoverable errors.
- Cancel pending or active sidebar mouse drags when a movement reports the primary button released.
- Include the completed update-history display reduction in the same release integration.

## Capabilities

### New Capabilities

- `composer-preferences`: Persisted controls for prediction generation and busy-message dispatch.
- `path-picker-folder-creation`: Folder creation dialog lifecycle and validation.
- `sidebar-pointer-drag-lifecycle`: Mouse-button release recovery shared by session and workspace drags.

### Modified Capabilities

None. Existing queue, follow-up recommendation, session-order and workspace-order behavior remains in place; this change adds preference and input-lifecycle contracts around those paths.

## Impact

Configuration serialization, UI preference and follow-up HTTP routes, React settings/composer/path picker/sidebar components, localization, and focused tests. No new dependencies, seed resources, or migration revision. Native Mac tap-to-click behavior remains an explicit manual verification limitation.
