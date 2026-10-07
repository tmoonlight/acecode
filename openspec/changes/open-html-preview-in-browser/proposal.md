## Why

HTML files currently show source without a direct way to view the local page. Users need a browser action next to the preview's wrap and copy controls.

## What Changes

- Add a globe action for `.html` and `.htm` previews, case-insensitive.
- Open the saved local file through a correctly encoded `file://` URL in a new ACECode browser tab owned by the current session.
- Preserve the existing unsaved-file save/discard/cancel guard and report browser failures.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `side-preview-rendering`: Local HTML preview toolbar action.

## Impact

Web preview components, browser-tab coordination, path conversion tests and browser fixtures. No new dependency or native bridge protocol is needed. Included in the user-requested v1.0.0 release.
