## Why

Files listed at the end of a conversation turn cannot be saved elsewhere or copied through their card. Users need the four direct file actions shown in the supplied reference, without another menu implementation.

## What Changes

- Add Save as, Copy path, Copy file contents, and Open in Explorer to turn file cards, in that order.
- Reuse the shared context menu, absolute-path resolver, clipboard, native save picker, file readers, and Explorer action.
- Save the current file bytes; copy current text rather than a diff or truncated preview. Report missing, binary, or oversized text through existing errors.
- Preserve file preview, turn-specific changes, and folding.

## Capabilities

### New Capabilities
- `turn-file-actions`: File operations on conversation turn file cards.

### Modified Capabilities
None.

## Impact

TurnFileList, DesktopContextMenu, shared file/download helpers, file routes and daemon API documentation. No new dependencies. Desktop Save as uses the existing native picker; ordinary browsers download through the existing authenticated binary reader.
