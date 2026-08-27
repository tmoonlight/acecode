## Context

ACECode's tool boundary already carries everything an OS-level operator needs.
`ToolResult::attachments` accepts a `{name, mime_type, path}` descriptor;
`AgentLoop::materialize_result_attachments` turns it into a session attachment,
`output_attachments_to_content_parts` turns that into content parts, and the
provider layer emits an `image_url` part for vision-capable models. This is the
exact path `browser_screenshot` uses today, so a desktop capture needs no new
transport.

What is missing is entirely below that boundary. No source file in the
repository calls `SendInput`, `XTestFakeButtonEvent`, `CGEventPost`, `BitBlt`,
or any equivalent. `agent_browser` synthesizes input through the CDP `Input`
domain, which cannot leave the page.

UI-TARS-desktop (Apache-2.0) is the reference for the missing layer. Its
`BaseOperator` is two methods, `screenshot()` and `execute(action)`, and its
`nut-js` operator is roughly 325 lines that are mostly a key-name map. The parts
worth taking are the action vocabulary, the physical-to-logical downsample
before the image reaches the model, and the resize-factor arithmetic in
`action-parser`. The parts worth refusing are the `Thought:/Action:` DSL, the
regex action parser, and the separate `GUIAgent` loop, all of which exist to
serve a model family trained on that specific text format.

Two ACECode invariants constrain the design more than the platform work does.
Permission decisions are per tool call, but a GUI task is tens of calls. And the
prompt-cache prefix invariant recorded in `CLAUDE.md` means any retroactive edit
to already-sent conversation content invalidates the cache from that point to
the end of the turn.

## Goals / Non-Goals

**Goals:**

- Expose screen capture and synthetic pointer/keyboard control as ordinary
  built-in tools that the existing `AgentLoop` drives.
- Keep the model's coordinate space unambiguous and independent of DPI scaling,
  monitor layout, and capture downsampling.
- Make the capability opt-in, revocable mid-session, and observable while it is
  active.
- Bound image cost per turn without rewriting conversation history.
- Keep the backend contract narrow enough that macOS and X11 implementations are
  additive.

**Non-Goals:**

- Porting the UI-TARS `Thought:/Action:` DSL, its action parser, or its
  `GUIAgent` loop.
- Requiring or bundling a specific vision model. Model choice stays with
  `saved_models` and the existing `vision` capability tag.
- macOS and Linux backends. This change lands the contract and the Windows
  backend; the other platforms are separate follow-ups.
- Wayland. Screen capture and input injection there require
  `xdg-desktop-portal` plus `libei` and do not fit this contract.
- Remote or sandboxed desktops, including the UI-TARS AIO HTTP operator.
- Element-level semantics such as an accessibility tree. This layer is
  deliberately pixel-only; `agent_browser` remains the semantic path for web
  content.

## Decisions

### Expose plain JSON tools rather than a nested agent

Each action is a normal tool with a JSON schema, so tool calls appear in the
transcript, flow through `PermissionManager`, honor `ctx.abort_flag`, render
through the existing TUI and web tool rows, and count toward the session's own
token accounting. A single `gui_task(instruction)` tool that runs an inner loop
was rejected: it would hide every intermediate step from the permission gate and
the UI, and would spend tokens against a model the session does not track.

The action set is taken from UI-TARS's vocabulary because it is small and
proven, split so that each verb maps to one tool rather than one DSL string:
`computer_screenshot`, `computer_click` (with `button` and `count`),
`computer_move`, `computer_drag`, `computer_scroll`, `computer_key` (chords and
explicit hold/release), and `computer_type`. `wait`, `finished`, and `call_user`
are not ported; ACECode already terminates on a text-only reply, has
`task_complete`, and has `AskUserQuestion`.

Only `computer_screenshot` is registered with `is_read_only = true`. Every
action tool is write-class so it cannot be auto-approved.

### Split a narrow backend interface from platform code

`ComputerBackend` declares `capture()`, `pointer_move`, `pointer_button`,
`pointer_scroll`, `key_event`, and `type_text`, plus a `ScreenGeometry` query.
Platform bodies live in `computer_backend_win.cpp`, and later
`computer_backend_mac.mm` and `computer_backend_x11.cpp`, following the
`_win` / `_mac` / `_posix` split already used by `notifications`,
`folder_picker`, `single_instance`, and `proxy_resolver`.

The Windows backend uses `SendInput` for pointer and keyboard events and a
`BitBlt` desktop capture. DXGI Desktop Duplication is faster but requires
device-lost handling and a redistributable code path; the capture rate here is
one frame per model turn, so `BitBlt` is chosen for simplicity and is an
internal detail the interface does not expose.

When no backend exists for the target platform, `register_computer_use_tools`
registers nothing, exactly as `register_agent_browser_tools` does outside
`_WIN32 || __APPLE__`. Tools that are absent are absent from the tool list
rather than present-and-failing, so the model never plans around a capability it
cannot use.

### Make the captured image the single coordinate space

Three spaces exist: logical desktop coordinates that `SendInput` consumes,
physical pixels that the framebuffer holds, and the pixel grid of the image the
model actually sees after downsampling. Bugs in this area are silent — clicks
land near, but not on, the target — so the contract is fixed explicitly.

