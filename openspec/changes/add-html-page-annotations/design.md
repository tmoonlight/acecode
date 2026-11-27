> **Superseded（2026-11-22）**：本文档描述的路线（预览为主 / 整页截图 / 不可变记录 / 收据去重 / 导航确认）已被 `openspec/changes/redesign-html-page-annotations/` 取代，不再维护。
> 现行方案见 `docs/specs/2026-11-21-html-page-annotations-technical-design.md`，WIP 现状见 `docs/specs/2026-11-22-html-page-annotations-wip-alignment.md`。

# Design

## Context

See proposal.md for motivation and GitHub spec #193 for the complete approved contract. Native browser pages sit above the trusted WebUI; existing surface coordination supports local occlusion on Windows/macOS. Existing element selection and automation evaluation provide native-controlled page execution. Existing screenshot handling on macOS captures webview bounds, not a full page. Conversation submission supports queue/steer and client identities, but this is not proof of durable batch receipt/deduplication.

## Goals / Non-Goals

**Goals:** preserve page/host trust separation and existing submission ownership while adding one annotation lifecycle per page/document; make capture and receipt failures explicit and testable.

**Non-Goals:** no page-accessible send bridge, no replacing source selection, no disk-writing preview, no saved-record editing/deletion, no restart persistence. Scope remains exactly #193.

## Decisions

1. Separate page runtime locating/markers/previews from trusted popup and batch state. Render the popup over the browser using host-controlled overlay/surface coordination (or native hosting where needed), never in the chat column. Reuse existing trusted desktop bridging, validate page/document identity on every command/result, and keep page execution incapable of submitting conversations. Arbitrary page messages are not privileged commands.
2. Store semantic records with stable IDs, monotonically increasing numbers, page/document identity, target summaries/locators, one optional page-coordinate region, shared description, per-target actual differences and pending/sending/sent state. Runtime DOM references do not cross transport. Preview stylesheet and reversible text-node records form independent layers; cancellation removes the current layer only. Original view disables previews and hides numbered pins while retaining their records. Preserve original inline styles and child-node identity.
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
capture. Uncertain HTTP retries first use
`POST /api/sessions/:id/messages?annotation_reconcile_only=1`, exposed by
`sendInput(sessionId, payload, {reconcileOnly:true})`. This lookup preserves the same
batch ID, exact frozen payload and connection, without recapture/reupload. Query
metadata is excluded from the payload fingerprint. Under the queue admission lock,
lookup returns only an existing matching receipt; an unknown ID returns
`404 annotation_batch_not_accepted` without enqueueing work or consuming the ID.
A matching receipt can therefore be recovered even after document/target invalidation.
Only that exact not-accepted result permits native target/document revalidation
followed by possible first admission; ambiguous lookup errors, malformed receipts
and mismatched IDs never authorize admission. Defined explicit original admission
refusals (`annotation_batch_unavailable`, `annotation_batch_capacity`,
`annotation_batch_conflict`) clear uncertainty and require native revalidation before
another normal submission. No retry is redirected to a newly selected conversation.
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
layers are independent; original view disables both and hides pins without deleting their records.
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
and draft style declarations and hides numbered pins without deleting their records. Transition suppression is
settled before restoring author declarations so cancellation/comparison does not
leave a long-running transition. A requested border width on a border with style
`none` derives `solid`; actual per-side style/width changes are recorded too.

**Approved cascade behavior:** follow Codex. Preview rules are unlayered author
stylesheet declarations with `!important` and never rewrite original inline styles.
When an inline `!important` declaration or a more specific layered author
`!important` rule wins, the preview does not report an error, does not record an
ineffective difference, and does not add a user-origin extension.
`style_preview.errors` stays an empty array. Task 2.1 stays open because this is
not arbitrary-page cascade coverage, and real Windows/macOS desktop acceptance is
still outstanding.

