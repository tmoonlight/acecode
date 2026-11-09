## Why

File-level controls overlap the document and move between formats. The breadcrumb row provides a consistent place to find them and to open the displayed file's folder.

## What Changes

- Move file-level preview actions into one overflow menu at the right edge of the breadcrumb row.
- Offer the existing file-manager action for the current file in both this menu and the file detail context menu on supported desktop hosts.

## Capabilities

### Modified Capabilities

- `file-preview-wrap-toggle` — expose the wrap command in the file actions menu.

### New Capabilities

- `file-detail-actions` — group commands by their applicability to the current preview.

## Impact

Web file previews, their desktop context menu, and related UI tests.
