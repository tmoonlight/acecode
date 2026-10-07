# Verification — 2026-10-07

- `pnpm test`: passed the full Web suite, including preferences, native-close response race, menu/nav order and existing session controller teardown coverage.
- `pnpm build`: passed production build and regex compatibility check. Preview HTML comes from the existing native embed; no GIF/video or duplicate office asset was added to the Web bundle.
- `node web/scripts/test-virtual-office-preferences.mjs` with local Playwright/Edge: passed menu/icon placement, default off, opt-in, settings/menu synchronization, native-close notification, polling/subscription teardown and reopening, sandboxed offline preview, light/dark and 390/1100px layouts, pause/resume, reduced motion and invitation acknowledgement. No browser errors or external requests.
- `node web/scripts/test-desktop-office.mjs`: 70 checks passed, no errors or external requests.
- Windows Release `acecode_unit_tests`: 20 tests passed (`DesktopOfficeServiceTest.*:DesktopPetLayout*`), including version gate, persisted choice, once-only claim across restarts, close/reopen, stale callbacks, environment disable and write/open failures.
- Windows desktop compiled and linked using existing build libraries, with executable and PDB redirected to an isolated temporary validation directory because the normal executable is in use. No existing user process was stopped. Existing MSBuild directory warnings remain; zero compilation/link errors in the successful build.
- Isolated Windows native runtime: first-navigation bridges available; default has no pet window; enabling creates it; a snapshot/title reaches the real WebView2 scene; native close saves disabled; reenable creates another window; disabling destroys it; main app stays alive. Test process and daemon exited afterward.
- Strict layer lint: zero findings after tracking new source files. Strict final ownership check, OpenSpec strict validation and diff whitespace check passed. UI detector reported no findings.

macOS was source-reviewed only: shared lifecycle/preferences and early bridge registration are applied to the WKWebView host. A Mac build/runtime is not claimed. Follow the handoff in `docs/desktop-agent-office.md` for session linkage, close/reopen, restart and first-run invitation verification.

Current project version remains 0.9.36. The invitation gate intentionally activates at 0.9.37 and later stable versions; version thresholds are covered by native tests. This change requests code delivery only, with no release tag or package publication.

## Follow-up during 0.9.37 release validation

With the native version gate active, a clean-profile startup exposed a race: the welcome claim could finish while the existing guided tour was preparing, and the queued welcome then blocked that tour while waiting for it. The invitation had been acknowledged but neither surface appeared. The provisional update entry was withdrawn and the package job cancelled before GitHub Release publication.

The fix distinguishes queued and visible invitations; an already preparing/running tour can finish first. Regression tests check this interaction with `shouldPrepareDesktopGuidedTour`. Full Web tests/build passed again. An isolated native v0.9.37 process using the corrected production Web build passed automatic invitation display, dismissal without enabling, once-only acknowledgement, snapshot/title delivery, native close persistence, same-process reopening and disabling. Native evidence: `C:/Users/shao/AppData/Local/Temp/ace-office-optin-native-xv7o4_q8/result.json`. Source version is now 0.9.37; publication recovery is tracked separately in the release evidence.
