## Why

ACECode currently samples the active thinking animation every 20 ms. Each sample schedules a complete FTXUI frame, and FTXUI hides, redraws, and restores the hardware cursor for that frame. While the model is reasoning, these scheduled frames compete with keyboard input and repeat work whose cost grows with conversation history, causing delayed typing and visible cursor flashing.

The animation already derives its phase from elapsed time, so smooth motion does not require rendering every 20 ms. The TUI needs a render policy that keeps feedback alive while adapting to user activity and the real cost of a completed terminal frame.

## What Changes

- Replace the fixed thinking-animation cadence with adaptive pacing that responds to recent keyboard activity and measured full-frame cost.
- Coalesce high-frequency scheduled redraws so thinking ticks and streamed deltas cannot build an unbounded queue of redundant frames.
- Preserve elapsed-time animation phase, drag-autoscroll priority, and the slower legacy-console compatibility cadence.
- Compute tool-call dots and paired tool-result names for the visible render window and its adjacent tool batch in one bounded pass instead of rescanning the entire conversation twice per frame.
- Add deterministic unit and stress-style regression coverage for cadence selection, redraw backpressure, generation races, and bounded tool metadata derivation.
- Keep user-facing text, session persistence, daemon/Web/Desktop protocols, and the vendored FTXUI implementation unchanged.

## Capabilities

### New Capabilities

- `tui-render-pacing`: Keeps scheduled terminal rendering responsive through adaptive cadence, redraw coalescing, and bounded per-frame metadata derivation.

### Modified Capabilities

None.

## Impact

- TUI runtime and rendering code in `src/main.cpp` and `src/tui/`.
- Pure helpers and unit tests under `tests/tui/`.
- Testable-library source registration in `CMakeLists.txt`.
- No external API, configuration, persisted-session, model-provider, Web UI, Desktop UI, or submodule changes.
