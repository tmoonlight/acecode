# Tasks

## 1. Element annotation closed loop (#194)

- [x] 1.1 Add document-bound annotation runtime and restricted native/trusted commands; verify stale-document and untrusted-command rejection.
- [x] 1.2 Add toolbar entry, element outline/control and host-controlled popup over the browser; verify save creates a numbered pin and pending count without submission or file writes.
- [x] 1.3 Preserve saved pins on mode exit and immutable saved records; verify behavior and existing source-selection regression tests.

## 2. Reversible style previews (#195)

- [ ] 2.1 Implement all approved style fields through independent stylesheet layers and actual-difference filtering; verify every property and untouched original inline styles.
- [x] 2.2 Add default-off current-ratio lock and draft-only cancellation over saved layers; verify repeated edits of the same target.
- [x] 2.3 Add original-view comparison retaining pins and restoring previews; verify round trips and mode exit.

## 3. Visible text previews (#196)

- [x] 3.1 Implement reversible text-node previews preserving child-node identity; verify nested elements, text changes and exact restoration.
- [x] 3.2 Integrate saved/draft/original text layers and actual differences; verify cancellation retains earlier saved text.

## 4. Multiple targets and region (#197)

- [x] 4.1 Support adding multiple elements to one draft with shared description/pin; verify target switching and save.
- [x] 4.2 Support one page-coordinate region replaced by subsequent drags without property editing; verify scroll coordinates and replacement.

