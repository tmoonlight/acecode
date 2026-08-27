## ADDED Requirements

### Requirement: Enabled frames render atomically via synchronized updates
When synchronized output is enabled, the TUI SHALL wrap every non-empty terminal frame write in a begin/end synchronized update pair (`CSI ?2026h` / `CSI ?2026l`) emitted in the same single write as the frame payload, so terminals supporting DEC mode 2026 present each frame atomically instead of exposing partially drawn intermediate states. The wrapper SHALL cover both TerminalOutput (scroll) and AltScreen render modes because both share the same frame flush path.

#### Scenario: Frame output is bracketed
- **WHEN** a frame is flushed while synchronized output is enabled
- **THEN** the byte stream begins with `CSI ?2026h` and ends with `CSI ?2026l` around the frame payload in one write

#### Scenario: Both render modes benefit
- **WHEN** the TUI runs in scroll mode or in AltScreen mode with synchronized output enabled
- **THEN** frames are bracketed identically because both modes flush through the same path

#### Scenario: Empty flushes are not bracketed
- **WHEN** a flush occurs with an empty output buffer
- **THEN** no synchronized update sequences are emitted

### Requirement: Synchronized output defaults off on unsupported or unknown terminals
The TUI SHALL enable synchronized output automatically only on terminals positively identified as supporting DEC mode 2026 via environment-variable heuristics. It SHALL stay disabled on ConEmu/Cmder (`ConEmuPID`), legacy or classic Windows Console Host, multiplexers (`TERM` starting with `tmux` or `screen`), and on any terminal that no rule recognizes.

#### Scenario: Known modern terminal enables the feature
- **WHEN** the TUI starts under Windows Terminal (`WT_SESSION` set), kitty, WezTerm, Ghostty, iTerm2, VS Code terminal, Terminal.app, foot, Contour, mintty, or Warp
- **THEN** synchronized output is enabled under the default `auto` mode

#### Scenario: Legacy Windows terminal stays unchanged
- **WHEN** the TUI starts under ConEmu/Cmder, legacy Windows console, or classic Windows Console Host
- **THEN** synchronized output stays disabled and the emitted byte stream is identical to a build without the feature

#### Scenario: Unknown terminal stays unchanged
- **WHEN** no environment rule identifies the terminal (for example plain `xterm-256color` or Alacritty)
- **THEN** synchronized output stays disabled under `auto`

#### Scenario: Multiplexer defaults off
- **WHEN** `TERM` starts with `tmux` or `screen`
- **THEN** synchronized output stays disabled under `auto`

#### Scenario: Blacklist wins over whitelist
- **WHEN** environment signals match both a whitelist and a blacklist entry
- **THEN** synchronized output stays disabled

### Requirement: Users can override the synchronized output decision
The config option `tui.sync_output_mode` SHALL accept `auto`, `always`, and `never`, default to `auto`, and normalize any other value to `auto` with a warning without blocking startup. `always` SHALL enable the wrapping regardless of detection, `never` SHALL disable it regardless of detection, and `auto` SHALL follow the terminal-support detection result.

#### Scenario: Force enable on an unknown terminal
- **WHEN** `tui.sync_output_mode` is `always` on a terminal no rule recognizes
- **THEN** frames are bracketed with synchronized update sequences

#### Scenario: Force disable on a supporting terminal
- **WHEN** `tui.sync_output_mode` is `never` under Windows Terminal
- **THEN** no synchronized update sequences are emitted

#### Scenario: Invalid value falls back to auto
- **WHEN** `tui.sync_output_mode` is set to an unrecognized string
- **THEN** the value is normalized to `auto`, a warning is logged, and startup continues

### Requirement: Disabled means byte-identical output
When synchronized output resolves to disabled, the TUI SHALL emit terminal output identical to a build without the feature, preserving the legacy Windows terminal compatibility behavior.

#### Scenario: Legacy terminal output is unchanged
- **WHEN** the TUI runs with synchronized output disabled
- **THEN** the terminal byte stream contains no `?2026` sequences and matches the pre-feature behavior
