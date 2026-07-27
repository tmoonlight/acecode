## Context

The modern TUI currently wakes every 20 ms while the thinking row is visible. A wake posts `Event::Custom`; FTXUI then rebuilds the ACECode element tree, rasterizes the terminal frame, hides the hardware cursor, writes the frame, and restores the cursor. The elapsed-time shimmer is deterministic, but its sampling rate causes up to 50 complete redraw requests per second even when no semantic state changed.

Two properties amplify the issue:

- streamed assistant deltas also post an uncoalesced custom event for every token chunk;
- each frame derives tool-call status and result identity with two full-conversation scans even though the transcript renderer already has a bounded visible window.

The implementation must remain in ACECode-owned code. The vendored FTXUI fork, persisted sessions, and non-TUI surfaces are outside the change.

## Goals / Non-Goals

**Goals:**

- Keep keyboard-driven frames immediate while reducing background redraw competition.
- Preserve a visibly live, elapsed-time-based thinking animation.
- Adapt background cadence to recent typing and observed completed-frame latency.
- Bound queued periodic redraw work and visible-frame tool metadata work.
- Make pacing and pairing behavior deterministic and unit-testable.

**Non-Goals:**

- Replacing FTXUI, changing its terminal diff protocol, or modifying the submodule.
- Caching or incrementally parsing all Markdown in this change.
- Changing animation colors, wording, speed, session data, or daemon/Web/Desktop behavior.
- Throttling correctness-bearing UI events such as permission overlays, completion, or explicit keyboard input.

## Decisions

### 1. Separate immediate events from coalescible background redraws

Keyboard events and correctness-bearing callbacks continue to invalidate the screen directly. Thinking ticks and streamed deltas use a new `TuiRedrawPacer`.

The pacer assigns a generation to a scheduled redraw and accepts another scheduled request only after a frame containing the current generation has completed. Repeated requests while that generation is queued or being drawn are coalesced. If a request arrives after a different frame has started, its newer generation is not cleared by the older frame's completion.

This keeps background work bounded without allowing a late state change to be mistaken for work already rendered. A simple atomic boolean was rejected because clearing it after a frame can lose a request that arrived after that frame took its state snapshot.

### 2. Measure completed loop-frame latency without changing FTXUI

At the start of each ACECode render, the renderer captures a pacer generation and monotonic timestamp. It posts a non-invalidating FTXUI closure that runs after the current draw/flush cycle and completes that frame ticket. The elapsed value is a conservative loop-frame latency: it includes ACECode render work, FTXUI rasterization/output, and any normal frame-cap handoff.

Measuring only `render_tui_frame()` was rejected because it excludes terminal serialization and flush—the portion directly associated with cursor hide/show behavior. Adding a callback inside the FTXUI submodule was rejected to keep this change isolated from vendored code.

### 3. Use explicit adaptive cadence floors and a frame-cost backoff

The modern pacing policy uses named constants:

| Situation | Base interval |
|---|---:|
| drag autoscroll | 50 ms |
| legacy console compatibility | 1000 ms |
| visible thinking, no recent keyboard input | 60 ms |
| streamed delta, no recent keyboard input | 50 ms |
| thinking or streaming within 750 ms of keyboard input | 250 ms |
| ordinary legacy animation/status polling | 300 ms |

For thinking and streaming, the selected interval is at least three times the last completed loop-frame latency, capped at 400 ms. The three-times backoff targets at most roughly one quarter of wall time in background redraws when the cap is not reached. The cap prevents a costly frame from making live feedback appear frozen.

Drag autoscroll remains highest priority, and conhost compatibility remains slower than the modern layout. The shimmer's visual position continues to come from elapsed time, so fewer sampled frames do not slow or accumulate phase error.

A fixed return to the old cadence was rejected because it cannot account for terminal speed or long-history frame cost. Dynamically reducing animation velocity was rejected because it would change the intended visual motion rather than merely how often that motion is sampled.

### 4. Let keyboard frames carry current background state

The event handler records monotonic time for real keyboard events, excluding custom, mouse, cursor-position, and cursor-shape events. Keyboard events themselves remain immediate; while typing is recent, periodic animation and streaming redraws fall back to 250 ms. The keyboard-triggered frames still sample the current shimmer phase and newest streamed text, so responsiveness takes priority without freezing feedback.

### 5. Derive paired tool metadata for the render window in one pass

A new pure helper accepts the requested message range. If either range edge cuts through a contiguous `tool_call`/`tool_result` batch, it expands only to that batch boundary, then computes call dots and paired result names together with the existing FIFO rules.

The renderer indexes this compact result rather than allocating and filling two conversation-sized vectors. Existing full-history helper APIs remain available for non-frame paths and compatibility tests, implemented through the same canonical pairing logic.

Expanding to the contiguous batch boundary is necessary for correct pairing when the visible window begins at a result or ends at an unresolved call. A global incremental cache was rejected for now because every append, replay, abort boundary, and in-place summary update would need invalidation; the bounded pure helper removes the known per-frame scan with much lower state risk.

## Risks / Trade-offs

- **[Risk] A 60 ms idle cadence is less densely sampled than 20 ms.** → The shimmer remains elapsed-time based with unchanged speed and interpolation; user input immediately produces additional frames.
- **[Risk] The posted completion closure measures conservative loop latency rather than isolated terminal-write time.** → Conservative backoff is desirable for responsiveness, and values are clamped before cadence selection.
- **[Risk] A streamed delta can be coalesced before the next allowed background frame.** → The waiting ticker supplies later frames, keyboard frames include current state, and completion/busy transitions still post directly.
- **[Risk] A visible range can split a parallel tool batch.** → The metadata helper expands both edges through the contiguous tool-role run before applying FIFO pairing.
- **[Risk] Very expensive frames can exceed the intended redraw duty cycle at the 400 ms cap.** → The cap preserves liveness; further Markdown/layout caching remains a separate measurable optimization if needed.

## Migration Plan

1. Add and unit-test the pure pacing and redraw-generation helpers.
2. Add the bounded tool metadata helper and equivalence/boundary tests.
3. Route only thinking ticks and streamed deltas through the pacer.
4. Record keyboard activity and completed-frame tickets in the main TUI renderer.
5. Build and run focused and full unit tests plus the repository quality check.

Rollback is a source revert: no configuration, protocol, or persisted-data migration is involved.

## Open Questions

None. The cadence constants are intentionally named and covered by deterministic tests so future tuning does not require changing the scheduling architecture.
