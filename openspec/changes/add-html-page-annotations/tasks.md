# Tasks

## 1. Element annotation closed loop (#194)

- [ ] 1.1 Add document-bound annotation runtime and restricted native/trusted commands; verify stale-document and untrusted-command rejection.
- [ ] 1.2 Add toolbar entry, element outline/control and host-controlled popup over the browser; verify save creates a numbered pin and pending count without submission or file writes.
- [ ] 1.3 Preserve saved pins on mode exit and immutable saved records; verify behavior and existing source-selection regression tests.

## 2. Reversible style previews (#195)

- [ ] 2.1 Implement all approved style fields through independent stylesheet layers and actual-difference filtering; verify every property and untouched original inline styles.
- [ ] 2.2 Add default-off current-ratio lock and draft-only cancellation over saved layers; verify repeated edits of the same target.
- [ ] 2.3 Add original-view comparison retaining pins and restoring previews; verify round trips and mode exit.

## 3. Visible text previews (#196)

- [ ] 3.1 Implement reversible text-node previews preserving child-node identity; verify nested elements, text changes and exact restoration.
- [ ] 3.2 Integrate saved/draft/original text layers and actual differences; verify cancellation retains earlier saved text.

## 4. Multiple targets and region (#197)

- [ ] 4.1 Support adding multiple elements to one draft with shared description/pin; verify target switching and save.
- [ ] 4.2 Support one page-coordinate region replaced by subsequent drags without property editing; verify scroll coordinates and replacement.

## 5. Full-page native screenshot (#198)

- [ ] 5.1 Add explicit full-page capture on Windows preserving viewport defaults; verify bounds/resource failures and beyond-viewport capture.
- [ ] 5.2 Add explicit full-page capture on macOS preserving viewport defaults; verify native implementation and capture contract tests.

## 6. Reliable batch send (#199)

- [ ] 6.1 Freeze sendable membership and initiating session, capture/upload one full-page image before submitting; verify composer preservation, differences-only requests and concurrent additions.
- [ ] 6.2 Submit through existing conversation boundary with queue semantics, receipts and receiver-side batch deduplication; verify busy mode, switching sessions and uncertain retry.
- [ ] 6.3 Retain pending data on preparation/submission failure and acknowledge only accepted records; verify pin retention, counts and increasing numbers.

## 7. Navigation and invalidation (#200)

- [ ] 7.1 Guard active navigation/refresh with default-stay confirmation and no auto-send; verify all host entry paths.
- [ ] 7.2 Retain invalid unsent records on document replacement or target removal and block affected batches without relocation; verify stale callbacks and absent conversation.

## 8. Integration and acceptance (#201)

- [ ] 8.1 Complete interaction-to-submission regressions, security/lifetime checks and source-selection regression; verify focused native tests, pnpm test, pnpm build and git diff --check.
- [ ] 8.2 Review against baseline 0155a863 on standards and spec axes, fix findings and update API/acceptance documentation; verify final diff and review results.
- [ ] 8.3 Perform real Windows complete desktop workflow including all fields and long-page screenshot pixels; record observed results without substituting automated tests.
- [ ] 8.4 Build and perform real macOS complete desktop workflow including all fields and long-page screenshot pixels; record observed results and keep draft if unavailable.
