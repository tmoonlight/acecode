> **Superseded（2026-11-22）**：本文档描述的路线（预览为主 / 整页截图 / 不可变记录 / 收据去重 / 导航确认）已被 `openspec/changes/redesign-html-page-annotations/` 取代，不再维护。
> 现行方案见 `docs/specs/2026-11-21-html-page-annotations-technical-design.md`，WIP 现状见 `docs/specs/2026-11-22-html-page-annotations-wip-alignment.md`。

# Proposal

## Why

Desktop users cannot point at rendered elements or regions when requesting HTML/CSS changes. Source-selection annotations cannot capture visual intent, reversible previews or a full-page visual reference.

## What Changes

- Add Windows/macOS Agent Browser page annotations with host-controlled popup, element markers and reversible text/style previews.
- Save immutable numbered pins with multiple element targets and at most one region per annotation; preserve previews and pins through original-view comparison and sending.
- Send pending annotations through the existing conversation boundary in one acknowledged, deduplicated batch with one full-page screenshot; protect drafts and navigate/refresh safely.
- Extend full-page capture on both native platforms without changing existing viewport capture defaults.
- Preserve source-selection annotation behavior and untrusted-page isolation.

## Capabilities

### New Capabilities

- `html-page-annotations`: Desktop page selection, reversible preview, saved pins, guarded batch submission and failure handling.

### Modified Capabilities

None. The capability inventory has no existing main specs.

## Impact

Native desktop browser hosting/bridges and screenshot adapters, trusted WebUI browser surface coordination and conversation submission, submission receipt/deduplication, and corresponding tests/docs. No Linux/TUI, standalone HTML file rendering, vendored changes or new page-accessible conversation capabilities. Spec #193; tickets #194–201.
