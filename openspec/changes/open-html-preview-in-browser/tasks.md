## 1. Implementation

- [x] 1.1 Add local HTML URL conversion and test extensions, workspace roots, Windows/UNC/POSIX paths and reserved characters. Passed `node web/src/lib/htmlPreview.test.js`.
- [x] 1.2 Add the toolbar action and integrate session-owned browser navigation, unsaved-file guard and failure handling; verified against the real App/ChatView with isolated native bridge and filesystem fixtures.

## 2. Validation

- [x] 2.1 Run Web tests/build, strict OpenSpec validation, diff checks and focused browser checks; all passed on 2026-10-08.
- [x] 2.2 Handle asynchronous native page startup before immediate HTML navigation; readiness/failure/cancellation tests, delayed-ready App fixture, and native Windows file loading passed before publication.

## Validation evidence

- `node web/src/lib/htmlPreview.test.js`: passed HTML/HTM case matching, absolute and relative roots, Windows/UNC/POSIX paths, Unicode and reserved-character encoding, and unsupported paths.
- `pnpm i18n:catalog`, `pnpm test`, `pnpm build`: passed. Production compatibility check found no lookbehind among 4519 regular expressions.
- `openspec validate open-html-preview-in-browser --strict`, `git diff --check`: passed.
- Real App/ChatView browser fixture passed toolbar ordering, keyboard activation, session ownership, encoded navigation, unsaved cancel/save, non-HTML exclusion, and creation/navigation error reporting. Evidence: `%TEMP%/ace-html-browser-pTOugM/`.
- Initial fixture attempts selected a non-button Files tab and a collapsed file panel; corrected the fixture to reveal the panel through the real shortcut. No product change was needed.
- Browser proof uses an isolated native bridge fixture. Native package and cross-platform release verification are tracked separately for v1.0.0.
- Native Windows verification exposed a real startup race: page creation returned `ready:false` and immediate navigation was rejected. The action now waits up to 15 seconds for readiness, stops on close/session changes, and reports startup failures. Added deterministic readiness/timeout/closed/cancellation regressions; the real App fixture now rejects navigation during a 200 ms startup delay and passes.
- A packaged v1.0.0 Desktop with the actual readiness helper loaded a real `file://` page containing Unicode, spaces, `#` and `%` in its filename and reported the expected title. Evidence is in the release's `native-html-smoke.json`; updated App captures are under `%TEMP%/ace-html-browser-pit2jD/`.
- Full Web tests/build, i18n generation and strict OpenSpec validation passed again after the readiness fix. The initial native harness required a standard `USERPROFILE/AppData/{Local,Roaming}` hierarchy; this was a test-profile correction, not a production source change.
