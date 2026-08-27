## Why

ACECode redraws each TUI frame with a full repaint plus cursor-up redraw. Terminals render the frame while it is still being written, so users see half-drawn intermediate states (top half new, bottom half old) — visible flicker that the TUI-comparison report (`docs/tui-comparison/report.md`, gap #2) ranked as the highest-ROI quick win (MICE score 8.1). Every competitor that adopted synchronized output (CSI `?2026h` / `?2026l`, DEC mode 2026) eliminated this class of flicker: the terminal buffers everything between the two sequences and paints it as one atomic frame.

## What Changes

- Patch the vendored FTXUI fork (`external/ftxui`) so that, when enabled, every terminal flush is bracketed by `CSI ?2026h` (begin synchronized update) and `CSI ?2026l` (end synchronized update), mirroring the existing `ACECODE-PATCH` conventions and the `TrackMouse`-style opt-in API (`App::EnableSynchronizedOutput(bool)`).
- Gate the feature with an environment-variable heuristic that extends the existing `TerminalCapabilities` detection: known-good modern terminals (Windows Terminal, kitty, WezTerm, Ghostty, iTerm2, VS Code terminal, foot, Terminal.app) get it; legacy/classic conhost, ConEmu/Cmder, tmux/screen, and unrecognized terminals do not.
- Add a `tui.sync_output_mode` config option (`auto` / `always` / `never`, default `auto`) parsed and normalized exactly like the existing `tui.alt_screen_mode`.
- Apply the wrapping in both render modes (TerminalOutput scroll mode and AltScreen) — they share the single `App::TerminalFlush()` output choke point, so both are covered by one change.
- Keep output byte-identical to today when the feature resolves to disabled.

## Capabilities

### New Capabilities

- `synchronized-output`: Atomic frame presentation via DEC mode 2026 with terminal-support gating, config override, and unchanged-by-default legacy behavior.

### Modified Capabilities

None. Terminal capability detection (`terminal_capability`) gains a helper but its existing struct, signatures, and behavior are unchanged.

## Impact

- `external/ftxui`: `include/ftxui/component/app.hpp`, `src/ftxui/component/app.cpp` (new `EnableSynchronizedOutput` option + `TerminalFlush` bracketing), and a new `ACECODE_PATCHES.md` patch inventory; `ports/ftxui/vcpkg.json` port-version bump to force a vcpkg rebuild.
- `src/utils/terminal_capability.hpp/.cpp`: new pure, env-injectable `detect_synchronized_output_support_with()` plus a real-env wrapper.
- `src/tui/render_mode.hpp`: new header-only `decide_synchronized_output()` policy function following `decide_render_mode()`.
- `src/config/config.hpp/.cpp`: new `TuiConfig::sync_output_mode` field with load/normalize/serialize.
- `src/main.cpp`: call `screen.EnableSynchronizedOutput(...)` before `App::Loop`.
- `tests/utils/terminal_capability_test.cpp`, `tests/tui/render_mode_test.cpp`: unit coverage for detection and policy.
- `README.md` / `README_CN.md`: config table entries.
- No protocol, daemon, or web changes.
