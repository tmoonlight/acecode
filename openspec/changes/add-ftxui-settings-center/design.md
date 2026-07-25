## Context

The terminal application already runs one long-lived FTXUI `ScreenInteractive` loop. Its chat surface is a `Renderer` around a focusable input component, with a large `CatchEvent` layer for prompt, picker, permission, question, scrolling, and mouse behavior. Existing `/model` and `/mode` pickers are intentionally small inline overlays; `/config`, `/skills`, and `/mcp` currently emit text into the transcript.

Desktop exposes a much broader settings surface backed by a mixture of authenticated daemon routes, shared C++ helpers, runtime managers, and a few frontend-only placeholders. This change must reuse the real domain seams, exclude placeholders, preserve quick active-session commands, and avoid creating a nested FTXUI loop.

The repository bundles a patched FTXUI 6.1.9 with native menus, inputs, radioboxes, checkboxes, sliders, tabs, modal stacking, resizable windows, tables, canvas drawing, scrolling frames, animation, mouse handling, and custom render transforms. The user explicitly chose modern rendering without a legacy-terminal fallback.

The visual subject is a professional terminal control plane for an AI coding agent. Its single job is fast, keyboard-first inspection and mutation of settings without leaving the active process. The design language follows the supplied Claude Code management screen: one restrained border, one horizontal tab rail, one search band, a dense content viewport, and a contextual command footer. The signature element is the continuous top rail whose active tab, search scope, list selection, and footer all share the current ACECode accent.

## Goals / Non-Goals

**Goals:**

- Make `/config` a complete English-only, full-screen FTXUI settings surface with the eight approved top tabs.
- Add a separate five-tab capability management center opened directly by the corresponding slash commands.
- Keep all UI inside the existing `ScreenInteractive` and allow native FTXUI components to own focus, forms, scrolling, animation, and modal interaction.
- Preserve the semantic boundary between global/new-session defaults and current-session controls.
- Reuse one typed mutation and validation path across TUI and daemon/Desktop callers for fields exposed on both surfaces.
- Keep async loads and runtime actions responsive and stale-result safe.
- Make state and presentation logic independently testable without running an interactive terminal.

**Non-Goals:**

- Localizing the TUI or exposing the Desktop/Web `ui.locale` preference.
- Rendering runtime status in General, adding feedback, or copying inactive Desktop placeholders.
- Moving Skills, MCP, Connectors, Tools, or Hooks into `/config`.
- Changing `/model`, `/mode`, `/theme`, `/models`, or parameterized `/skills` and `/mcp` semantics.
- Redesigning Desktop settings layout.
- Supporting legacy terminal rendering or an alternate ASCII-only design.

## Decisions

### 1. Mount settings as sibling FTXUI roots

The top-level TUI component becomes a `Container::Tab` with Chat, Settings, and Capability Management children. A small surface controller owns the active root and requested tab. Slash commands call a typed `open_tui_surface(surface, tab)` callback supplied through `CommandContext`.

This keeps one screen loop, preserves terminal installation/cleanup, and lets FTXUI transfer focus to the selected root. Starting a second `ScreenInteractive::Loop()` was rejected because nested terminal mode, mouse tracking, selection, and exit callbacks would conflict with the active chat loop.

Blocking permission and AskUserQuestion overlays remain higher priority. Settings cannot open while the foreground agent turn or tool execution is active. Background subagent records do not block it. If a blocking overlay is raised while a management surface is visible, the overlay temporarily owns input and the previous surface/focus is restored afterward.

### 2. Build focused controllers rather than extending `main.cpp`

New files under `src/tui/settings/` own:

- stable page/tab identifiers and slash-command routing;
- draft, selection, filter, scroll, dirty, loading, error, and modal state;
- pure filtering, validation-message, footer, and row-presentation helpers;
- FTXUI component factories and renderers;
- adapters for settings, models, usage, archives, and capability managers.

`main.cpp` only constructs dependencies, mounts the two roots, and supplies open/close/post-event callbacks. The existing large input handler short-circuits to the active non-chat component before interpreting chat shortcuts.

### 3. Use one deliberate FTXUI visual system

The settings shell uses:

- `MenuOption::HorizontalAnimated()` plus `xframe` for the top rail;
- `Input` for search and text fields;
- `Menu`, `Radiobox`, `Checkbox`, `Toggle`, and `Slider` for native focus and selection;
- `yframe`, `vscroll_indicator`, `Table`, `Canvas`, `gauge`, and `spinner` for data-heavy pages;
- `Modal` plus `Window` for model editing, Copilot login, dirty-form prompts, and destructive confirmation;
- `Renderer` transforms that use only the existing `ThemePalette` semantic colors.

Dark and light palettes remain authoritative. No new hard-coded product palette is introduced. The top rail and selected rows use `ui.accent`, `ui.selection_bg`, and `ui.selection_fg`; success/warning/error states use semantic colors. Animation is concentrated on tab and selection movement instead of scattered decoration.

Single-line scalar and filter inputs use a compact one-row transform with a
palette-backed background and in-field placeholder. Multiline height is
reserved for custom instructions and structured editors such as raw JSON.

The UI copy is English. Stable IDs never depend on labels, and copy is centralized per surface so a future whole-TUI localization change does not require rewriting controllers.

### 4. Keep approved page scope exact

The Settings tabs are:

1. General: default permission mode and native notifications.
2. Appearance: TUI theme (`auto`, `dark`, `light`) only.
3. Configuration: upgrade service base URL only.
4. Personalization: global custom instructions.
5. Models: saved profiles, global default, model probing, and Copilot auth.
6. Usage: 30-day summary, trend, token categories, models, and workspaces.
7. Archived: all-workspace archived sessions, multi-select restore, and permanent purge.
8. About: ACECode version, FTXUI version, config path, and project link.

