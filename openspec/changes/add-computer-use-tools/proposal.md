## Why

ACECode can already drive a web page. `src/tool/agent_browser/` registers
fifteen CDP-backed tools that navigate, read a semantic accessibility snapshot,
click by element reference, and capture a PNG that reaches the model through
`ToolResult::attachments`. Everything outside a Chromium page is unreachable:
the repository contains no screen capture and no synthetic input path on any
platform, so an agent cannot operate a native installer, a desktop IDE, a
settings dialog, or any application that is not a browser.

The GUI-agent stacks that do solve this — `bytedance/UI-TARS-desktop` being the
most complete open one — converge on a very small operator contract: capture the
screen, execute one pointer or keyboard action, repeat. Their value is not the
surrounding machinery. UI-TARS wraps that contract in its own agent loop, its
own `Thought:/Action:` text DSL, its own parser, and a model family trained to
emit that DSL. ACECode already owns an agent loop, a tool protocol, permission
gating, abort propagation, and an image-to-context pipeline, so adopting the
wrapper would mean running a second opaque agent inside a tool call.

This change takes the operator contract and the coordinate handling that make
those stacks work, and exposes them as ordinary ACECode tools driven by the
existing `AgentLoop` and any vision-capable configured model.

## What Changes

- Add a `ComputerBackend` abstraction under `src/tool/computer_use/` that
  captures the screen and executes single pointer/keyboard actions, with a
  Windows implementation.
- Register a built-in tool group — `computer_screenshot`, `computer_click`,
  `computer_move`, `computer_drag`, `computer_scroll`, `computer_key`, and
  `computer_type` — using ordinary JSON arguments and the existing
  `ToolResult::attachments` path for captures.
- Gate the whole group behind a new `config.computer_use` section that is
  disabled by default, and skip registration entirely on platforms without a
  backend, mirroring how `agent_browser` and `lsp` are gated.
- Add a pure coordinate layer that converts between captured-image pixels,
  physical screen pixels, and logical desktop coordinates so DPI scaling and
  multi-monitor origins do not silently misplace clicks.
- Add a session-scoped control grant: the first action tool in a session raises
  one permission decision that covers the sequence, rather than one decision per
  click, and the grant is revoked on session end, abort, or `/computer off`.
- Add a capture budget that bounds screenshot pixel dimensions and per-turn
  capture count without retroactively rewriting earlier conversation content.
- Add a `/computer` command that reports backend availability, grant state, and
  screen geometry, and revokes an active grant.

## Capabilities

### New Capabilities

- `computer-use-tools`: Defines OS-level screen capture and synthetic pointer
  and keyboard control as gated built-in tools, including the coordinate
  contract, the session-scoped control grant, and the capture budget.

### Modified Capabilities

None.

## Impact

- New `src/tool/computer_use/` sources and the built-in registration chain in
  `src/tool/builtin_tool_registry.hpp`.
- `AppConfig` gains a `computer_use` section; `config.json` schema notes and
  documentation follow.
- Command registration for `/computer` in both the TUI builtin command table and
  the daemon builtin allowlist.
- Windows link surface gains `User32`/`Gdi32` usage for input and capture.
- Focused unit tests for the pure coordinate, budget, and grant-state logic, and
  a gated manual smoke path for the native backend.
- `README.md`, `README_CN.md`, and the repository capability notes.
