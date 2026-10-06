# Verification — 2026-10-07

## Passed

- `node web/scripts/test-desktop-office.mjs`: 57 checks, no page errors or external requests. Used installed Playwright with Edge. Includes hover delay/re-entry, mouse versus keyboard focus, native control rectangles, pin acknowledgements, close message, and 172/344/860px layouts; preserves the existing 40 office animation/state checks.
- `pnpm test` in `web/`: passed.
- `cmake --build build --config Release --target acecode-desktop acecode_unit_tests --parallel 1`: passed. Existing MSBuild shared-intermediate-directory warnings remain; build was serial. The initial compile caught removal of a constant also used for the room origin; the layout constant was restored while removing only unconditional toolbar hit testing, then the build passed.
- `build/tests/Release/acecode_unit_tests.exe --gtest_filter=DesktopPetLayout.*`: 14/14 passed, including dynamic control hit testing, hidden-region passthrough, invalid rectangle rejection and the shared overlay limit.
- `openspec validate hover-desktop-office-controls --strict`: passed.
- `git diff --check`: passed.
- Impeccable detector: reviewed advisories for inherited palette/radii and compact spacing. New controls reuse the established compact office control appearance; no scene redesign.

## Windows native runtime

Used the repository `dev_desktop.desktop_environment` / `launch_desktop` helpers with the current `build/Release/acecode-desktop.exe`, isolated temporary USERPROFILE/AppData, and no model calls. WebView2 CDP supplied pointer input; Win32 APIs inspected actual window regions, styles and Z order. The environment does not permit moving the physical desktop cursor, so this was automation, not a manual desktop interaction claim.

Eight native checks passed:

1. Default topmost state and no hidden toolbar hit region.
2. Hover restores the region; exit delays hiding; re-entry cancels hiding.
3. Pin click removes `WS_EX_TOPMOST`.
4. A normal main window can be placed above the unpinned pet in real native Z order.
5. Wheel zoom changes the pet size while retaining its unpinned state.
6. Pin click restores `WS_EX_TOPMOST`.
7. Focus left by a pointer click does not keep the toolbar region alive after exit.
8. Close destroys the pet HWND while leaving the main app HWND alive.

Temporary evidence: `C:/Users/shao/AppData/Local/Temp/ace-office-controls-native-q5q5vwbc/result.json` and `native-hover.png`; browser captures: `C:/Users/shao/AppData/Local/Temp/ace-office-browser-JN8kNi/`. The isolated app was closed after verification.

## Boundaries

- macOS floating/normal level and deferred close paths were updated and reviewed from source; no macOS compiler or runtime validation was performed on Windows.
- Installed Desktop packages are not updated by this local build. The pet HTML is embedded directly by CMake, independently of `web/dist`; the user can run the current development build to see the change.
- Existing dirty changes in AGENTS.md, ChannelsSettings.jsx, ComposerSessionControls.jsx and globals.css were left outside this change. No commit or push.

## Release integration — 0.9.36

After implementation, the user authorized release if both Claude App and Codex App had no other active ACECode turn. Claude's local ACECode process reported `idle`; Codex's current runtime had no other in-progress ACECode turn. The earlier retained working-tree changes were reviewed and included through PR #106.

The fixed audit window is 2026-10-05 01:51:29 through 2026-10-07 01:51:29 (Asia/Taipei). All 46 worktrees were inspected. Claude's completed `38eeec9651aa2698d84f1a4817573d841af7bd3b` adds door entrances, report-in hops and faster goodbyes; its animation code merges with the new window controls. Only the browser script's reported check count conflicted; both sets of assertions were retained and the combined 64-check run passed.

The two recent preserved upgrade branches (`6d45f1ce`, `8d64540d`) are already present as patches: reverse apply checks pass outside the combined HTTP test file, and all five introduced HTTP tests are byte-identical in master. No duplicate merge is needed. Remaining worktrees are outside the window, and the nine explicitly retained migration refs remain untouched.

Seed assets, seeding code and tests are unchanged from v0.9.35; revision and manifest remain `2026-10-04.1`. Release binaries are built from the final synchronized master and the release script verifies upgrade behavior and the complete GitHub/aupdate inventory. The final version/commit, CI, hashes and public downloads are recorded in the external release verification JSON rather than adding a post-tag source commit.

![Office controls after animation integration](../../../docs/images/agent-office/office-controls-hover.png)