**Codex annotation chrome:** the comment card and toolbar follow the observed
Codex desktop controls without copying Codex source. In annotation mode, pointer
hover shows an element outline and a compact card with tag, rendered size, color
and font; one unmodified primary click immediately opens the annotation card.
The collapsed card is 294px, the expanded card is at most 380px, and the
top-left `调整` control uses `aria-expanded`, not `aria-pressed`. The card header
stays fixed, the property editor scrolls inside a 230px ceiling, and the action
footer stays fixed. There is no longer a separate target-type row inside the
style stack: the target tag (e.g. `/p`, `/div`) sits on the same row as the drag
grip at the top of the expanded card, and the last field in the scrolling editor
keeps a 2px gap above the action-bar divider.
Rows align their labels by text content and put the value control in a stable
176px second column. Collapsed four-way spacing controls keep at least 45px of
usable width per input and wrap when the viewport cannot preserve that width.
Color rows include a swatch, font/weight rows use a picker,
numeric rows split the value from its unit, edited rows expose a restore action,
and every edit previews live. Opacity is entered as `0..100%` and clamped before
conversion to CSS `0..1`. Width/height and padding/margin pairs use centered
circular lock controls between upper and lower T-shaped guides; the controls have
default, hover, and locked states, and paired locks propagate values
independently. Collapsed padding/margin rows keep four editable values, and
expanded rows replace them with their vertical/horizontal pair controls.
The value grid reserves the leading reset area only when a reset is visible, and
the two-column gap pair uses a smaller reservation so edited values retain a
readable value-and-unit control. Width/height and spacing locks share one T-shaped
bracket (26×62 path `M4 3H23V21M23 41V59H4`) with a centered 16×16 circular lock
badge, replacing the earlier 14px link icon; the badge has default, hover and locked
states. The editor also exposes letter spacing, word spacing and row/column gap
fields, a font family picker (Inter/System/Arial/Serif/Mono plus in-page custom
values) with weights 100–900, spacing pairs inferred from equal non-zero values,
and a 20ms debounce with `CSS.supports` gating before a live preview is sent.

The floating toolbar is a dark rounded bar with a draggable grip,
`正在批注 · {count}`, screenshot-copy, hide-marker toggle, hold-to-view-original,
discard-all, blue `发送`, and exit. The count column is width-stable. Discard asks
`丢弃所有待处理标注?` with the body lines `页面上的所有编辑都将被移除` and
`放弃后所有标记和修改都被清空` before the `取消` / `放弃` actions (no full stop).
The dialog uses `dismissOnBackdrop={false}`: clicking the backdrop does not close it,
and while it is open the native page stays painted but input is disabled and the
in-page chrome is suspended. Discard clears every saved record and resets numbering
to 1. The toolbar hides while a card or selection mode is active and when annotation
mode ends, so it cannot strand a lone send button. Clicking outside an open card
cancels that draft without leaving annotation mode. The adjust and submit buttons
show descriptive tooltips; submit also shows `Enter 发送 · Ctrl+Enter`.

The toolbar is mounted inside the browser viewport as an overlay, defaults to
horizontally centered 16px above the bottom (52px tall, 14px radius, draggable via a
grip clamped inside the viewport), and switches to absolute positioning after a drag.
The send button has a stable width (min 64px); its label is `发送` / `发送中` / `重试`
for idle/working/retry states. With no conversation it shows an inline `请先选择对话`
note and disables send. Screenshot copy confirms with `已复制截图` for 1800ms and
falls back to `网页截图失败` / `无法复制截图`. The hold-original control also responds
to Space/Enter and restores the page when the pointer leaves the button. The annotation
entry in the browser toolbar is a 16px chat icon with tooltip `网页批注` / `退出网页批注`;
the former standalone `原始页面` button was removed.

The model still receives `Page annotations` plus the structured JSON and the
full-page screenshot. The transcript renders that message as a card: the screenshot
crop, the page title, and non-empty descriptions. Numbers appear only when there is
more than one description. Bounds, locators, element text, and differences stay out
of the card. The address-bar control is a fixed 30×30 icon and does not gain a text
label while active.
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

### Windows full-page scrollbar restoration

