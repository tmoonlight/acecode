## 1. Scroll Model and Runtime State

- [x] 1.1 Add pure sidebar viewport helpers for row clamping, line-step scrolling, FTXUI focus placement, and scrollbar track mapping.
- [x] 1.2 Add runtime-only sidebar scroll and drag state plus reflected sidebar content, viewport, and scrollbar geometry.
- [x] 1.3 Reset sidebar scroll and drag state whenever Ctrl+O changes detail mode.

## 2. Sidebar Rendering

- [x] 2.1 Parameterize MCP and TodoWrite count limits so compact mode keeps existing caps and expanded mode renders every item.
- [x] 2.2 Compose expanded sidebar sections as one ordered flow without the compact-mode filler.
- [x] 2.3 Wrap the expanded flow in an independent FTXUI viewport and thick scrollbar while preserving the 43-column outer width.

## 3. Sidebar Pointer Interaction

- [x] 3.1 Route mouse-wheel events over the expanded sidebar to its row offset without changing chat scroll.
- [x] 3.2 Implement sidebar scrollbar press, drag, and release handling with valid-range clamping.
- [x] 3.3 Preserve existing chat scrollbar, drag-selection, AskUserQuestion, narrow-terminal, and compact-sidebar event behavior.

## 4. Verification

- [x] 4.1 Add unit coverage for sidebar scroll helper boundaries and pointer-to-offset mapping.
- [x] 4.2 Add rendering coverage for compact caps, expanded full lists, retained TodoWrite formatting, continuous section order, and overflow scrollbar behavior.
- [x] 4.3 Build the focused TUI test target and run the relevant sidebar, TodoWrite, scroll, and chat regression tests.
- [x] 4.4 Run the OpenSpec validator, repository quality check, full available unit suite, and `git diff --check`.