Implementation contract (#197): `targets` contains at most 64 unique exact element identities;
`target` is the current element or null when the region is selected. `region` is null or one
finite, positive-size page-CSS-coordinate `{x,y,width,height}` rectangle. `selection_mode` is
null, `element` or `region`; the trusted popup stays mounted but hidden during picking, so its
single description survives target switching/replacement. `add_target`, `add_region`, `select_target`
and `select_region` are restricted native actions, not page message capabilities. Save binds the
document, current target, ordered `target_ids` and region; one record/number/pin covers the whole
draft. Required target summary/identity/bounds fields are validated while bounded target metadata
and the existing differences slots remain available to #195/#196; no text/style fields are added.
The shared native session and runtime retain the existing callback sequence/incarnation guards.

Focused runtime/popup behavioral tests and eight native session GoogleTests cover deduplication,
switching, shared description/save/cancel, page-coordinate scrolling, reverse drag, region replacement,
untrusted/cancelled/zero-area drags, malformed arrays/regions, stale identity and immutable records.
Real Windows/macOS desktop interaction acceptance is not claimed by these automated tests;
section 8 remains unchanged.
Verification: standalone annotation JS tests, all eight native session tests (MSVC Release),
and `pnpm build` pass. Full `pnpm test` fails at the unchanged `agentBrowser.test.js:40`
native-page readiness assertion under the combined runner; that suite passes standalone.

## 5. Full-page native screenshot (#198)

- [ ] 5.1 Add explicit full-page capture on Windows preserving viewport defaults; verify bounds/resource failures and beyond-viewport capture.
- [ ] 5.2 Add explicit full-page capture on macOS preserving viewport defaults; verify native implementation and capture contract tests.

Ticket #198 implementation: explicit `fullPage`/`captureBeyondViewport` routing,
document/page-bound capture orchestration, bounds/PNG budgets and recoverable failures
are implemented. Windows uses CDP full-document bounds/clip; macOS uses WebKit PDF
capture plus complete-single-page PNG rasterization. Viewport paths remain separate.

Verified locally on Windows: 11 focused `AgentBrowserCapture` GoogleTests pass using
an isolated MSVC build; `agent_browser_host.cpp` compiles independently against the
existing SDK headers without configuring/installing vcpkg. The Windows smoke source
also compiles, and WIC successfully decodes the tall PNG test fixture's bottom green
pixel. These tests cover request
defaults, malformed/conflicting requests, tall-image contract, resource/bounds limits,
truncated/corrupt PNG, stale page/document/layout/scale callbacks and retry after failure.
Native smoke helpers now include a deterministic tall-page probe that decodes PNG
pixels below the viewport, checks viewport/scroll preservation, rejects stale document
tokens/resource overflow and retries. The native probes have not been executed here.
Windows full native link/runtime acceptance and macOS compile/runtime/pixel acceptance
remain unverified; 5.1/5.2 stay unchecked rather than claiming platform acceptance.
`openspec validate add-html-page-annotations --strict` and `git diff --check` pass.

## 6. Reliable batch send (#199)

- [x] 6.1 Freeze sendable membership and initiating session, capture/upload one full-page image before submitting; verify composer preservation, differences-only requests and concurrent additions.
- [x] 6.2 Submit through existing conversation boundary with queue semantics, receipts and receiver-side batch deduplication; verify busy mode, switching sessions and uncertain retry.
- [x] 6.3 Retain pending data on preparation/submission failure and acknowledge only accepted records; verify pin retention, counts and increasing numbers.

Ticket #199 sender: extracted page-scoped in-memory batch controller and send bar;
existing composer/source-selection paths are not used as buffers or modified. Native
trusted capture/validate/acknowledge commands validate owned membership and exact
runtime document/targets, without equating annotation and capture identity tokens.
Ordinary session message admission uses the already integrated bounded receiver
idempotency prerequisite; no new receiver work or steer path is introduced.

Verified: batch-controller freeze/concurrency/additions/conversation/transport tests,
description/differences filtering, every capture/upload/validate/receipt/ack failure,
matching receipt before acknowledgement, exact payload retries and source/composer
boundary regressions; native session membership/acknowledgement and runtime exact
target tests; 19 isolated MSVC session/capture tests; Windows host syntax compile;
full `pnpm test`, `pnpm build` and regenerated source i18n catalog. The prior receiver
prerequisite separately verified concurrent duplicate acceptance/conflict/rejection
and ordinary/busy queue admission. Native full integration/link, actual Windows
workflow and macOS compile/runtime remain section 8 acceptance, not claimed here.
Sender/receiver retention is runtime-only with no restart-recovery guarantee.
Strict layer/ownership scans still report the inherited host/main size, capture
test-helper location, capture allocation and annotation-bridge capture findings.
The sender adds host lines but no new dependency/ownership findings. Native boundary
extraction is being implemented separately; no baseline exceptions are added here.

## 7. Navigation and invalidation (#200)

- [ ] 7.1 Guard active navigation/refresh with default-stay confirmation and no auto-send; verify all host entry paths.
- [ ] 7.2 Retain invalid unsent records on document replacement or target removal and block affected batches without relocation; verify stale callbacks and absent conversation.

## 8. Integration and acceptance (#201)

- [x] 8.5 Extract focused native capture/annotation transport and state serialization boundaries, relocate shared smoke support, and correct ownership scanner header-token classification; verify strict standards gates and isolated native compilation/tests without changing capture behavior or baselines. Verified all four strict gates, 104 Python tests, 23 standalone native tests (including fake-client probe failure regressions), Windows host/transport/bridge/smoke translation-unit compilation, and OpenSpec validation. Probe null/error handling preserves native failures. No shared build, real native capture acceptance, or macOS compilation/runtime claimed.

- [ ] 8.1 Complete interaction-to-submission regressions, security/lifetime checks and source-selection regression; verify focused native tests, pnpm test, pnpm build and git diff --check.
- [ ] 8.2 Review against baseline 0155a863 on standards and spec axes, fix findings and update API/acceptance documentation; verify final diff and review results.
- [ ] 8.3 Perform real Windows complete desktop workflow including all fields and long-page screenshot pixels; record observed results without substituting automated tests.
- [ ] 8.4 Build and perform real macOS complete desktop workflow including all fields and long-page screenshot pixels; record observed results and keep draft if unavailable.

#201 automated checkpoint at base `8f2e060e`: extraction ownership assertions now
verify the serializer/native-command helper and reachable production call sites,
without removing favicon/owner/state/security contracts. Own-worktree `pnpm test`
and `pnpm build` pass. A new opt-in browser/native integration runner exercises
real interaction/save records through sender/API/ordinary queue/receipt and checks
exact document ownership and late callbacks. Its parent/child DOM assertion
reproduces review finding F2 (`Earth` diff versus `world` actual child text), so
8.1 remains unchecked pending the separate product fix and final integrated rerun.
An additional real HTTP refusal followed by exact-target clone replacement also
reproduces F1: the old retry first-admits invalid work despite native validation
rejecting the replacement. The latest run stops at F1; both regressions stay red.
Navigation #200, complete review and both real desktop workflows remain unverified
here. See the acceptance document for runnable commands and injected-boundary limits.
