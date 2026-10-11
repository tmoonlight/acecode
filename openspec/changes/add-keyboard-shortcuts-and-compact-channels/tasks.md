## 1. Keyboard actions

- [x] 1.1 Add shared shortcut catalog, matching/context guards and reasoning stepping; verify pure tests cover modifiers, IME, platform labels, bounds and default depth.
- [x] 1.2 Wire App and active conversation handlers, terminal forwarding and shortcut hints; verify real keypresses trigger existing actions once and preserve navigation guards.
- [x] 1.3 Add searchable shortcut settings with sticky search and localized copy; verify search, settings discovery and scroll behavior.

## 2. Message channels

- [x] 2.1 Bundle all seven upstream platform icons and source records; verify assets render in cards/dialogs and production build.
- [x] 2.2 Stabilize card layout, move management before connection and retain approvals/recovery in management; verify state transitions preserve card height and existing operations.

## 3. Integration validation

- [x] 3.1 Run focused and full Web tests, i18n generation, build, strict OpenSpec validation and diff checks; record results.
- [x] 3.2 Run browser checks across desktop/narrow, light/dark and keyboard/overlay/IME contexts; record evidence and separate Web proof from installed Desktop verification.

## 4. Editable bindings (2026-10-11)

- [x] 4.1 Implement observable persistent overrides, capture/validation/conflict/reset logic; verify default compatibility, modifier collisions and storage failure tests.
- [x] 4.2 Connect effective bindings to global/contextual handlers, labels, terminal and presentation forwarding; verify old bindings stop working and new bindings act once.
- [x] 4.3 Add edit icons and recording UI with save/cancel/default controls and localized errors; verify keyboard focus, recording isolation and reload persistence in browser.
- [x] 4.4 Run focused/full Web tests, production build, i18n, strict OpenSpec and scoped diff checks; record browser evidence and native verification limits.

## Editable-binding validation evidence (2026-10-11)

- All 20 catalog rows expose editable bindings; the history row has independent previous/next editors (21 edit buttons).
- `pnpm i18n:catalog`, final `pnpm test` (3101 pass records, exit 0), `pnpm build`, strict OpenSpec validation and `git diff --check` passed. The production compatibility scan checked 4524 regex literals with no lookbehind.
- `node scripts/test-shortcut-editing.mjs` passed recording isolation, cancel, conflicts, reserved keys, immediate replacement, reload persistence, reactive hints, xterm forwarding, cross-window storage updates, failed-write preservation, send/newline/history/find, dismissal and individual reset checks.
- `node scripts/test-preview-presentation.mjs` passed existing preview regressions plus actual Markdown save rebinding and live sandboxed PPT shortcut updates without reloading slides. Its fixture now derives dirty state from draft/baseline text so saving is actually exercised.
- Browser screenshots/results: `C:/Users/shao/AppData/Local/Temp/ace-shortcut-editing-Kjdf4t/`; preview regression screenshots: `C:/Users/shao/AppData/Local/Temp/ace-preview-presentation-q65JeG/`.
- Full-suite/build logs: `%TEMP%/ace-shortcut-edit-web-tests-final.log` and `%TEMP%/ace-shortcut-edit-build-final.log`. Impeccable detector returned no findings; desktop light and narrow dark editor screenshots were inspected.
- Existing source-based tests were updated to follow the shared matcher; behavior tests retain the default desktop Ctrl+Enter newline contract, IME/keyCode 229 protections and legacy alternate-layout/numpad matching. A remapped desktop newline no longer falls through into sending a message.
- Browser tests use isolated storage and mocked APIs. No real sessions, channels or credentials were changed. Unrelated dirty work remains preserved; no commit, release or installed Desktop package rebuild was performed.

## Prior validation evidence (2026-10-07)

- Focused Node tests: appShortcuts, channelsSettings, settingsNavigation and browserDefaults passed.
- `pnpm i18n:catalog`, final `pnpm test`, final `pnpm build`, strict OpenSpec validation and `git diff --check` passed. Build compatibility scan checked 4518 regex literals with no lookbehind.
- `node scripts/test-busy-reasoning.mjs` passed existing busy-update, failure rollback, late response and default restoration scenarios.
- `node scripts/test-shortcuts-and-channels.mjs` passed nine browser groups using actual App/Settings/ChatView and isolated daemon/WebSocket fixtures: search/sticky/focus, create/history/drafts, panels, reasoning/IME/menus, busy/stop, xterm forwarding, channel state/approval/connect controls, narrow English search and four viewport/theme combinations.
- Final screenshots and browser results: `C:/Users/shao/AppData/Local/Temp/ace-shortcuts-channels-27LTYw/`. Full logs: `%TEMP%/ace-shortcuts-web-tests-final.log` and `%TEMP%/ace-shortcuts-web-build-final.log`.
- Browser executables reused through `ACE_PLAYWRIGHT_MODULE` and `ACE_CHROMIUM_EXECUTABLE`; no daemon, real credentials or vendor messages were used.
- First visual pass found narrow card titles collapsing and low-contrast monochrome Feishu artwork; fixed both and confirmed in the final pass. Impeccable detector returned no findings.
- Concurrent update-button relocation changes in App/TopBar/Sidebar and unrelated style/modal-test edits were preserved. This change has not been committed, published or packaged into the installed Desktop; native Windows/macOS accelerator integration remains outside the Web proof.