WebView2 157.0.4341.60's surface screenshot hides the classic root scrollbar and can
leave the restored document laid out at the scrollbar-free width (845 to 860 CSS
pixels in the native fixture). Waiting for animation frames or forcing layout alone
does not restore it. Disabling beyond-viewport painting instead loses real bottom
pixels. Neither widening measured bounds nor accepting a viewport image is valid.

`agent_browser_capture_layout` owns the narrow CDP workaround behind the existing
injected native-command/current/completion seam. It inspects root computed styles
in an isolated world. With a classic auto gutter, a distinct, forced inspector
stylesheet reserves the existing gutter during capture. An existing stable or
stable-both-edges gutter is not replaced: the owned sheet briefly flips overflowing
root `overflow-y` between auto and scroll and forces layout after capture. The
helper clears only its own sheet text before completion, including native failure
and stale-page paths; it never edits original inline declarations, scroll position
or viewport settings. It does not disable shared DOM/CSS domains. A preflight
computed-style check rejects author cascade priorities that block the workaround
with `capture_layout_unsupported` before taking a screenshot. Cleanup failures
cannot produce success, and an earlier native error is retained.

The original document identity, strict pre/post bounds and scale comparisons,
resource budgets, complete-PNG validation and viewport-capture path remain intact.
The native probe removes the example page's external body max-width constraint
from its own fixture and uses explicit CSS pixel units when restoring fixture
height. Its pixel sample therefore tests green content, not the gray area outside
the body. It also checks root client dimensions, original inline styles and author
stylesheet count, in addition to scroll/viewport preservation. This is scoped
Windows capture evidence, not complete annotation workflow or macOS acceptance.

## Native overlay geometry and edge fidelity (2026-11)

`web/src/lib/agentBrowserSurfaceCoordinator.js` owns the occlusion geometry sent to
both hosts. `AgentBrowserOcclusionRect` carries `radius` and a `square_corners` bit
mask (1=TL, 2=TR, 4=BR, 8=BL, clamped to 0..15); a set bit means that corner stays
square because the browser viewport clipped that edge. Holes with differing radii
or any non-zero `square_corners` are not merged. The layout signature includes
`radius`, `square_corners` and `interaction_blocked` so a changed hole is not
dropped by deduplication.

Conversion to surface pixels uses `Math.floor` for left/top and `Math.ceil` plus
`HOLE_FAR_EDGE_GUARD = 1` for right/bottom, clamped to the surface bounds. The extra
device pixel compensates for the native window region being a 1-bit mask whose far
(right/bottom) edge is open, which otherwise eats a 1px border. `radius` is clamped
to `floor(min(width,height)/2)` and to 0..32768; IPC parsing defaults `radius` and
`square_corners` to 0 and rejects out-of-range values with
`Agent Browser occlusion rectangle is invalid`.

On Windows, `apply_agent_browser_widget_region` draws `CreateRoundRectRgn` with
`radius*2`, then for each set corner ORs back a `radius×radius` square before
`RGN_DIFF` subtraction, restoring that corner to a right angle. On macOS,
`square_corners == 0xF` draws a plain rect hole, `0` uses `CGPathAddRoundedRect` on
all corners, and mixed masks use `add_occlusion_cornered_hole` with
`CGPathAddArcToPoint` per corner (0 radius for a square corner); the mask uses
even-odd fill with edge antialiasing off. The radius clamp and mask clamp are applied
on both hosts.

To avoid a white halo when an implicit overlay sits on a coloured page,
`NATIVE_SURFACE_IMPLICIT_OVERLAY_BLEED = 1` gives menu/listbox/tooltip overlays a
1 CSS-pixel default bleed, overridable by `data-ace-native-overlay-bleed` clamped to
0..8. The annotation card deliberately declares no bleed and snaps to its own pixel
grid; cards, toolbar and selection menus drop the earlier `0 0 0 1px` halo ring so
their edges stay consistent on any page colour.

## Modal suspension and native input blocking (2026-11)