Every action tool takes `x` and `y` in **captured-image pixels of the most
recent `computer_screenshot` in this session**, with the origin at the image's
top-left. A pure `computer_coords` translation unit converts that to logical
desktop coordinates using the capture's recorded scale and virtual-desktop
origin. The capture records its geometry so a later action can be rejected when
the screen configuration changed after the screenshot, instead of clicking a
stale location.

Capture downsamples physical pixels to logical pixels before encoding, following
the `nut-js` operator's `pixelDensity` handling, so a HiDPI display does not
double every coordinate. The resize-factor arithmetic in UI-TARS's
`smartResizeForV15` is ported into the same unit: models in the Qwen-VL family
are trained on images resized to a multiple of a fixed factor, and feeding a
native-resolution frame degrades grounding without any error surfacing. This
arithmetic is pure and unit-testable, and it is the piece most worth taking from
the reference implementation.

Normalized 0–1000 coordinates, which UI-TARS models emit natively, are not
accepted. They exist to serve one model family's output format; a general tool
contract in absolute image pixels is checkable and does not silently rescale.

### Bound capture cost without rewriting history

A capture is clamped to a configured maximum edge length before encoding, and a
per-turn capture count limit rejects further screenshots with an explanatory
result rather than a hard failure.

Screenshots are deliberately **not** pruned retroactively. A sliding image
window of the kind UI-TARS uses in `processVlmParams` is correct for a
stateless per-request assembly, but in ACECode the same technique would mutate
already-sent conversation content and invalidate the prompt cache from the
mutation point through the end of every subsequent request in the turn. That
cost grows with exactly the long tool-heavy turns this feature produces. Cost is
therefore controlled at capture time — fewer, smaller images — and long sessions
fall back to the existing compaction path, which is already a cache-boundary
event.

### Grant control once per session, not once per click

The first action tool call in a session raises one permission decision that
describes the capability rather than the individual click. Once granted, action
tools in that session proceed without further prompts. The grant is stored in
session-scoped runtime state, is never persisted, and is dropped on session end,
on user abort, and on `/computer off`.

Per-call confirmation was rejected as unusable: a short GUI task is tens of
calls, and a prompt the user answers reflexively is not a safety control. A
persisted grant was rejected because the blast radius — full synthetic input to
every application the user is signed into — is larger than any other tool in the
codebase and should not survive a restart silently.

`computer_screenshot` is read-only and does not require the grant, so the model
can look before asking to act.

While a grant is active the surfaces that already show session state show that
the session can control the machine, and the existing abort path drops the grant
so the visible stop control is also the kill switch.

### Keep the capability disabled by default

`ComputerUseConfig` carries `enabled` (default `false`), plus the capture bound
and per-turn capture limit. Registration is skipped when disabled, matching
`config.web_search.enabled` and `config.lsp.enabled`. Unlike those, the default
is off: an unaware user upgrading ACECode should not gain a tool that can drive
their desktop.

`/computer` reports backend availability, screen geometry, grant state, and the
configured bounds, and revokes an active grant. It follows `/lsp` and
`/websearch` in sharing one dispatch function across the TUI command table, the
daemon builtin allowlist, and the web slash-command list.

### Record third-party attribution

The action vocabulary, the physical-to-logical capture handling, and the
resize-factor arithmetic derive from UI-TARS-desktop, which is Apache-2.0. The
ported arithmetic carries an attribution comment naming the upstream file, and
the repository NOTICE records the dependency. No upstream source is vendored
wholesale and no npm package is added; the runtime remains a self-contained C++
binary.

## Risks / Trade-offs

- [Grounding quality is a model property, not a tool property] → Pixel-accurate
  clicking is what UI-TARS's specialized models are trained for, and a general
  vision model will misclick more often. The tools are therefore additive and
  opt-in rather than promoted as a default workflow, and model selection stays
  with the existing `vision` capability tag so a user who configures a
  GUI-grounding model gets the better behavior without a code change.
- [Full-screen captures are expensive in tokens] → Bounded capture size and a
  per-turn limit keep a single turn's image cost predictable, at the cost of
  detail on large displays. Region capture would help further and is left for a
  follow-up once real usage shows which regions matter.
- [A session-scoped grant is coarser than per-call confirmation] → It covers a
  sequence the user cannot practically review click by click. The mitigations
  are that the grant is never persisted, is visible while active, is dropped by
  the existing abort control, and covers only action tools.
- [Coordinates can go stale between capture and action] → Captures record their
  geometry and actions are rejected when the screen configuration changed, so a
  monitor change produces an explicit error instead of a click at a wrong
  location. Content that moves without a geometry change is not detectable here
  and remains the model's responsibility to re-verify by capturing again.
- [Windows-only in this change] → Users on other platforms see no tools at all
  rather than a broken capability. The backend interface is the only thing the
  tools depend on, so a macOS or X11 body is additive; macOS additionally needs
  Accessibility and Screen Recording TCC grants from a signed bundle, which is
  the main reason it is scoped separately.
- [Automating a signed-in desktop can trip application anti-automation rules] →
  Synthetic input is indistinguishable from a script to the target application.
  The capability being opt-in and non-persistent keeps this a deliberate user
  choice rather than a default behavior.
