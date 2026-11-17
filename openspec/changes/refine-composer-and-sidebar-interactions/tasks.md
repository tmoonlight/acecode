# Tasks

## 1. Composer preferences

- [x] 1.1 Review the daemon persistence, API validation, disabled-generation gate, and composer dispatch against the added specifications; corresponding config, HTTP, and frontend regression tests are present.
- [x] 1.2 Run the focused native and frontend tests and verify persistence, prediction suppression, and busy-send choices in the complete settings/chat UI; record evidence and limitations. The originating task passed 597 native tests and verified the complete UI with an isolated daemon/model fixture; final integrated frontend tests also pass.

## 2. Folder creation

- [x] 2.1 Review the localized creation dialog, name validation, error recovery and selection flow; name-limit tests and the original task's browser verification are present.
- [x] 2.2 Verify the integrated frontend suite and build include the final folder-dialog changes without regressions. Both commands passed with unchanged source hashes.

## 3. Sidebar dragging

- [x] 3.1 Review the mouse release guard in both session and workspace drag paths; focused regression tests cover held/released buttons and unaffected pointer kinds.
- [x] 3.2 Verify the guard tests and normal/cancelled dragging in a browser; explicitly retain the unperformed Mac hardware check. The production Sidebar passed seven isolated Chromium checks: pending/active release cancellation for sessions and folders, preserved next click, and real held-button reordering for both row types.

## 4. Release integration

- [x] 4.1 Wait for all Claude tasks and their background work, review the final stable diff, and pass strict OpenSpec, source guards, frontend tests/build and applicable native checks. All tasks completed; source hashes remained unchanged through frontend tests/build. Seven source guards, strict OpenSpec and 204 goal/session/upgrade tests passed in addition to the originating task's 597 config/HTTP tests.
- [x] 4.2 Commit and push the integrated source through the required PR, confirm synchronized clean master, and record the source integration reference. Source commit `412b5caf` was merged through PR #191 as `9f6d0659`; canonical master and origin/master were clean and synchronized before version preparation.

## Workflow follow-up

- Run final version CI and the complete stable publishing/mirroring workflow. Record exact tag, packages, sizes, hashes and public verification in the release audit before shutdown.

## Verification limits

- Mac hardware tap-to-click and operating-system drag-lock combinations have not been tested. This is an attempted fix for missing pointer release delivery, not a claim to solve OS drag locking.
- Earlier manual desktop activation and cross-machine sharing checks remain outside this change and are not marked passed.
- In the isolated model fixture, predictions appeared after reopening a completed session but not immediately after a live completion. The originating task reproduced this using the pre-change HEAD frontend; the observation is retained as a separate existing investigation, not represented as fixed here.
