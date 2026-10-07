## 1. Implementation

- [x] 1.1 Add local HTML URL conversion and test extensions, workspace roots, Windows/UNC/POSIX paths and reserved characters. Passed `node web/src/lib/htmlPreview.test.js`.
- [x] 1.2 Add the toolbar action and integrate session-owned browser navigation, unsaved-file guard and failure handling; verified against the real App/ChatView with isolated native bridge and filesystem fixtures.

## 2. Validation

- [x] 2.1 Run Web tests/build, strict OpenSpec validation, diff checks and focused browser checks; all passed on 2026-10-08.

## Validation evidence

- `node web/src/lib/htmlPreview.test.js`: passed HTML/HTM case matching, absolute and relative roots, Windows/UNC/POSIX paths, Unicode and reserved-character encoding, and unsupported paths.
- `pnpm i18n:catalog`, `pnpm test`, `pnpm build`: passed. Production compatibility check found no lookbehind among 4519 regular expressions.
- `openspec validate open-html-preview-in-browser --strict`, `git diff --check`: passed.
- Real App/ChatView browser fixture passed toolbar ordering, keyboard activation, session ownership, encoded navigation, unsaved cancel/save, non-HTML exclusion, and creation/navigation error reporting. Evidence: `%TEMP%/ace-html-browser-pTOugM/`.
- Initial fixture attempts selected a non-button Files tab and a collapsed file panel; corrected the fixture to reveal the panel through the real shortcut. No product change was needed.
- Browser proof uses an isolated native bridge fixture. Native package and cross-platform release verification are tracked separately for v1.0.0.