The runtime accepts `set_modal_suspended {enabled}`: while suspended it hides all
selection/hover/region outlines and the hover card, clears `hovered`, and early-returns
from select/pointer handlers; `start` and `exit` reset it. The session permits this
action with a boolean `enabled` only (`Invalid modal suspension request` otherwise),
exposes `modal_suspended` in the snapshot, and clears it on discard/document replace.

`annotationChromeSuspension.js` reference-counts named sources (menu, dialog) so the
feedback emit fires only on 0→1 and 1→0 edges. A visible WebUI `[role=menu]` under a
MutationObserver suspends chrome; the discard dialog calls `suspendDialogChrome`.
`preview_text`, `preview_style` and `set_modal_suspended` are quiet commands that do
not enter the busy state, so opening a menu cannot grey out the toolbar.

`AgentBrowserBounds.interaction_blocked` keeps the native page painted but ignores
pointer/keyboard input: Windows calls `EnableWindow(widget, FALSE)` while shown and
restores it when hidden; macOS `hitTest:` returns `nil`. The WebUI writes this from a
visible overlapping `[data-ace-native-interaction="disabled"]` marker via
`nativeSurfaceInteractionBlockedByDocument`, and `Modal` supports `nativeOverlayMode`
(overlap/blocking) plus `nativeOverlayInteraction` (disabled/blocking) so a dialog can
keep the page visible while blocking its input.

## Hover-card lifecycle (2026-11)

The hover outline is 1px `Highlight` with a 2px radius; the info card is
168–260px wide with 8px padding and 10px radius, placed 6px below the target and
flipped above when space is short, with colour normalised to `#rrggbb` and alpha 0
shown as `transparent`. It renders only when not modal-suspended, active, target
connected, not dragging, not in region mode, and (when a card is open) in element
selection mode. `pointerout` with no `relatedTarget` and window `blur` both call
`dropHover`, so the card dies on pointer-leave/focus-loss instead of staying pinned.

## Screenshot bridge, localisation and message card (2026-11)

A user-initiated viewport screenshot bridge
`aceDesktop_agentBrowserAnnotationScreenshot({page_id})` returns
`{ok, data, mimeType:'image/png'}` and reports `Invalid annotation screenshot request`
or `Browser closed`; the host adapter's `annotation_dispatch` accepts
`action:'screenshot'` on the viewport path (`{format:'png'}`), yielding
`Annotation capture is already running` when a capture is in flight. This is distinct
from the full-page capture used by the batch sender.

`pageAnnotationBatches.js` localises native errors before display:
`annotation_target_or_document_changed` / `Annotation document or target is invalid`
→ `批注目标已失效，请重新选择元素后再发送`, `capture_document_changed` →
`网页内容已变化，请重新批注后再发送`, `Annotation page is unavailable` →
`批注页面不可用`, `Annotation capture is already running` →
`截图正在进行中，请稍后再试`; missing `sessionId` and missing `api` are now separate
`请先选择对话` / `批注页面不可用` errors. `PageAnnotationTextField` maps the two
English `text_preview.error` strings to Chinese copy. All these strings are in the
i18n catalog with English overrides.

`pageAnnotationMessage.js` parses only payloads headed `Page annotations\n`, keeps
non-empty descriptions, and falls back to index+1 for non-positive numbers.
`pageAnnotationMessageCopyText` builds a title/description plain-text summary with
numbers only for multiple comments. `pageAnnotationScreenshot` recognises
`page-annotations-*.png` attachments; `contentPartsWithoutAnnotationScreenshot`
removes them from the generic attachment strip. `PageAnnotationMessageCard` renders
the title, a `{N} 条注释` footer and a click-to-enlarge screenshot, and is a tracked
production component (not a test helper).

`page_background` (from body/documentElement computed `backgroundColor`, falling
back to `#ffffff`) and `style_preview.previous_values` are additional snapshot
fields used by the UI. `text`/`style` helpers gained `discardAll()`, and runtime
`discard` reverts all text/style layers, clears pins, resets `original`,
`markersHidden` and `modalSuspended`, and the session resets numbering to 1.
