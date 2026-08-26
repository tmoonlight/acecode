## 1. FTXUI fork patch

- [x] 1.1 Add `App::EnableSynchronizedOutput(bool)` declaration to `external/ftxui/include/ftxui/component/app.hpp` (opt-in option, must be called before `Loop()`, `ACECODE-PATCH(synchronized-output)` marker).
- [x] 1.2 Implement the option flag in `App::Internal` and bracket every non-empty `App::TerminalFlush()` buffer with `CSI ?2026h` / `CSI ?2026l` in `external/ftxui/src/ftxui/component/app.cpp`.
- [x] 1.3 Document the patch set in `external/ftxui/ACECODE_PATCHES.md` (referenced by the port description).
- [x] 1.4 Bump `port-version` in `ports/ftxui/vcpkg.json` so vcpkg rebuilds the fork.

## 2. Terminal support detection

- [x] 2.1 Add `detect_synchronized_output_support_with()` (env-injectable pure helper) and the real-env `detect_synchronized_output_support()` wrapper to `src/utils/terminal_capability.hpp/.cpp`, implementing the blacklist > whitelist > unknown-off rules.
- [x] 2.2 Add unit tests covering every whitelist entry, every blacklist entry (ConEmu, legacy/classic conhost, tmux, screen), unknown-terminal default-off, and priority of blacklist over whitelist.

## 3. Configuration and wiring

- [x] 3.1 Add `TuiConfig::sync_output_mode` (`auto` default) with load-time validation/normalization and serialization in `src/config/config.hpp/.cpp`, mirroring `alt_screen_mode`.
- [x] 3.2 Add header-only `decide_synchronized_output(const TuiConfig&, bool)` policy to `src/tui/render_mode.hpp` with unit tests for `always` / `never` / `auto`.
- [x] 3.3 Call `screen.EnableSynchronizedOutput(decide_synchronized_output(...))` before `App::Loop()` in `src/main.cpp`.
- [x] 3.4 Document `tui.sync_output_mode` in `README.md` and `README_CN.md` config tables.

## 4. Verification

- [x] 4.1 Rebuild ftxui + `acecode` + `acecode_unit_tests`; run the unit suite and the new focused tests.
- [x] 4.2 Run `openspec validate add-synchronized-output --strict`.
- [ ] 4.3 Manual smoke on the dev machine: default config in a modern terminal (no flicker, no rendering regression), `sync_output_mode: "never"` produces pre-change behavior, `always` wraps output even on an unknown terminal.
- [x] 4.4 Record the real-device terminal matrix still requiring user verification (legacy conhost/ConEmu, Windows Terminal, tmux/screen).

## Verification

- `cmake --preset macos-x64-debug` — passed; vcpkg rebuilt `ftxui:x64-osx@6.1.9#8` (fork patch included).
- `cmake --build build/macos-x64-debug --target acecode_unit_tests -j 8` — passed (252/253 objects, linked).
- `acecode_unit_tests --gtest_filter='TerminalCapability*:SynchronizedOutputSupport*:RenderModeDecide*:SynchronizedOutputDecide*:Config*'` — 219/219 passed.
- New suites: `SynchronizedOutputSupport` 11 tests, `SynchronizedOutputDecide` 4 tests — all passed (one whitelist fix: ghostty TERM is `xterm-ghostty`, matched by substring).
- Full suite `acecode_unit_tests`: 3308 passed, 14 failed — all pre-existing environment failures (network/PTY/browser-tool sandbox gaps: TcpProbe, Pty*, ManagedRemoteWebProxy, WebServerHttp, BuiltinToolRegistry, SkillsToolTest); `SettingsCenterRender.NarrowTopRailFollowsDeepLinkedTab` confirmed failing on the stashed baseline too (unrelated to this change). `RemoteWebTcpProxy` aborts in this sandbox (pre-existing).
- `cmake --build build/macos-x64-debug --target acecode` — passed; `./acecode --help` runs (exit 0).
- `openspec validate add-synchronized-output --strict` — passed.
- `git diff --check` — passed.

### Real-device verification matrix (user, manual)

| Terminal | Expectation |
|---|---|
| Windows Terminal | 2026 on (WT_SESSION whitelist); long stream + resize: no flicker |
| Legacy conhost / classic conhost / Cmder·ConEmu | 2026 off; byte-identical output to pre-feature (no `?2026` in stream, no raw-ESC garbage) |
| kitty / WezTerm / Ghostty / iTerm2 / VS Code terminal / Terminal.app / foot / mintty | 2026 on; no flicker |
| tmux / screen | 2026 off under `auto`; force-on test with `tui.sync_output_mode: "always"` |
| Unknown terminal (Alacritty, bare xterm) | 2026 off under `auto`; `always` sends sequences, terminal ignores harmlessly |
