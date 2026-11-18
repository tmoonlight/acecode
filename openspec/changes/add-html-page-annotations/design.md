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
