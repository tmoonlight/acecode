## 1. Configuration and Gating

- [ ] 1.1 Add `ComputerUseConfig` (`enabled` default false, max capture edge, per-turn capture limit) to `AppConfig` with sparse serialization.
- [ ] 1.2 Add `register_computer_use_tools` to the built-in registration chain, skipped when disabled and compiled out where no backend exists.
- [ ] 1.3 Add config schema notes for the `computer_use` section.

## 2. Backend Contract and Coordinates

- [ ] 2.1 Declare `ComputerBackend` (`capture`, `pointer_move`, `pointer_button`, `pointer_scroll`, `key_event`, `type_text`, geometry query) and `ScreenGeometry`.
- [ ] 2.2 Add the pure `computer_coords` unit: captured-image pixels to logical desktop coordinates, capture downscale factors, virtual-desktop origin, and bounds checks.
- [ ] 2.3 Port the resize-factor arithmetic from UI-TARS `action-parser` with an attribution comment naming the upstream file.
- [ ] 2.4 Add the pure capture-budget unit for maximum edge clamping and per-turn capture counting.
- [ ] 2.5 Add unit tests for coordinate translation, scaled and multi-monitor geometry, staleness detection, out-of-bounds rejection, and budget clamping.

## 3. Windows Backend

- [ ] 3.1 Implement desktop capture and PNG encoding in `computer_backend_win.cpp`, downsampling physical to logical pixels.
- [ ] 3.2 Implement pointer move, button, scroll, and drag through `SendInput`.
- [ ] 3.3 Implement the key-name map, chords, explicit hold/release, and literal text entry, releasing held keys on teardown or abort.
- [ ] 3.4 Add the Windows link dependencies and confirm the capability is absent from non-Windows builds.

## 4. Tool Surface

- [ ] 4.1 Add `computer_screenshot` as read-only, returning a PNG through `ToolResult::attachments` and reporting image dimensions and capture geometry.
- [ ] 4.2 Add `computer_click`, `computer_move`, `computer_drag`, `computer_scroll`, `computer_key`, and `computer_type` with JSON schemas, abort polling, and structured results.
- [ ] 4.3 Reject coordinate-bearing actions with no prior capture, with stale geometry, or with out-of-bounds coordinates before dispatching input.
- [ ] 4.4 Add tool summaries and compact call previews so TUI and web tool rows render usefully.

## 5. Control Grant and Safety

- [ ] 5.1 Add session-scoped grant state with a single capability-level permission decision on the first action tool call.
- [ ] 5.2 Drop the grant on session end, on user abort, and on explicit revocation; never persist it.
- [ ] 5.3 Surface an active grant in the session status surfaces so an operating session is visible.
- [ ] 5.4 Add unit tests for grant acquisition, reuse, denial, abort-drop, and non-persistence.

## 6. Command Surface

- [ ] 6.1 Add a shared `/computer` dispatch reporting availability, geometry, grant state, and bounds, with an `off` subcommand.
- [ ] 6.2 Register `/computer` in the TUI command table, the daemon builtin allowlist, and the web slash-command list.

## 7. Verification and Documentation

- [ ] 7.1 Add a gated manual smoke path for the native backend, following the notification smoke-test precedent.
- [ ] 7.2 Document the capability, its default-off posture, the coordinate contract, and the grant model in `README.md`, `README_CN.md`, and the repository capability notes.
- [ ] 7.3 Record the UI-TARS-desktop Apache-2.0 attribution in NOTICE.
- [ ] 7.4 Build the relevant targets, run focused tests, manually verify capture and a click sequence on Windows, and strictly validate the OpenSpec change.
