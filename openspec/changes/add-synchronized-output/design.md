## Context

- Every FTXUI frame funnels through a single choke point: sequences accumulate in `App::Internal::output_buffer` via `TerminalSend()` and leave the process in `App::TerminalFlush()` (`external/ftxui/src/ftxui/component/app.cpp`), which does one `std::cout << buffer << std::flush`. Without synchronized output the terminal paints this stream incrementally, exposing half-drawn frames.
- The vendored FTXUI fork (`external/ftxui`, pinned at d347ccf3 by the parent repo, built through the `ports/ftxui` overlay port) already carries ACECode patches (`ACECODE-PATCH(conhost)`, `drag-autoscroll`, `mouse-origin`, full-repaint env var), so patching the fork is established practice. The fork's `main` branch additionally carries a `TrackMouse`-style `EnableKittyKeyboard(bool)` opt-in API that this change mirrors (the parent pin predates it).
- `src/utils/terminal_capability.hpp` already provides `TerminalCapabilities` detection (ConEmu, Windows Terminal, legacy/classic conhost) with an env/version-injectable pure helper for unit tests.
- DEC mode 2026 has no query protocol that we can rely on across our terminal matrix; per the vt-extensions spec, DECRQM (`CSI ? 2026 $ p`) exists but reading replies requires stdin round-trips with timeouts at startup (the DA1-family approach deferred to the separate terminal-probe initiative, report gap #5).

## Goals / Non-Goals

**Goals:**

- Eliminate half-frame flicker on terminals that support DEC mode 2026, in both render modes, with a minimal fork patch.
- Keep legacy Windows terminals (our compatibility moat) byte-for-byte unchanged.
- Make the enable decision a pure, unit-testable function of config + environment.
- Force a deterministic vcpkg rebuild of the fork after the patch.

**Non-Goals:**

- Fixing the resize sequence-mixing corruption noted in `app.cpp` (separate task).
- DA1/DECRQM query-based detection (report gap #5 territory).
- Incremental layout or scroll-mode overhaul (report gap #1 / #8).
- Detecting Alacritty (no stable env marker) — it stays "unknown" and off unless forced.

## Decisions

### 1. Bracket frames inside `App::TerminalFlush()`

`TerminalFlush()` is the only path frame bytes take to the terminal. When enabled it prepends `\033[?2026h` and appends `\033[?2026l` to the accumulated buffer before the single `std::cout` write. Both sequences go out in one write, so the bracket can never be split mid-frame by the app; if the process dies anyway, terminals auto-release the mode after their own timeout (~150 ms). Empty buffers are not bracketed.

### 2. Opt-in public API instead of an env var

`App::EnableSynchronizedOutput(bool enable)` set before `App::Loop()`, mirroring `TrackMouse` and the fork's `EnableKittyKeyboard` patch style. The alternative (an `ACECODE_FTXUI_*` env var, like the full-repaint patch) would work but hides a rendering-critical decision in stringly state; the API keeps the call site in `src/main.cpp` explicit and greppable.

### 3. Environment-variable whitelist for support detection

New pure helper `detect_synchronized_output_support_with(caps, env_lookup)` in `terminal_capability`:

- **Blacklist (off, highest priority):** `ConEmuPID` set (ConEmu/Cmder), `caps.is_legacy_conhost` or `caps.is_classic_conhost`, `TERM` starting with `tmux` or `screen` (multiplexer version unknowable from env; force-able via config).
- **Whitelist (on):** `WT_SESSION` set (Windows Terminal), `KITTY_WINDOW_ID` set or `TERM == "xterm-kitty"` (kitty), `TERM_PROGRAM` in `iTerm.app` / `WezTerm` / `ghostty` / `vscode` / `Apple_Terminal` / `WarpTerminal` / `contour` / `mintty`, `TERM` starting with `foot` or `ghostty`.
- **Unknown (off):** anything else, including POSIX terminals with no recognizable markers (e.g. Alacritty, plain `xterm-256color`).

On non-Windows builds the Windows-only signals simply never fire and the POSIX rules apply. Terminals that ignore unknown DEC modes harmlessly would tolerate unconditional sending, but the conservative default protects the legacy-Windows moat and surprises no one; `sync_output_mode: "always"` overrides for adventurous users.

### 4. Policy function mirrors `alt_screen_mode`

`decide_synchronized_output(const TuiConfig&, bool terminal_supported)` in `src/tui/render_mode.hpp`: `"always"` → on, `"never"` → off, `"auto"` (default) → follow detection. Invalid config values normalize to `"auto"` with a warning, exactly like `alt_screen_mode`.

### 5. Port-version bump for rebuild

vcpkg hashes port files, not the `SOURCE_PATH` working tree, so `ports/ftxui/vcpkg.json` gets a `port-version` bump to guarantee the patched fork is rebuilt and reinstalled.

## Risks / Trade-offs

- **Stale `vcpkg_installed` headers** (built from a newer fork checkout with the kitty patch) will be replaced by the rebuild; the parent repo stays pinned at d347ccf3 + this patch until the fork commit is promoted.
- **tmux 3.3+** actually handles mode 2026 correctly, but defaulting off there is the safe choice until real-device verification; users can force it on.
- **macOS Terminal.app** support varies by OS version; on older versions the sequences are ignored harmlessly (no benefit, no damage).
- One extra `std::string::insert` per flush is negligible next to the existing full-frame serialization.