The Capability Management tabs are Skills, MCP Servers, Connectors, Tools, and Hooks. Models are deliberately absent.

### 5. Separate immediate settings from explicit drafts

Radioboxes, checkboxes, theme selection, default permission, notifications, and setting the default model persist immediately and expose Saving/Saved/Failed state.

Upgrade URL, custom instructions, and model forms use explicit Save/`Ctrl+S`. `Esc` from a dirty editor opens Save/Discard/Cancel. Switching a top tab while dirty follows the same gate. Destructive model/session actions always use an immediate FTXUI modal with the exact target name or count.

Settings remembers the last tab, selection, filter, and scroll position for the current process. A fresh process opens General. Explicit deep links such as `/config models` override remembered state.

### 6. Preserve current-session command boundaries

`/config` edits persisted global values. Default permission and default model affect future TUI/Desktop sessions; they do not silently mutate the active session. `/mode` and `/model` remain the active-session controls, and `/theme` remains the quick theme shortcut.

No-argument `/skills` and `/mcp` open the capability center. Their existing argument-taking subcommands continue through the old command handlers. `/connectors`, `/tools`, and `/hooks` are new UI-only entries. `/models` remains the models.dev registry command.

### 7. Introduce typed, reload-before-patch config transactions

A shared settings mutation service accepts a narrow typed operation rather than a replacement `AppConfig`. It:

1. acquires an in-process mutex and an interprocess lock beside `config.json`;
2. reloads the latest on-disk config while the lock is held;
3. validates and applies one typed mutation;
4. serializes through the canonical config builder;
5. writes with sibling-temp plus atomic replace;
6. publishes the resulting field snapshot to the caller;
7. releases the lock before invoking surface-specific runtime apply hooks.

The service returns structured success/error results and never includes API keys, tokens, custom headers, or connector secrets in diagnostics. Existing daemon routes for the same fields migrate to these operations; unrelated config routes are not redesigned.

Atomic replacement uses the existing cross-platform atomic-file helper. The lock implementation uses `LockFileEx` on Windows and `flock` on POSIX. A unique lock file remains present but unlocked between transactions.

### 8. Keep runtime application explicit

Persisted mutation and runtime application are separate steps with rollback-safe reporting:

- theme swaps the active palette after persistence;
- notification and default-permission snapshots update the current process's default/template state without changing the active session;
- saved-model/default-model mutations refresh the TUI model list and model-pool watcher;
- skill reload, MCP enable/disable/reconnect, connector enablement, Browser Bridge toggle, and hook enable/trust operations call their existing runtime managers;
- failed runtime application leaves the persisted state visible with a precise “saved; restart required” or failure message rather than pretending the mutation was live.

### 9. Run async data work off the FTXUI thread

Usage aggregation, archive enumeration/actions, model probes, Copilot polling, skill reload, MCP reconnect, connector refresh, and hook diagnostics run on worker tasks. Each request captures a monotonically increasing generation. Completion posts back through `ScreenInteractive::Post`; the controller ignores results from an older generation or a closed surface.

Controllers are owned for the full TUI loop and stop/join their workers before dependent managers are destroyed.

### 10. Test the state model and rendered contract

Pure tests cover tab parsing, command routing, filtering, dirty transitions, footer actions, permission/default boundaries, model action availability, archive selection, and stale-result suppression.

FTXUI render tests create fixed-size `Screen` instances and assert the top rail, active tab, English copy, selection, search, modal, and narrow horizontal-scroll behavior. Domain tests use temporary config homes to verify reload-before-patch, atomic persistence, validation rollback, secret redaction, and concurrent non-overlapping mutations.

## Risks / Trade-offs

- **[The feature surface is broad]** → Keep controllers and adapters page-scoped, land tasks in vertical slices, and mark each OpenSpec task immediately after its focused tests pass.
- **[The current TUI input handler is large]** → Add one early active-surface dispatch instead of duplicating settings event handling inside the chat handler.
- **[Cross-process writers outside the new service can still race]** → Migrate every existing route that mutates an exposed Settings field and make canonical `save_config` atomic; document remaining legacy callers for follow-up.
- **[FTXUI native inputs do not provide syntax-aware JSON editing]** → Use multiline input with validation, formatting, and an error line; do not create a second custom text editor in this change.
- **[A long model list or usage dataset can make rendering expensive]** → Filter before rendering, virtualize visible rows, cache normalized usage data, and redraw only after posted state changes.
- **[Modal focus can leak back to chat]** → Keep modals inside the active settings component tree and test focus restoration across close/cancel paths.
- **[Runtime state may diverge after a successful save but failed live apply]** → Show the exact state and restart requirement; never roll disk back after an external runtime side effect has partially occurred.

## Migration Plan

1. Add shared mutation primitives and tests without changing visible behavior.
2. Add the Settings/Management controllers and mount them behind inactive roots.
3. Change slash-command no-argument routing and keep parameterized fallbacks.
4. Migrate daemon routes for the exposed fields to shared mutations.
5. Build and run focused/full tests, then manually exercise every tab, modal, theme, and command entry.

Rollback is a source revert. No config schema migration is introduced; older binaries ignore no new fields because the feature edits existing configuration structures.

## Open Questions

None. The user approved English-only UI, top-only navigation, the eight Settings tabs, the separate five-tab capability center, modern FTXUI rendering, mixed immediate/explicit save behavior, and preservation of active-session commands.
