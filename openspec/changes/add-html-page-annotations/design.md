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
