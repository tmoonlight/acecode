## Why

Linux WebKitGTK can resolve ACECode's missing first-choice console font to a proportional substitute instead of trying the remaining font stack. xterm then allocates every character the width of `W`, producing visibly sparse text. The product must handle this without changing the user's Linux fonts or fontconfig.

## What Changes

- Select one terminal font using measured ASCII advances at the terminal's actual size, including normal and bold text.
- Skip proportional or invalid substitutes and fall back to the browser's standalone `monospace` family.
- Reuse this selection for every console tab on Desktop and Web, preserving PTY behavior, controls, font size and layout.
- Cover missing named fonts and the observed WebKit behavior with regression tests and a real Linux WebKit/xterm rendering check.

## Capabilities

### New Capabilities

- `console-dock-ui`: Add a monospaced font-fallback contract to the capability already introduced by the unarchived `add-console-dock` change; no canonical spec for this capability exists yet.

### Modified Capabilities

None.

## Impact

- `web/src/components/ConsoleDock.jsx` and a focused font-selection helper/test under `web/src/lib/`.
- No new dependencies, backend/API changes, system font installation, fontconfig edits, or settings UI.
