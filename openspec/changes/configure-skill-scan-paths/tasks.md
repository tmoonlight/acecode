## 1. Configuration and discovery

- [x] 1.1 Add optional ordered scan paths and shared resolution; verify config round-trip, empty/reset, environment, workspace isolation and precedence with C++ tests.
- [x] 1.2 Add authenticated configuration API, atomic save and live policy refresh; verify HTTP validation/persistence and listing/runtime agreement, and document the API.

  Verified: Windows MSVC Release `acecode_unit_tests` rebuilt successfully (`--parallel 1`). Focused filter `SkillInitOpencodeTest.*:SkillsHandlerTest.*:ConfigFirstInitTest.*:WebServerHttp.Skill*:CommandsHandlerTest.*:SkillViewToolTest.*:SkillsToolTest.*` passed 80/80 tests. HTTP coverage includes updating an already-created session, daemon body lookup, list refresh, persistence, empty list and reset. Handler tests cover invalid input and write failure preserving configuration.

## 2. Settings

- [x] 2.1 Add the path editor, defaults action, Skills link, search and translations; verify Web unit tests and keyboard navigation/save in browser.

  Verified: `pnpm test`, `pnpm build`, and `node scripts/test-skill-scan-paths.mjs` with installed Edge. Browser covers keyboard link/focus, default values, failed-save draft retention, retry, navigation persistence, empty/reset, light/dark/narrow layouts and English. Impeccable detector returned no findings. This is browser fixture verification, not installed Desktop verification.

- [x] 2.2 Remove synthetic disabled entries from the settings catalog; verify delete/refresh/restore and changed scan scope through native handler and HTTP regression tests, update API documentation, and rerun strict validation.

  Follow-up verified on 2026-10-10: rebuilt Windows MSVC Release tests with `--parallel 1`; the same focused native filter now passes 82/82. New handler and HTTP scenarios cover actual SKILL.md deletion in project/global roots, remaining installed entries, restore with disable preferences intact, and switching to empty scan paths. `node src/lib/skillsSettings.test.js`, strict OpenSpec validation and `git diff --check` pass. This backend-only fix did not require new Web assets; installed Desktop/daemon was not restarted or repackaged.

## 3. Integration

- [x] 3.1 Run focused native tests, pnpm test/build, strict OpenSpec validation and git diff --check; record results and remaining limits.

  Verified on 2026-10-10: all checks above plus `openspec validate configure-skill-scan-paths --strict` and `git diff --check`. Full native suite and installed Desktop packaging/restart were not run; no commit, push or release performed.

- [x] 3.2 Add 20/50/100 (default 50) per-list pagination, shared persisted size, full-catalog search and page clamping; verify pure pagination tests and browser checks for card counts, workspace independence, keyboard navigation, refresh and toggles; run pnpm test/build, translations and strict validation.

  Pagination verified on 2026-10-10: `pnpm test`, `pnpm build`, `node scripts/test-skill-pagination.mjs`, strict OpenSpec and diff checks pass. Installed Edge fixture with 2,300 global skills and 123 workspace skills confirms at most the selected page size is rendered per list, independent pages, search across all entries, toggle count/page preservation, refresh clamping, no repeated scan requests on page/search changes, persisted 20/50/100 selection, invalid preference fallback, keyboard focus, light/dark/narrow layouts and English. Impeccable detector reports no findings. Native API contracts are unchanged; installed Desktop was not repackaged.
