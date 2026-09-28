<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

`SavedModelList` renders `filterSavedModels` in array order. `ModelSettingsSection` loads `/api/models` and notifies `onModelProfileUpdated` after mutations; App uses that revision to refresh pickers. Backend model edits already use `settings_mutations` and `mutate_config` to load the latest config, validate, save, and publish live state. See proposal.md for motivation.

## Goals / Non-Goals

**Goals:** One persisted array order, minimal card changes, safe failure recovery, and desktop-compatible pointer handling.

**Non-Goals:** New sorting modes, provider catalog ordering, default-model changes, or a new drag library.

## Decisions

- Use pointer events, a small SVG handle, and a themed insertion line. Card bodies support mouse dragging; the handle also supports touch and arrow keys. Keep native HTML drag disabled to avoid desktop file-drop interception. Cancel on Escape, pointer cancellation, lost capture, blur, list changes, and unmount; clean listeners and scrolling frames.
- Render the complete existing row as a noninteractive portal preview above the settings mask. Capture the full card rectangle even when dragging the handle, preserve the pointer grab offset and measured dimensions, and update its viewport position with pointer motion. Keep the source in layout as a dim placeholder; only real list rows participate in target detection. Remove the preview through the existing drag cleanup path.
- Resolve visible row targets and move the source within the full model array. Hidden rows retain relative order; dropping on an action without starting a drag does not reorder. Scroll the nearest scrollable ancestor while the pointer approaches its viewport edge.
- POST `/api/config/model-order` with `{names: [...]}`. Validate a full permutation against the latest config, then reuse `run_mutation` with saved-model revision publishing. Never send full profiles back for an order-only operation.
- Optimistically show the order, disable competing mutations, and notify picker refresh on success. Restore the previous list and quietly reload on failure; errors use existing toast behavior. An unchanged order does not write.
- The endpoint lives under `/api/config` to avoid shadowing profiles named `reorder`. Legacy disabled-provider profiles are absent from `/api/models`; validate the visible names and preserve disabled profiles in their original slots.
- A stale model set is rejected with `MODEL_ORDER_CONFLICT`. Concurrent content edits remain safe because the transaction reorders current profiles by name rather than replacing their fields. Simultaneous order-only saves use the existing serialized last-write policy.

## Risks / Trade-offs

- Pointer lifecycle leaks could leave the page dragging: own cleanup in a focused hook and exercise cancellation, unmount, and outside drops in browser checks.
- Web-only mocks cannot verify the desktop host: compile backend changes and exercise real Chromium pointer gestures, while reporting native desktop coverage separately.
- No schema migration is required; the existing `saved_models` array is authoritative. Backend and embedded frontend must be rebuilt together for packaged use.

## Verification

- CMake `acecode_unit_tests` MinSizeRel build passed, including the model HTTP route.
- `SettingsMutations.*`, `ModelsHandler.*`, and `WebServerHttp.SavedModelOrderPersistsAndValidatesRequests` passed (51 distinct relevant tests across the focused runs).
- `pnpm test`, `pnpm build` (including regex compatibility), i18n audit, strict OpenSpec validation, and scoped `git diff --check` passed.
- Chromium exercised the real settings component with isolated API fixtures: card/handle dragging, persisted reload order, default preservation, filtered models, picker revision notification, keyboard bounds, Escape/outside/blur cancellation, pending guards, rollback and conflict refresh, edge scrolling, empty search, and unmount cleanup. No page exceptions occurred. HTTP 500/409 console entries were intentional failure fixtures.
- Native desktop host and an installed daemon were not restarted or visually verified. Package deployment must include both the updated daemon route and frontend assets.

### 2026-09-18 whole-card drag follow-up

- Chromium confirmed full-card dimensions and content equality, pointer grab-offset preservation from both the body and handle, movement within an unchanged drop target, a stationary source placeholder, input isolation, and preview removal on drop, Escape, and unmount. Existing reorder, filtering, failure rollback, and edge-scroll checks also passed.
- `pnpm test`, `pnpm build`, strict OpenSpec validation, and scoped `git diff --check` passed. No backend behavior changed; native desktop host verification was not performed.
