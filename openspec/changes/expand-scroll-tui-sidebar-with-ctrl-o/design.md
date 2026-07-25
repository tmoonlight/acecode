## Context

The regular TUI sidebar is rendered only in the wide, non-ConHost layout. Its
single renderer currently builds a capped top group, a `filler()`, and a bottom
group. MCP servers stop at 8 rows, changed files stop at 10 rows, and TodoWrite
stops at 10 rows. This composition keeps compact status visible but cannot
display an arbitrarily tall list.

`TuiState::transcript_expanded` is the runtime-only Ctrl+O reveal gate. The chat
already owns a separate `yframe` viewport and scrollbar, so the sidebar must not
reuse chat scroll position or chat mouse-drag state.

## Goals / Non-Goals

**Goals:**

- Reuse Ctrl+O as one global TUI detail toggle.
- Preserve the existing compact sidebar without visual or ordering regressions.
- Render all rows from capped sidebar sections in a continuous expanded flow.
- Give the expanded sidebar independent row scrolling and scrollbar input.
- Keep all scroll state runtime-only and clamp it as content changes.

**Non-Goals:**

- Changing sidebar width, activation threshold, or ConHost behavior.
- Removing the two-line limit for one TodoWrite item's text.
- Adding keyboard focus or PageUp/PageDown navigation to the sidebar.
- Changing TodoWrite, MCP, LSP, file-change, or background-task data.
- Changing Web/Desktop behavior.

## Decisions

### Reuse the existing Ctrl+O state

`transcript_expanded` will also select the expanded sidebar renderer. A second
toggle would make two competing notions of "show details" and would not satisfy
the requested one-keystroke behavior. Ctrl+O will reset the sidebar scroll
position when either entering or leaving expanded mode.

### Keep compact and expanded compositions separate

Compact mode will retain the current `top_rows + filler + bottom_rows`
composition and current caps. Expanded mode will concatenate the same sections
into one `vbox` in this order:

1. Session title
2. MCP
3. LSP
4. Files Changed
5. TodoWrite
6. Background Tasks
7. Version, update/status, model, and working directory

The expanded branch removes only count-based folding. Existing filename
truncation, TodoWrite status ordering, and two-line TodoWrite row formatting
remain in place so one long value cannot monopolize the narrow pane.

### Use an independent FTXUI viewport

The expanded flow will be wrapped in `reflect_unclipped`, `focusPosition`, the
existing thick vertical scrollbar decorator, and `yframe`. Separate reflected
boxes will publish the sidebar content, viewport, and scrollbar track geometry.
A runtime `sidebar_scroll_top_row` will be converted to the FTXUI focus position
without changing chat focus.

The scrollbar reserves its own narrow hit area inside the existing 43-column
sidebar; the outer sidebar width remains unchanged. It is visually empty when
the content fits, matching the existing scrollbar decorator contract.

### Route pointer input by pane geometry

Wheel events whose pointer is inside the expanded sidebar viewport will adjust
only `sidebar_scroll_top_row`. Press, move, and release events on the sidebar
scrollbar will use sidebar-specific drag state and map track position to the
sidebar row offset. Chat wheel, selection, and scrollbar handling remain the
fallback for chat geometry.

The row offset will be clamped against the latest reflected content and viewport
heights before scrolling or positioning. Content shrinkage therefore cannot
leave the viewport beyond the new end.

### Keep scroll math testable

Pure helpers will own offset clamping, line-step scrolling, viewport focus
position, and scrollbar track mapping. Rendering tests will cover compact versus
expanded row visibility, while helper tests will cover overflow and boundary
behavior without launching the interactive TUI.

## Risks / Trade-offs

- [Expanded footer is no longer pinned] → This is intentional in the continuous
  flow; toggling Ctrl+O off restores the pinned compact footer.
- [Dynamic content changes after a render] → Clamp against reflected row counts
  on the next render/input event and reset on mode changes.
- [Sidebar events interfere with chat selection] → Hit-test the sidebar before
  entering chat scrollbar or drag-selection branches and use distinct drag
  state.
- [Scrollbar columns reduce expanded content width slightly] → Account for the
  reserved columns when wrapping rows and keep the overall sidebar width fixed.
- [Very large lists increase DOM work] → Expansion is explicit and runtime-only;
  compact mode retains caps and remains the default.

## Migration Plan

No persisted state or data migration is required. The feature can be rolled back
by removing the expanded renderer and sidebar runtime scroll fields; compact
behavior remains the unchanged fallback.

## Open Questions

None. The requested flow, Ctrl+O reuse, and independent scrollbar behavior are
fully specified.
