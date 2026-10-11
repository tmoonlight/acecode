## Context

`skill_init.cpp` is shared by TUI, daemon, tools and session initialization. It splits project/global roots. Settings has a dedicated Configuration section and search locating behavior. Skill policy refresh already publishes snapshots via session control callbacks.

## Goals / Non-Goals

Goals: one editable ordered list, consistent runtime/UI resolution, backward-compatible absence, persistence-before-refresh.
Non-goals: changing seed installation locations, importing skill content, packaging or release.

## Decisions

- Use `optional<vector<string>> scan_paths` instead of repurposing `external_dirs`: preserves absent vs empty and lets old configuration retain exact discovery order.
- Custom list entries resolve in order. Relative entries expand along workspace ancestors; absolute/`~` paths are global. `${ACECODE_DATA_DIR}` denotes the active data directory; `${XDG_CONFIG_HOME}` and `${XDG_CACHE_HOME}` fall back to `~/.config` and `~/.cache`. Other `${ENV}` require a nonempty value. This keeps one human-editable list instead of exposing separate arrays and switches.
- Default editor values include project-relative roots and global templates, honoring legacy `reuse_opencode` and `external_dirs`. Unchanged drafts are not saved. Explicit custom save uses literal list priority, including interleaved global/project paths. Restore sends null.
- Keep existing project/global helper projections for grouping, add the shared ordered resolver for initialization and UI duplicate selection. Expert roots remain prepended.
- GET/PUT `/api/config/skill-paths` exposes effective editable `paths`, `default_paths`, and `customized`. PUT requires `paths` (string array or null), validates before atomically mutating only `skills.scan_paths`, then refreshes daemon/session policies under the existing config lock.
- Configuration uses a multiline editor with save/reset and help. Skills links internally to Configuration, focuses the named textarea after mount/loading, and reuses existing focus styling. Register all new visible copy in i18n and add a search entry.
- Follow-up deletion fix: the settings catalog is based solely on discoverable skills. Remove the Web API's synthetic rows derived only from `skills.disabled`. Keep disable preferences without pruning during GET: a missing skill may exist in another workspace, on a temporarily unavailable directory, or be restored later. Existing disabled skills retain their full metadata; a restored same-name skill remains disabled.
- Follow-up pagination: preserve the global/workspace grouping and complete API arrays, but slice each filtered list before creating card elements. One section-level page-size preference uses the existing `usePreference` helper with 20/50/100 and a default of 50. Each list owns its page; query/page-size changes reset to 1, refreshed shorter lists clamp the page, toggles preserve it. Top and bottom controls make long pages navigable; page changes scroll/focus the list's top controls. Counts still use the full lists. This addresses render volume without changing disk scan or API pagination contracts.

## Risks / Trade-offs

- Reordering a custom list can override a project skill with a global one -> explain first-match priority and test UI/runtime agreement.
- Missing directories/environment variables -> skip without creating arbitrary paths; preserve entries so removable/offline paths can later work.
- Native default install/open paths may be excluded by a custom list -> keep seed location unchanged; directory opening should follow configured roots where customized.

## Migration Plan

No eager user-config rewrite. Missing/null uses old paths; reset removes the override. Existing `external_dirs` and `reuse_opencode` remain stored and resume on reset.
