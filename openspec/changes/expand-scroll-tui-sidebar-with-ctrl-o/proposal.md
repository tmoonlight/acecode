## Why

The wide TUI sidebar folds MCP servers, changed files, and TodoWrite items behind
`+N more` rows with no way to inspect the complete lists. `Ctrl+O` already acts
as the TUI-wide detail toggle for transcript tool rows, so it should reveal the
sidebar details as part of the same view mode.

## What Changes

- Extend `Ctrl+O` so the regular right sidebar switches between its existing
  compact summary and an expanded detail view.
- In the expanded view, show every MCP server, changed file, and TodoWrite item
  in one continuous top-to-bottom flow while preserving the existing section
  order and per-row compact formatting.
- Give the expanded sidebar an independent vertical viewport and scrollbar;
  wheel and scrollbar gestures over the sidebar scroll it without moving the
  transcript.
- Restore the existing capped, top-and-bottom anchored layout when `Ctrl+O` is
  turned off, and reset the expanded sidebar viewport to the top when toggling
  modes.
- Keep narrow-terminal and ConHost-compatible layouts unchanged because they do
  not render the regular right sidebar.

## Capabilities

### New Capabilities

- `tui-sidebar-expanded-view`: Defines the Ctrl+O expanded sidebar flow,
  independent scrolling, scrollbar interaction, and compact-mode restoration.

### Modified Capabilities

- `todowrite-checklist`: The regular TUI sidebar can reveal every TodoWrite item
  under Ctrl+O while retaining the existing compact checklist by default.

## Impact

- Affects TUI-only runtime state, sidebar composition, mouse event routing, and
  focused TUI unit tests.
- Reuses the existing Ctrl+O reveal state and FTXUI scrollbar primitives.
- Does not change TodoWrite storage, MCP/LSP behavior, file-change collection,
  Web/Desktop rendering, daemon APIs, or persisted session data.
