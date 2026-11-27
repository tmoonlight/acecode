> **Superseded（2026-11-22）**：本文档描述的路线（预览为主 / 整页截图 / 不可变记录 / 收据去重 / 导航确认）已被 `openspec/changes/redesign-html-page-annotations/` 取代，不再维护。
> 现行方案见 `docs/specs/2026-11-21-html-page-annotations-technical-design.md`，WIP 现状见 `docs/specs/2026-11-22-html-page-annotations-wip-alignment.md`。

# Tasks

## 1. Element annotation closed loop (#194)

- [x] 1.1 Add document-bound annotation runtime and restricted native/trusted commands; verify stale-document and untrusted-command rejection.
- [x] 1.2 Add toolbar entry, element outline/control and host-controlled popup over the browser; verify save creates a numbered pin and pending count without submission or file writes.
- [x] 1.3 Preserve saved pins on mode exit and immutable saved records; verify behavior and existing source-selection regression tests.

## 2. Reversible style previews (#195)

- [ ] 2.1 Implement all approved style fields through independent stylesheet layers and actual-difference filtering; verify every property and untouched original inline styles.
- [x] 2.2 Add default-off current-ratio lock and draft-only cancellation over saved layers; verify repeated edits of the same target.
- [x] 2.3 Add original-view comparison hiding numbered pins and restoring previews; verify round trips and mode exit.

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

- [x] 5.1 Add explicit full-page capture on Windows preserving viewport defaults; verify bounds/resource failures and beyond-viewport capture.
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
remained unverified at this historical checkpoint; see the final 39cd verification
below for Windows 5.1. macOS 5.2 remains unchecked.
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

- [x] 7.1 Guard active navigation/refresh with default-stay confirmation and no auto-send; verify all host entry paths.
- [x] 7.2 Retain invalid unsent records on document replacement or target removal and block affected batches without relocation; verify stale callbacks and absent conversation.

Ticket #200: document replacement retains invalid unsent records and numbering; exact
element/ancestor removal (including shadow roots and remove/reinsert in one turn)
invalidates the whole saved record without locator relocation and restores previews.
Page-coordinate regions survive scrolling and mode exit. Default-stay native confirmation
covers toolbar/address/history/reload, page navigation, destructive authenticated proxy
commands, cookie/controller replacement and clear-data reload. Frozen store transitions
reserve all affected pages before prompting; identity/revision/sequence/ticket checks
reject nested-modal and asynchronous stale continuations. Sending holds a bounded trusted
lease through receiver admission; page code receives no privileged messaging capability.
Passive polling is single-flight and does not advance ordinary command sequence.

Verified: 37 isolated MSVC session/navigation/native-boundary GoogleTests, Windows
host/transport/bridge compilation, batch sender lease tests, real headless Chromium DOM
lifecycle smoke, pnpm build, all four strict layer/size/ownership/document-path gates
without baseline changes, and git diff --check. Full pnpm test remains blocked by the
8f2 native-extraction source-path assertions owned by #201 (favicon serialization and
isolated-world ownership); runtime behavioral cases run before the latter assertion.
Native Windows interactive acceptance and macOS compilation/runtime remain unverified.
R1 follow-up adds a bounded per-session pending registry shared with native transports
until completion. Document replacement, navigation suspension, page retirement and
shutdown settle held capture/validation/evaluation callbacks exactly once; the existing
UI-thread poller expires requests after 15 seconds even when the document is not pollable.
Callbacks run outside registry mutation and native completion locks; no detached timers
or page capabilities are added. Late success cannot reacquire/release a newer lease,
and cancelled Windows CDP evaluation cannot advance its remaining stages. Deterministic
fake-clock tests cover bounds, expiry, shutdown/reentrancy and retained old-document
receipt acknowledgement versus rejected new admission. Section 8 acceptance is unchanged.
Follow-up verification: 42 isolated native tests pass, Windows host/transport/bridge
compile passes, and all four strict gates remain clean with unchanged size baselines.

## 8. Integration and acceptance (#201)

- [x] 8.6 Restore Windows full-page capture's classic root scrollbar layout using an owned CDP helper; verify real beyond-viewport pixels, unchanged layout/scroll/viewport/styles, native error/cancellation cleanup, strict gates and independently linked native smoke without using the shared build graph. Verified 37 isolated native tests, normal/stable/both-edges/thin/horizontal-overflow real pixel smoke, safe unsupported-author-style failure with preserved state, and all four strict standards gates. Complete annotation workflow, integrated rebuild/rerun and macOS acceptance remain separate.

