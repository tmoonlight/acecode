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

## Validation evidence (2026-10-07)

- Focused Node tests: appShortcuts, channelsSettings, settingsNavigation and browserDefaults passed.
- `pnpm i18n:catalog`, final `pnpm test`, final `pnpm build`, strict OpenSpec validation and `git diff --check` passed. Build compatibility scan checked 4518 regex literals with no lookbehind.
- `node scripts/test-busy-reasoning.mjs` passed existing busy-update, failure rollback, late response and default restoration scenarios.
- `node scripts/test-shortcuts-and-channels.mjs` passed nine browser groups using actual App/Settings/ChatView and isolated daemon/WebSocket fixtures: search/sticky/focus, create/history/drafts, panels, reasoning/IME/menus, busy/stop, xterm forwarding, channel state/approval/connect controls, narrow English search and four viewport/theme combinations.
- Final screenshots and browser results: `C:/Users/shao/AppData/Local/Temp/ace-shortcuts-channels-27LTYw/`. Full logs: `%TEMP%/ace-shortcuts-web-tests-final.log` and `%TEMP%/ace-shortcuts-web-build-final.log`.
- Browser executables reused through `ACE_PLAYWRIGHT_MODULE` and `ACE_CHROMIUM_EXECUTABLE`; no daemon, real credentials or vendor messages were used.
- First visual pass found narrow card titles collapsing and low-contrast monochrome Feishu artwork; fixed both and confirmed in the final pass. Impeccable detector returned no findings.
- Concurrent update-button relocation changes in App/TopBar/Sidebar and unrelated style/modal-test edits were preserved. This change has not been committed, published or packaged into the installed Desktop; native Windows/macOS accelerator integration remains outside the Web proof.
