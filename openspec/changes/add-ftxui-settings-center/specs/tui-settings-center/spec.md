## ADDED Requirements

### Requirement: In-session settings surface
The TUI SHALL open `/config` as a full-screen FTXUI surface inside the existing interactive screen loop and SHALL close back to the unchanged chat surface.

#### Scenario: Open and close settings
- **WHEN** an idle TUI user submits `/config`
- **THEN** the settings surface opens without starting a nested terminal loop, and `Esc` or the close control restores the chat surface and its prompt state

#### Scenario: Foreground turn blocks settings
- **WHEN** the foreground agent is generating or executing a tool and a settings-open request is attempted
- **THEN** the TUI keeps the chat surface active and explains that the turn must finish or be stopped first

### Requirement: English top-tab navigation
The settings surface SHALL use English static copy and one horizontal top tab rail containing General, Appearance, Configuration, Personalization, Models, Usage, Archived, and About, with no left navigation.

#### Scenario: Navigate tabs
- **WHEN** the user presses Tab or Shift+Tab outside an editor, clicks a tab, or invokes `/config <tab>`
- **THEN** the matching top tab becomes visibly active and its page receives focus

#### Scenario: Tabs exceed available width
- **WHEN** the terminal is narrower than the complete tab rail
- **THEN** the horizontal rail scrolls to keep the active tab visible and does not transform into a left-side menu

### Requirement: Settings search
The settings surface SHALL provide slash-triggered search with current-page and all-settings scopes.

#### Scenario: Search all settings
- **WHEN** the user focuses search, selects all-settings scope, and enters text
- **THEN** matching settings display with their owning tab and Enter jumps to the selected setting

#### Scenario: Escape search
- **WHEN** search contains text and the user presses Esc
- **THEN** the search is cleared before the settings surface is closed

### Requirement: Approved General and Appearance scope
General SHALL expose only the persisted default permission mode and native notification total switch. Appearance SHALL expose only the TUI theme values `auto`, `dark`, and `light`.

#### Scenario: Change the default permission
- **WHEN** the user selects Default, Accept Edits, Plan, or YOLO in General
- **THEN** the persisted global default for future TUI/Desktop sessions changes and the current session permission mode remains unchanged

#### Scenario: Toggle native notifications
- **WHEN** the user toggles Native notifications
- **THEN** the shared notification total switch is persisted and reflected by supported notification surfaces

#### Scenario: Change theme
- **WHEN** the user selects Auto, Dark, or Light in Appearance
- **THEN** the theme is persisted and the active TUI palette updates immediately

### Requirement: Configuration and personalization editors
Configuration SHALL edit the upgrade service base URL, and Personalization SHALL edit global custom instructions with explicit save and dirty-form protection.

#### Scenario: Save a valid upgrade URL
- **WHEN** the user enters a non-empty HTTP or HTTPS upgrade URL and saves
- **THEN** the normalized URL is persisted and the editor reports Saved

#### Scenario: Reject an invalid upgrade URL
- **WHEN** the user attempts to save a URL without an HTTP or HTTPS scheme
- **THEN** no configuration is changed and an actionable validation error is displayed beside the field

#### Scenario: Render compact scalar and filter inputs
- **WHEN** Configuration displays the upgrade URL field or Models displays its search filter
- **THEN** the editable field occupies one terminal row, uses a visible background, and shows an in-field placeholder when empty

#### Scenario: Leave dirty custom instructions
- **WHEN** custom instructions differ from the loaded value and the user changes tabs or presses Esc
- **THEN** a Save, Discard, or Cancel modal appears before navigation continues

### Requirement: Complete saved-model management
Models SHALL list, filter, add, edit, rename, delete, probe, and set the global default for saved model profiles, including OpenAI, Anthropic, and Copilot-specific fields and Copilot device authentication.

#### Scenario: Set global default model
- **WHEN** the user marks a saved model as default
- **THEN** the global default for future sessions changes and the active session model remains unchanged

#### Scenario: Add multiple probed models
- **WHEN** an OpenAI-compatible or Copilot probe returns models and the user selects multiple IDs
- **THEN** one validated saved profile per selected model is created using the entered provider settings

#### Scenario: Edit a profile
- **WHEN** the user edits a model name, provider, base URL, API key, headers, context window, capabilities, or model ID and saves
- **THEN** the shared saved-model editor validates and persists the profile and preserves default-name consistency on rename

#### Scenario: Delete model used by busy session
- **WHEN** a saved model is used by a busy active session
- **THEN** its destructive action is disabled and the UI explains that the active work must settle first

#### Scenario: Authenticate Copilot
- **WHEN** the user starts Copilot device authentication
- **THEN** the TUI displays the user code, offers copy/open actions, polls asynchronously, and updates the authenticated state without exposing tokens

### Requirement: Usage inspection
Usage SHALL show the latest 30-day token summary, daily trend, token categories, model breakdown, and workspace breakdown using FTXUI data components.

#### Scenario: Usage data available
- **WHEN** usage aggregation succeeds
- **THEN** the page shows totals, records, sessions, a Canvas trend, token categories, and selectable model/workspace details

#### Scenario: Usage load fails
- **WHEN** aggregation fails
- **THEN** the page retains navigation and displays a precise retryable error instead of stale or fabricated totals

### Requirement: Archived-session management
Archived SHALL enumerate archived sessions across registered workspaces and allow selection, restore, and permanently destructive single or batch actions.

#### Scenario: Restore archived sessions
- **WHEN** the user selects one or more archived sessions and activates restore
- **THEN** each selected session is unarchived, successful rows disappear, and per-item failures remain visible

#### Scenario: Permanently delete archived sessions
- **WHEN** the user requests permanent deletion
- **THEN** a modal names the session or selected count, states that deletion cannot be undone, and purges only after explicit confirmation

### Requirement: About information
About SHALL display the ACECode version, bundled FTXUI version, canonical config path, and project link without runtime status.

#### Scenario: Open About
- **WHEN** the user selects About
- **THEN** static build/config information is shown and no daemon, MCP, or tool health panel appears

### Requirement: Save state and modal priority
The surface SHALL distinguish immediate settings from explicit drafts and SHALL give blocking permission/question interactions priority.

#### Scenario: Immediate setting save succeeds
- **WHEN** an immediate setting is changed
- **THEN** the shell transitions through Saving to Saved and exposes the persisted value

#### Scenario: Immediate setting save fails
- **WHEN** persistence fails
- **THEN** the visible control rolls back to the confirmed value and the shell displays a non-secret error

#### Scenario: Blocking interaction arrives
- **WHEN** a permission request or AskUserQuestion becomes active while settings is visible
- **THEN** the blocking modal receives input first and the prior settings tab/focus is restored after it closes

### Requirement: Existing quick commands remain distinct
The change SHALL preserve `/model`, `/mode`, `/theme`, and `/models` behavior and SHALL offer `/config show` for the legacy text summary.

#### Scenario: Current-session model command
- **WHEN** the user invokes `/model`
- **THEN** the existing current-session picker opens rather than the Models settings tab

#### Scenario: Legacy config summary
- **WHEN** the user invokes `/config show`
- **THEN** the previous text configuration summary is appended to the transcript without opening settings
