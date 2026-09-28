<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

SectionUsage in SettingsPage.jsx owns a 30-day request and inline bars. The existing usage endpoint supports up to 366 daily buckets, including empty dates; normalizeUsageStats retains those buckets and their metadata. Date strings follow the request's fixed timezone offset. Existing tooltips demonstrate body portals with native-overlay coordination.

## Goals / Non-Goals

**Goals:** Isolate calendar calculation and interaction in focused modules, preserve nearby statistics, and match the reference's compact rounded squares and quiet headings.

**Non-Goals:** No ledger, API, billing, historical backfill, theme-package, or native-shell changes.

## Decisions

- Request 365 days independently from the existing 30 days. Each request owns its loading/error state and honors unmount/reload cancellation. This preserves summary semantics and allows either section to succeed independently.
- Use date-only UTC arithmetic on backend date strings, avoiding a second timezone conversion or DST-induced missing dates. Pad Monday-start weeks with null cells outside the window.
- Daily/cumulative modes use seven rows. Weekly mode uses one merged vertical cell per week within the same chart area. Cumulative labels explicitly say the displayed window is the source of the running total.
- Quantize positive values into theme-accent intensity levels; zero uses a neutral surface. No hard-coded palette or third-party chart dependency.
- Use a body-portal tooltip with measured viewport placement, existing anchoredMenuPosition helper, native-overlay attribute, exact localized numbers, and hover/focus/tap dismissal. Roving keyboard focus avoids hundreds of Tab stops; arrow keys move through dates or weeks.
- Put calendar and usage-summary styles alongside the new component to avoid modifying the already-dirty global theme file.
- Match the corrected reference at a maximum content width of 732px. Render the three existing summary values above their labels in one 62px-high bordered strip, with two vertical dividers and no responsive stacking. Preserve auxiliary information in accessible labels and native tooltips.
- 使用情况整页通过统一容器共用 Token 活动图的 732px 最大宽度与左右自动外边距，在设置内容区水平居中；标题与刷新按钮、三项摘要、活动图、六项 Token 统计、模型明细、工作区明细及提示状态都保持相同的左右边界。窄窗口下填满可用宽度，保留 Token 统计原有两列/六列断点。
- Use an SVG viewBox for the heading, mode controls, entire dot matrix, and month axis. Compute equal square cells at approximately 11px with 3px gaps and 3px corner radii at the maximum width; reduce all geometry uniformly at narrower widths. Keep every in-range date, including when the calendar has 53 columns rather than the reference's 52. Render the last twelve month names at twelve equal axis positions, as explicitly requested, rather than variable-width calendar-month boundaries. Exact dates remain available per cell. Remove the visible range and intensity legend, retaining the cumulative range in accessible descriptions and tooltips.

## Risks / Trade-offs

- One additional ledger query per refresh adds work; keep requests bounded and reuse both results while toggling views.
- Fixed-offset calendar grouping inherits the API's existing behavior across historical DST changes; preserve the authoritative date buckets.
- Very narrow layouts produce small cells; preserve the full year without horizontal scrolling as explicitly requested, and retain keyboard navigation and exact token tooltips.
- Tooltips inside settings may otherwise clip; portal them to document.body, dismiss pointer details on viewport/scroll changes, and reposition keyboard details when focus navigation scrolls the calendar. Consume Escape before the settings window's close listener.
