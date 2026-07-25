## Why

ACECode's TUI exposes configuration through scattered slash commands while `/config` only prints a short summary, so terminal users cannot manage the settings and integrations already available from Desktop. A native FTXUI settings experience is needed now to make the terminal a first-class configuration surface without changing the established quick commands for active sessions.

## What Changes

- Replace no-argument `/config` with an in-session, full-screen FTXUI settings center using one top tab rail: General, Appearance, Configuration, Personalization, Models, Usage, Archived, and About.
- Keep the first TUI settings experience English-only. General contains the default permission mode and native notification toggle; Appearance contains only the TUI theme; the settings center does not add locale selection, runtime status, feedback, or inactive Desktop placeholders.
- Provide complete terminal workflows for upgrade-service configuration, custom instructions, saved-model/default-model management, Copilot authentication, usage inspection, archived-session restore/purge, and version/config information.
- Add a separate Claude Code-style capability management center with Skills, MCP Servers, Connectors, Tools, and Hooks top tabs. No-argument `/skills` and `/mcp` open their tabs; new `/connectors`, `/tools`, and `/hooks` entries open the corresponding tabs while existing parameterized commands remain available.
- Preserve `/model` as the current-session model picker, `/mode` as the current-session permission control, `/theme` as the quick TUI theme command, and `/models` as the models.dev registry command.
- Route TUI settings and management actions through reusable typed operations with validation, reload-before-patch persistence, atomic replacement, secret redaction, and runtime apply hooks so TUI and Desktop do not maintain conflicting mutation rules.
- Add pure state/presentation tests, configuration/domain-operation tests, FTXUI render tests, and focused command-routing coverage.

## Capabilities

### New Capabilities

- `tui-settings-center`: Full-screen FTXUI `/config` navigation, settings pages, editors, search, save semantics, and active-session/default boundaries.
- `tui-capability-management-center`: Shared Skills/MCP/Connectors/Tools/Hooks management surface and slash-command tab routing.
- `shared-settings-mutations`: Reusable, concurrency-safe settings operations shared by terminal and daemon/Desktop paths.

### Modified Capabilities

None.

## Impact

- **TUI:** `src/main.cpp`, `src/tui_state.hpp`, new focused components under `src/tui/`, theme/presentation helpers, keyboard and modal routing.
- **Commands:** `/config`, `/skills`, `/mcp`, plus new `/connectors`, `/tools`, and `/hooks` no-argument UI entry points; existing argument-taking behavior remains compatible.
- **Configuration/domain logic:** `src/config/`, saved-model helpers, notification/theme runtime application, MCP/skills/connectors/hooks/tool managers, and archived/usage query seams.
- **Daemon/Desktop:** Existing routes reuse shared typed mutations where they edit the same fields; no Desktop layout redesign is included.
- **Tests/docs:** TUI, command, config, model, archive, usage, and daemon API documentation where shared endpoint behavior changes.
- **Dependencies:** Uses the repository's existing FTXUI 6.1.9 fork; no new third-party dependency is introduced.
