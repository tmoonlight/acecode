## ADDED Requirements

### Requirement: Expert discovery and conversation controls remain separate surfaces
The system SHALL provide an expert-components management page and SHALL reuse the existing real conversation composer for expert dispatch. The expert-components page MUST NOT render a simulated, floating, fixed, or duplicated chat composer or conversation status bar.

#### Scenario: Open the expert-components page
- **WHEN** a user navigates to the expert-components page
- **THEN** the page shows expert discovery and management content without any chat input at its bottom

#### Scenario: Open expert controls in a conversation
- **WHEN** a user opens the plus menu on any conversation composer
- **THEN** the system shows the expert entry on that existing composer rather than creating a second composer

### Requirement: Expert and team catalog follows the imported information architecture
The system SHALL expose primary tabs named `专家` and `专家团`. Expert filtering SHALL use non-exclusive Tags, so one expert can appear under every Tag assigned to it. Search SHALL match at least expert name, author, introduction, Tag, and expertise text, and the selected Tag and search term SHALL combine as an intersection.

#### Scenario: Expert has multiple Tags
- **WHEN** an expert has both `OPC-一人公司` and `开发` Tags
- **THEN** the same expert appears when either Tag is selected

#### Scenario: Search within a Tag
- **WHEN** a user selects a Tag and enters a search term
- **THEN** only experts matching both constraints are displayed

#### Scenario: Switch to expert teams
- **WHEN** a user selects the `专家团` primary tab
- **THEN** the page displays team cards and team-appropriate actions without expert-only Tag semantics being misrepresented as categories

### Requirement: Expert cards distinguish expertise from opening prompts
Every expert card SHALL present identity, author/source information, a concise introduction, and expertise items. A card MUST NOT use opening prompts as expertise labels. Card actions SHALL use the Chinese label `派遣`.

#### Scenario: Render an expert card
- **WHEN** an expert contains both expertise items and opening prompts
- **THEN** only the expertise items appear in the card’s capability chips or summary

#### Scenario: Dispatch from a card
- **WHEN** the user invokes the card’s primary use action
- **THEN** the action label is `派遣`

### Requirement: Expert detail provides selectable opening prompts
The expert detail surface SHALL display identity, author/source, introduction, expertise, and a section of opening prompts. Selecting an opening prompt SHALL copy its text into the currently active real conversation composer, MUST NOT send it automatically, and SHALL keep the user in the current conversation context.

#### Scenario: Select an opening prompt with a conversation available
- **WHEN** the user selects an opening prompt from expert detail
- **THEN** the prompt text is placed in the real composer for review and the message remains unsent

#### Scenario: View expert detail without an active conversation
- **WHEN** the expert catalog is opened outside an active conversation
- **THEN** expert detail remains usable and does not create a fake composer to host the prompt

### Requirement: Composer plus menu has one canonical expert entry
Every conversation composer SHALL expose a first-level plus menu ordered as `专家组件`, a separator, then `文件或文件夹`. The obsolete browser option MUST NOT appear. `专家组件` SHALL use an SVG icon and open a secondary menu.

#### Scenario: Open the first-level plus menu
- **WHEN** the user activates the plus button on any conversation
- **THEN** the menu order and labels exactly match `专家组件`, separator, `文件或文件夹`

#### Scenario: Inspect removed choices
- **WHEN** the plus menu is visible
- **THEN** no browser choice is present

### Requirement: Recent expert menu is shared, bounded, and compact
The expert secondary menu SHALL combine experts and expert teams in one most-recently-used list shared by all conversation composers and persisted across reloads. It SHALL show at most five unique items, newest first, with each recent item rendered on one line. The secondary menu SHALL always end with an SVG-labelled `更多专家` action.

#### Scenario: More than five components were dispatched
- **WHEN** six or more distinct experts or teams have been used
- **THEN** the menu displays only the five most recently used unique components

#### Scenario: No recent components exist
- **WHEN** recent history is empty
- **THEN** the secondary menu shows only `更多专家` and does not show `没有最近的专家` or another empty placeholder

#### Scenario: Same history from different conversations
- **WHEN** a component is dispatched in one conversation and another conversation opens its expert menu
- **THEN** the second conversation sees the updated shared recent list

### Requirement: Dispatch binds in place without changing screens
Dispatching an expert or team SHALL apply it to the current conversation and update the composer’s current-expert status control without navigating away or replacing the conversation. A user SHALL be able to switch experts after messages already exist. A switch accepted while a turn is running SHALL be serialized so the in-flight turn keeps its original expert and subsequent turns use the new expert.

#### Scenario: Dispatch into an active conversation
- **WHEN** the user dispatches a different expert halfway through a conversation
- **THEN** the current screen and transcript remain in place and the composer status shows the newly selected expert

#### Scenario: Dispatch from a new-conversation composer
- **WHEN** the user selects an expert before the first message
- **THEN** the selection is placed in the composer status and used when that conversation is created

#### Scenario: Switch while a turn is active
- **WHEN** a user switches experts while an earlier turn is still executing
- **THEN** the earlier turn is not mutated and the new binding takes effect before the next turn

#### Scenario: Switching fails
- **WHEN** the server rejects or fails an expert switch
- **THEN** the UI restores the previous selection and shows an actionable error without navigating away

### Requirement: Expert discovery surfaces handle responsive and operational states
The catalog, menus, drawers, and dialogs SHALL remain usable at desktop and narrow widths, provide keyboard-visible focus and semantic controls, and distinguish loading, empty, error, and unavailable states. Long names and descriptions SHALL truncate or wrap without causing horizontal page overflow.

#### Scenario: Use a narrow viewport
- **WHEN** the available width is too small for the desktop grid or centered dialog
- **THEN** cards reflow and dialogs become a usable narrow dialog or drawer without clipped primary actions

#### Scenario: Catalog request fails
- **WHEN** expert data cannot be loaded
- **THEN** the page shows an error and retry action rather than presenting a legitimate empty catalog

