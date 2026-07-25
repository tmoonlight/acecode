## 1. Shared Settings Mutations

- [x] 1.1 Add a cross-platform config transaction lock, reload-before-patch API, atomic canonical writer, and temporary-home concurrency/rollback tests
- [x] 1.2 Add typed scalar mutations for default permission, notifications, TUI theme, upgrade URL, and custom instructions with validation and secret-safe results
- [x] 1.3 Add typed saved-model/default-model mutations that reuse `saved_models_editor` and preserve busy-session deletion guards
- [x] 1.4 Migrate daemon/Desktop routes for the settings exposed by `/config` to the shared mutation helpers and keep in-memory snapshots synchronized

## 2. TUI Surface Framework

- [x] 2.1 Add pure Settings/Management tab identifiers, parsing, search, footer-action, dirty-state, and process-lifetime navigation models with unit tests
- [x] 2.2 Build the English FTXUI shell with animated horizontal tabs, search band, scrollable content, contextual footer, save state, and modal stack
- [x] 2.3 Mount Chat, Settings, and Capability Management as sibling roots in the existing `ScreenInteractive` and add typed open/close callbacks to `CommandContext`
- [x] 2.4 Change `/config` routing to open Settings, add deep links and `/config show`, preserve `/model`, `/mode`, `/theme`, and `/models`, and add routing tests

## 3. Settings Pages

- [x] 3.1 Implement General with immediate persisted default-permission and native-notification controls, including live default-template refresh
- [x] 3.2 Implement Appearance with Auto/Dark/Light preview cards, immediate persistence, and live palette switching
- [x] 3.3 Implement Configuration upgrade-URL editing and Personalization multiline custom-instructions editing with explicit save and dirty navigation guards
- [x] 3.4 Implement Models list/filter/default actions and model detail presentation with busy-session action availability
- [x] 3.5 Implement the model add/edit FTXUI window, provider-specific fields, secret masking, headers JSON, context window, capabilities, validation, and multi-model creation
- [x] 3.6 Implement asynchronous model probing and Copilot device authentication windows with stale-result protection
- [x] 3.7 Implement Usage summary, Canvas trend, token categories, model/workspace breakdowns, loading/error/empty states, and refresh
- [x] 3.8 Implement all-workspace Archived list, filtering, multi-select restore, permanent purge confirmation, partial failure reporting, and refresh
- [x] 3.9 Implement About with ACECode/FTXUI versions, config path, project link, and copy/open actions

## 4. Capability Management Center

- [x] 4.1 Build the shared five-tab management shell and change no-argument `/skills` and `/mcp` plus new `/connectors`, `/tools`, and `/hooks` routing while preserving parameterized commands
- [x] 4.2 Implement Skills listing, scope/status metadata, filtering, details, name-based enable/disable, reload, and directory actions
- [x] 4.3 Implement MCP server listing, state/details, enable/disable/reconnect/reload, and validated raw JSON editor
- [x] 4.4 Implement Connectors listing, lifecycle status, enable/disable, refresh, and authentication-recovery presentation
- [x] 4.5 Implement Tools source/status listing and Browser Bridge persisted/live toggle without presenting false controls for immutable tools
- [x] 4.6 Implement Hooks listing, details, diagnostics, trust confirmation, enable/disable, and refresh

## 5. Verification and Documentation

- [x] 5.1 Add fixed-size FTXUI render tests for all Settings tabs, management tabs, English copy, top-rail scrolling, search, selection, save state, and modals
- [x] 5.2 Add focused command, mutation, model, usage, archive, manager, and stale-async-result tests
- [x] 5.3 Update command help and daemon API/config documentation for UI entry points and shared mutation semantics
- [x] 5.4 Build `acecode_unit_tests` and `acecode`, run focused and full CTest suites, run code-quality and whitespace checks, and manually exercise every tab in dark and light themes
- [x] 5.5 Render the Configuration URL and Models filter as one-row background-filled inputs with placeholders and add fixed-size regression coverage
- [x] 5.6 Render Archived and all five capability-management filters as one-row background-filled inputs and add fixed-size render coverage
- [x] 5.7 Replace the Models Add/Edit entries with one external `Edit...` config-file handoff, update shortcuts/footer state, and add callback-based regression coverage
- [x] 5.8 Build the TUI and unit tests, run focused settings tests, validate OpenSpec, and check the final diff
- [x] 5.9 Add a responsive token/date coordinate system to the Usage daily-trend Canvas and fixed-size render coverage
- [x] 5.10 Build the TUI and unit tests, run focused settings tests, validate OpenSpec, and check the final diff
