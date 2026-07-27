## ADDED Requirements

### Requirement: Scheduled redraws are backpressured

The TUI SHALL coalesce periodic thinking and streaming redraw requests so no newer periodic request is accepted while the currently requested generation has not completed a frame. Immediate keyboard and correctness-bearing UI events MUST remain outside this throttle.

#### Scenario: Repeated periodic requests arrive before completion

- **WHEN** multiple thinking ticks or streamed deltas request a redraw before the accepted generation completes
- **THEN** the TUI schedules one periodic redraw and coalesces the remaining requests

#### Scenario: A request arrives after an older frame starts

- **WHEN** a new periodic request is accepted after a frame captured an older generation
- **THEN** completing the older frame does not mark the newer generation complete

#### Scenario: A correctness-bearing state transition occurs

- **WHEN** a permission overlay, terminal status transition, explicit input event, or turn completion requires rendering
- **THEN** the TUI posts that render directly without waiting for the periodic redraw interval

### Requirement: Background cadence adapts to interaction and frame cost

The TUI SHALL select thinking and streaming intervals from named cadence constants, SHALL slow background redraws while keyboard input is recent, and SHALL apply a bounded backoff derived from completed loop-frame latency.

#### Scenario: Thinking is visible without recent typing

- **WHEN** the modern thinking row is visible, keyboard input is not recent, and measured frame cost does not require backoff
- **THEN** the ticker uses the 60 ms thinking interval

#### Scenario: The user types while the model is active

- **WHEN** keyboard input occurred within the 750 ms recent-input window
- **THEN** thinking and streaming background redraws use an interval of at least 250 ms while keyboard frames remain immediate

#### Scenario: Completed frames are expensive

- **WHEN** three times the last completed loop-frame latency is greater than the applicable base interval
- **THEN** the TUI uses the cost-derived interval up to the 400 ms liveness cap

#### Scenario: Drag or compatibility mode has priority

- **WHEN** drag autoscroll is active
- **THEN** the ticker uses its 50 ms cadence
- **WHEN** drag autoscroll is inactive and legacy console compatibility is active
- **THEN** the ticker uses its 1000 ms cadence

### Requirement: Animation phase is independent of render count

The thinking shimmer SHALL continue to derive visual position from elapsed time rather than from the number of rendered frames.

#### Scenario: Intermediate frames are coalesced

- **WHEN** several scheduled animation frames are skipped and a later frame is rendered
- **THEN** its shimmer position equals a frame computed directly for that same elapsed timestamp

### Requirement: Visible tool metadata work is bounded

The transcript renderer SHALL compute tool-call status and paired result identity in one pass over the visible message window plus only the contiguous tool batch intersected by a window edge.

#### Scenario: The visible window cuts through a parallel tool batch

- **WHEN** the first or last visible message lies inside a contiguous `tool_call`/`tool_result` run
- **THEN** the metadata range expands to that run's boundary and preserves FIFO call/result pairing

#### Scenario: Distant history is outside the visible window

- **WHEN** a long conversation contains non-tool message boundaries between distant history and the visible window
- **THEN** the per-frame metadata result contains no entries for that distant history

#### Scenario: Full-history compatibility helpers are used

- **WHEN** an existing caller requests call dots or result names for the entire conversation
- **THEN** it receives results equivalent to the bounded helper applied to the full range