- [x] 8.5 Extract focused native capture/annotation transport and state serialization boundaries, relocate shared smoke support, and correct ownership scanner header-token classification; verify strict standards gates and isolated native compilation/tests without changing capture behavior or baselines. Verified all four strict gates, 104 Python tests, 23 standalone native tests (including fake-client probe failure regressions), Windows host/transport/bridge/smoke translation-unit compilation, and OpenSpec validation. Probe null/error handling preserves native failures. No shared build, real native capture acceptance, or macOS compilation/runtime claimed.

- [x] 8.1 Complete interaction-to-submission regressions, security/lifetime checks and source-selection regression; verify focused native tests, pnpm test, pnpm build and git diff --check.
- [x] 8.2 Review against baseline 0155a863 on standards and spec axes, fix findings and update API/acceptance documentation; verify final diff and review results.
- [ ] 8.3 Perform real Windows complete desktop workflow including all fields and long-page screenshot pixels; record observed results without substituting automated tests.
- [ ] 8.4 Build and perform real macOS complete desktop workflow including all fields and long-page screenshot pixels; record observed results and keep draft if unavailable.

#201 automated checkpoint at base `7aeb4e9f`: the dedicated real-Chromium/native
driver suite passes after adapting its HTTP boundary to the production query-only
reconciliation contract. It preserves F1/F2 assertions and verifies explicit refusal
and unknown-receipt retries cannot newly admit invalid exact targets, while accepted
receipts recover with the identical payload. Parent/child differences match the
final real DOM, and disposal restores exact original markup/node identity. Existing
interaction/save, frozen membership/API, source/composer and ownership/stale-callback
seams also pass. Earlier red evidence remains in the acceptance record. Extraction
assertions retain serializer/native-command helper contracts and production wiring.
Own-worktree full `pnpm test`, `pnpm build` and seven focused frontend test files
pass at this checkpoint. API/design documentation includes query-only reconciliation
and the exact not-accepted/revalidation condition; no product files changed here.
All four strict gates (including final ownership) report zero findings;
OpenSpec strict validation and `git diff --check` pass.
8.1 remains unchecked pending #200/final integrated native verification; complete
review and both real desktop workflows are also unverified here. See the acceptance
document for runnable commands and injected-boundary limits.

#201 follow-up at integrated base `4b0c00c4`: the dedicated browser/native-driver
suite passes with the #200 lease-aware test adapter. It asserts real native lease
acquisition, matching release after HTTP failure and lifecycle-validation failure,
while retaining exact-target retry/no-admission, real queue and actual-DOM diff
assertions. The TEMP driver was directly recompiled, outside the shared Ninja graph.
This test-only correction does not complete 8.1 or the real platform workflows;
reliable integrated native recompilation and final review remain pending.
The follow-up's four strict gates report zero findings, OpenSpec strict and diff
checks pass, and the frontend build plus seven focused Node files pass. Full
`pnpm test` and a diagnostic rerun fail at `agentBrowser.test.js:40` (readiness
`undefined` instead of `true`); that file passes standalone. The failure remains
recorded, not waived or attributed to a proven baseline. See acceptance execution
records and TEMP `acecode-201-lease-web-diagnostic.log` for the captured stack.

Coordinator evidence update, 2026-11-19: reliable full native rebuild session 50005
passed with start/end HEAD fixed at `4b0c00c4`; `routes_ws` recorded 747 dependencies.
Focused native and CTest each passed 90/90, and a real visible integrated smoke
emitted `SMOKE_OK` and `SMOKE_OWNERSHIP_OK`. The first hidden-launch attempt failed
the active-visible harness check and remains recorded in acceptance. These results
supersede the pending reliable-rebuild status only for 4b0, not later merged sources.
S4 `5752ff65`, S2/S3 `67a0a021` and the two-file CI assertion fix `d298130f` are now
integrated; fresh frontend/driver/native compilation and execution remain pending.
Linux CI `44369114282` had one receipt-test failure among 7,058 tests; the fix retains
exactly-one real admission and metadata checks while excluding typed internal context.
Worker replay/isolated compilation is not a passing fresh Linux HTTP/CI run.
At that checkpoint no completion boxes changed: 2.1 cascade, 5.1/5.2 platform capture
acceptance, 8.1/8.2 final integration/review and 8.3/8.4 manual workflows remained open.

