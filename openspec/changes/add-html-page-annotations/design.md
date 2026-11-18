# Design

## Context

See proposal.md for motivation and GitHub spec #193 for the complete approved contract. Native browser pages sit above the trusted WebUI; existing surface coordination supports local occlusion on Windows/macOS. Existing element selection and automation evaluation provide native-controlled page execution. Existing screenshot handling on macOS captures webview bounds, not a full page. Conversation submission supports queue/steer and client identities, but this is not proof of durable batch receipt/deduplication.

## Goals / Non-Goals

**Goals:** preserve page/host trust separation and existing submission ownership while adding one annotation lifecycle per page/document; make capture and receipt failures explicit and testable.

**Non-Goals:** no page-accessible send bridge, no replacing source selection, no disk-writing preview, no saved-record editing/deletion, no restart persistence. Scope remains exactly #193.

## Decisions

1. Separate page runtime locating/markers/previews from trusted popup and batch state. Render the popup over the browser using host-controlled overlay/surface coordination (or native hosting where needed), never in the chat column. Reuse existing trusted desktop bridging, validate page/document identity on every command/result, and keep page execution incapable of submitting conversations. Arbitrary page messages are not privileged commands.
2. Store semantic records with stable IDs, monotonically increasing numbers, page/document identity, target summaries/locators, one optional page-coordinate region, shared description, per-target actual differences and pending/sending/sent state. Runtime DOM references do not cross transport. Preview stylesheet and reversible text-node records form independent layers; cancellation removes the current layer only. Original view disables previews while retaining markers. Preserve original inline styles and child-node identity.
3. Reuse supported native capture boundaries with an explicit full-page request, preserving current viewport defaults. Native adapters must capture beyond the viewport or report an error; bounded resource failures must not silently truncate. Tie capture to the same document snapshot used by the frozen batch.

   Capture implementation (#198): the existing authenticated `Page.captureScreenshot`
   proxy accepts `fullPage:true` or `captureBeyondViewport:true`; omitted/false stays on
   the viewport path. Windows measures CDP CSS content bounds and the top-frame loader,
   reads device scale in an isolated world, and captures an explicit beyond-viewport
   clip at CSS-pixel resolution. macOS measures in the existing isolated automation
   world and uses `WKWebView.createPDF` with the entire document rect, validating a
   single complete PDF page before in-memory PNG rasterization. Neither path resizes
   the viewport, scrolls/stitches partial images, writes files or exposes a page bridge.
   The async operation rechecks the native page/view, navigation, sharing permission,
   document identity and bounds; changed documents/layouts, native errors, budget
   overflow and incomplete PNG dimensions/chunks fail recoverably. An opaque returned
   capture `documentId` can be passed back for a later capture, but is not implicitly
   interchangeable with annotation-runtime document identities. Batch integration must
   retain its initiating document binding rather than substitute tokens between the
   two boundaries. Limits are 32768 pixels per side, 32 Mi pixels and 32 MiB PNG.
   Platform long-page pixel acceptance remains required; WebKit PDF does not promise
   protected-media/accelerated-video fidelity.
4. Batch controller freezes IDs/session, captures and prepares a single attachment, then invokes existing conversation submission with explicit queue behavior. Do not use composer state as a buffer. Reuse one batch identity for uncertain retries and implement receiving-side duplicate acceptance rather than relying only on a generated client ID. Receipt drives sent status; capture, upload and request failures do not erase records.
5. Navigation guards operate before host-directed transitions; document/target invalidation handles transitions that cannot be prevented. No locator fallback, no auto-send, no new saved-record deletion. Invalid batches remain blocked during the current application run.
6. Highest test seam is annotation interaction through conversation submission. Use existing frontend behavioral tests, native GoogleTests and browser runtime smoke helpers for supplemental state/DOM/capture tests. Windows/macOS complete desktop acceptance remains separate and cannot be checked off with source-only assertions.

## Risks / Trade-offs

- Native surface layering differs between platforms → reuse occlusion/overlay lifecycle and validate real desktop input/focus, not just JSX snapshots.
- Untrusted webpage can manipulate DOM → isolate privileged control, validate document generations, bound payloads, never expose send to page code.
- Dynamic DOM and concurrent previews → retain exact target/node identity, invalidate removed nodes, test overlapping saved/draft layers.
- Tall-page capture may exceed native limits → validate dimensions/resources and fail recoverably rather than truncate or crash.
- Busy submission/session changes/uncertain network results → explicit session-bound batch and receiver-side idempotency with receipt tests.
- No macOS environment on this Windows host → leave real macOS build/manual acceptance outstanding, keep PR draft until required results are available.

## Migration Plan

Additive APIs preserve existing viewport capture and source selection. Implement eight approved vertical tickets (#194–201) on one integration branch, with independent implementer worktrees and merger integration. Keep PR draft throughout incomplete acceptance. Rollback is removal of new annotation entry/handlers without rewriting persisted source-selection data. No new mandatory dependencies or persisted annotation migration are assumed.

## #199 Sender Contracts

`pageAnnotationBatches.js` owns runtime-only frozen batches, independent of composer
state and the annotation popup controller. Membership, descriptions/differences,
page metadata, initiating conversation and daemon credentials are snapshotted at
send time. One native full-page PNG is uploaded through the existing session
attachment API, then the exact ordinary-message payload is retained under one
`annotation_batch_id`. Busy conversations use ordinary queue admission, not steer.
Concurrent calls share one flight; additions remain eligible for a later batch.

Native `PageAnnotationSession::submission` validates saved record membership and
document identity. Trusted host `capture` evaluates isolated `validate_batch` against
the exact saved DOM references before and after screenshot measurement. Native
page/view/navigation/current-annotation-document guards and the existing capture
document/bounds/PNG checks must all pass. Capture `documentId` is deliberately a
different identity from runtime `document_id`; only native validation establishes
their relationship. `validate` repeats the runtime check after upload before the
first HTTP attempt. User-initiated annotation capture is available without agent
sharing, without bypassing any automation proxy permission checks.

Preparation failures retain the frozen membership; upload retries reuse a successful
capture. Uncertain HTTP retries reuse the original payload and connection without
recapture or target revalidation, so a previously accepted receipt can be recovered
even after navigation. No retry is redirected to a newly selected conversation.
Only a matching `queued:true` batch receipt triggers native `acknowledge`; failure of
that final step retries acknowledgement alone. Saved records remain immutable except
their sent status, preserving pins/previews/numbering and retaining invalid records.
The page-scoped sender survives panel unmount; page closure ends its retention.
Neither sender nor receiver claims restart recovery.

Automated coverage exercises freeze/additions/session changes, concurrent flights,
independent document tokens, capture/upload/validation/submission/ack failures,
malformed/mismatched/duplicate receipts, exact uncertain retries, native membership
validation and saved-pin retention. Full desktop interaction and platform screenshot
pixel acceptance remain section 8 work, not implied by these tests.

## #194 Implementation Contracts

The description-only slice uses `page_annotation_runtime.hpp` for exact document/element references,
`page_annotation_session.hpp` for native-owned immutable records, and `page_annotation_bridge.{hpp,cpp}`
for the trusted WebUI entry. Windows resolves the main frame and creates an isolated CDP world before
evaluation; macOS uses its existing automation `WKContentWorld`. There is no page message handler for
annotations and no description, conversation identity or file capability in runtime commands.

`AgentBrowserHost::annotation_command(page_id, command_json, completion)` accepts only
`start/read/exit/cancel/save`. `PageAnnotationSession::prepare/sequence/complete/fail` serializes commands
and rejects stale completions; native callbacks additionally validate weak host/page identity and
controller incarnation. `replace_document` cancels the draft and invalidates retained records. Only
start accepts an empty initial document ID; save binds the current target ID. Saved records carry
`targets`, empty `differences`, null `region`, pending status and stable page/document/record identity.
Future tickets can extend these explicit fields without conflating actual differences with descriptions.

`createPageAnnotationController` in `web/src/lib/pageAnnotations.js` binds queued document/target
identity and ignores stale revisions/disposed results. `PageAnnotationPopup` in
`web/src/components/PageAnnotations.jsx` is the trusted editor; its `snapshot/command/working/viewportRef`
interface and existing local native occlusion coordinator are the popup extension seam. The runtime's
closed shadow root contains only outline/control/pins, not a competing editor. Source selection's
`selected_element` event and chat-context behavior remain separate.

Verification details and outstanding platform checks are recorded in `docs/agent-browser.md` under
“网页描述批注（#194）”. Automated behavioral coverage and Windows translation-unit compilation do not
establish actual Windows/macOS popup layering, focus, hit testing or complete desktop acceptance.
Section 8 platform tasks remain unchecked; capture/send/navigation-confirmation scope remains owned by
the subsequent tickets.

## #196 Text Preview Contract

`page_annotation_text_runtime.hpp` exports a factory injected as runtime argument 2
(argument 3 remains available for styles). `preview_text {target_id,text}` is a
restricted document/target-bound command. The trusted popup uses the target's
`text_preview {value,editable,error}`; typing is serialized before save without
disabling the text field on each keystroke. Save acknowledgments carry only actual
`{target_id,property:"text",old_value,new_value}` differences, including text-only
annotations with an empty description. The baseline is the effective saved text,
not a previous unsaved draft or the temporarily displayed original.

The helper retains original Text nodes/data and exact parents, applies a common
prefix/suffix replacement to eligible descendant text nodes, and never replaces
an element subtree. Hidden/script/style/form-control content is excluded. The
editor uses concatenated eligible text-node data, not CSS-generated content or
layout-generated line breaks. Empty elements use an owned temporary text node,
removed during original view/cancellation/disposal. Saved node layers and draft
layers are independent; original view disables both without removing pins.
Original inline styles and all existing child nodes/listeners remain untouched.
Text beyond 4096 UTF-16 code units is explicitly non-editable, not truncated;
changed node topology rejects preview/save without selector-based relocation.

Verification: focused native session tests, production runtime/field JavaScript
behavior tests, and `node tests/desktop/page_annotation_text_smoke.mjs <Chromium>`
(real DOM identity/listeners and independently toggled stylesheet composition).
Headless Chromium is not Windows WebView2 or macOS WKWebView desktop acceptance;
both full native interaction sequences remain unverified under section 8.

The production web build and regex compatibility check pass. The full web runner
fails at `agentBrowser.test.js:40` when its readiness timer resumes after the
large static-import workload; the same failure reproduces with the unchanged
base-commit runner (without the new text suites). That suite passes standalone.
This unrelated full-suite scheduling failure remains unresolved, not a passed
acceptance check.
## #195 style implementation and extension contract

`page_annotation_style_runtime.hpp` exports `kPageAnnotationStyleRuntime`, a
`rootDocument => helper` factory supplied as argument 3 to `kPageAnnotationRuntime`.
Argument 2 remains reserved for the text helper. The style helper exposes
`select(target,id)`, `preview(target,id,styles,ratioLock?)`, `describe(target,id)`,
`differences()`, `save()`, `cancel()`, `setOriginal(enabled)` and `dispose()`.
`save()` returns an array of actual `{target_id,property,old_value,new_value}`
differences. Each exact target has independent saved declarations and a draft
with its own computed baseline, touched properties and default-off current ratio.
The single-target selection adapter cancels its previous draft; the multi-target
adapter must retain those drafts instead. Text integration should validate its
save before committing styles and concatenate both helpers' difference arrays.

The trusted command additions are `preview_style {target_id,styles,ratio_lock?}`
and `original {enabled}`. Native `PageAnnotationSession` validates fields and
acknowledged differences before storing an immutable record. Runtime snapshots
include `original`, `differences`, and per-target `style_preview` with `values`,
`styles`, `ratio_locked`, `ratio` and `errors`. Saved target summaries omit the
editor-only `style_preview`. The existing serialized controller binds document
and target at enqueue, including previews. The popup flushes dirty inputs before
save and retains edits made while another preview is in flight.

Constructed stylesheets belong to each Document/ShadowRoot; original inline
declarations and author stylesheet membership remain untouched. Exact targets
receive random marker attributes; copied attributes are removed from clones.
Mode exit/cancel removes drafts only. Original view temporarily disables saved
and draft style declarations without removing pins. Transition suppression is
settled before restoring author declarations so cancellation/comparison does not
leave a long-running transition. A requested border width on a border with style
`none` derives `solid`; actual per-side style/width changes are recorded too.

**Remaining cascade blocker:** an independent author stylesheet cannot override
an existing inline `!important` declaration while also preserving that declaration.
The helper reports `inline_important:<property>` and does not fabricate a change.
Important rules inside author cascade layers can also outrank unlayered important
preview declarations. This is not full arbitrary-page cascade coverage; task 2.1
remains open rather than accepting an undocumented exception or changing original
inline styles. Normal inline/stylesheet declarations and every approved field are
covered by actual Chromium CSS tests. Real Windows/macOS Desktop acceptance is
still outstanding and is not implied by those tests.
### Focused native standards boundaries

The hosts retain page membership, navigation/controller incarnation, sharing and annotation
sequence checks. `agent_browser_capture_cdp` owns full-page CDP measurement/clip orchestration
behind injected command/current/completion functions; `agent_browser_native_commands_win`
owns WebView2 transport and isolated annotation evaluation. `agent_browser_capture_mac` owns
WebKit snapshot/PDF/rasterization, with native-view retention and host-guarded evaluation.
`agent_browser_proxy_request` owns the existing authenticated request/deadline and exactly-once
completion/wait contract shared by both hosts. `agent_browser_state_json` owns browser-state
serialization without changing optional selected-element or annotations payload semantics.
These extractions preserve viewport defaults and full-page bounds/PNG/stale-document checks.

The shared capture probe lives under `tests/test_support/desktop` and both native smoke
programs include that full prefix. The ownership scanner masks only actual include header
tokens before scanning allocations; preprocessor macro allocations remain counted. No size
or ownership baseline is changed. Native runtime/pixel acceptance remains separate from
the focused boundary tests and translation-unit compilation.

The probe checks native errors and object shape before inspecting command/evaluation
results, validates capture metadata types, and retains the original native error on
failure. A platform-neutral checked-command seam has fake-client regressions for null
full-page responses, malformed results, and transport errors; it does not replace the
native pixel check or establish real capture acceptance.