Final coordinator verification, 2026-11-19, fixed clean start/end HEAD
`39cd01dbbd97d1d7bd8ee4dc288020e835287821`: native session 37596 passed desktop/unit/smoke
builds and dependency validation; freshly linked focused native and CTest each passed
107/107, including new HTTP receipt and MessagePayload tests. Real visible integrated
smoke and ownership passed; the capture probe covered bottom-green pixels, preserved
layout/style/viewport/scroll, stale/resource rejection and retry. This completes 5.1.
Same-HEAD complete pnpm test/build and regex checks, directly rebuilt driver/dedicated
integration, Edge lifecycle, layers/size/ownership/OpenSpec/diff all passed. Final
Standards and Spec reviews pinned to 39cd report no open code findings. This completes
automated integration 8.1 and dual review 8.2, not the whole feature. The prose-only
documentation-path failure is corrected in this documentation-only follow-up, without
baseline exemption; strict doc paths, OpenSpec and diff are rerun separately.
Acceptance records exact TEMP logs and historical failures, including Linux CI
44369114282 (one receipt failure among 7,058 tests) and the earlier broader native
11-failure run. Neither is replaced by a passing focused Windows run or a claimed
Linux rerun. Tasks 2.1, 5.2, 8.3 and 8.4 remain open; keep the pull request draft.

Codex chrome follow-up: the comment card and toolbar now match the observed Codex
controls, and the approved cascade behavior is Codex's unlayered `!important`
author preview. Ineffective inline or layered `!important` winners produce no
error and no difference. Tasks 2.1, 5.2, 8.3 and 8.4 stay open; the pull request
stays draft. Windows manual acceptance and macOS native/manual acceptance are
not claimed.

Interaction parity follow-up, 2026-11-19: element and region picking can start
from the floating toolbar before a draft popup exists; the toolbar hides while
the user is picking, while a card is open, and after leaving annotation mode.
Native occlusion holes carry the overlay border radius, so rounded cards and the
toolbar no longer expose the WebUI background at their corners. The card keeps a
fixed header/footer with a 230px scrolling editor, omits the target tag row,
left-aligns labels, splits values from units, exposes font/weight menus, restore
actions and live previews, and uses a clamped 0-100% opacity control. Width,
height and paired padding/margin locks use centered circular buttons between
T-shaped guides with hover and locked states; collapsed spacing rows retain four
editable values. Hover shows the native outline and compact element information
card, one click opens the card, and an outside click cancels its draft. Toolbar
screenshot, hide-marker, hold-original, discard-all, send, and exit behavior,
the exact discard confirmation, stable count width, tooltips and empty-input
disabled submit state are covered by frontend tests. Windows development desktop
manual inspection confirmed hover, one-click open, outside dismissal, fixed
header/footer, 230px scrolling, collapsed four-value spacing, expanded pair
locks and rounded card rendering. The expanded editor is 380px wide; its value
column is 176px, and each collapsed spacing input keeps at least 45px of usable
width before wrapping. Tasks 2.1, 5.2, 8.3 and 8.4 remain open; the
pull request stays draft, and Windows complete-workflow plus macOS
native/manual acceptance are not claimed.

Final interaction verification, 2026-11-19: the source catalog was regenerated
after the new annotation copy, and full `pnpm test` plus `pnpm build` (including
the regex compatibility scan) pass. Browser regression helpers now reflect the
one-click open behavior instead of the removed select-then-Annotate sequence.
Real Chrome headless lifecycle smoke passes exact DOM/shadow identity, queued
removal, multi-target previews, regions and root replacement. A freshly linked
standalone native driver passes dedicated annotation integration, including
trusted input, multi-target/region save, HTTP serialization, receipt handling,
lease release, refused-retry behavior and final-DOM differences. OpenSpec strict
validation and `git diff --check` pass. Windows development desktop manual
inspection is recorded above; tasks 2.1, 5.2, 8.3 and 8.4 remain open, Windows
complete-workflow and macOS native/manual acceptance are not claimed, and the
pull request stays draft.

Style-control follow-up, 2026-11-20: paired numeric cells no longer reserve reset
space when no reset is visible, and the two-column gap pair uses a smaller reset
reservation after editing. The ratio-lock link icon is 14px with a centered,
stronger locked stroke. Windows development-desktop inspection confirmed both
gap units remain inside their controls in the default and edited states and the
lock icon is legible in both states. Tasks 2.1, 5.2, 8.3 and 8.4 remain open.

Specification alignment, 2026-11-21: the spec/design documents were brought in line
with features already implemented in the worktree but not yet described there —
native occlusion geometry (`square_corners`, radius clamp, far-edge guard, implicit
bleed), modal chrome suspension and native input blocking, hover-card lifecycle,
send-error and i18n localisation, the screenshot bridge, annotation message parsing
and card presentation, the expanded editor field/lock set, target-tag row, 2px
editor gap and discard numbering reset. This is documentation only; no product code
or task checkboxes changed. The original-view pin discrepancy previously flagged
here was confirmed by the owner: hiding numbered pins in `original` view
(`markersVisible = !markersHidden && !original`) is intended. The spec scenario,
the three design statements and task 2.3 were aligned accordingly; pin records
remain intact and pins reappear when toggling back. No product code changed.
Tasks 2.1, 5.2, 8.3 and 8.4 and the draft PR state are unchanged.
