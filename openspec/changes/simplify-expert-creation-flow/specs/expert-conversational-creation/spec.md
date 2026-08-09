## ADDED Requirements

### Requirement: New expert uses one split primary control
The expert-components page SHALL retain the visible `新建专家` primary action with its existing icon, label, 32px height, and visual priority, and SHALL add a separate dropdown-arrow segment on its right. Activating the primary segment MUST start conversational expert creation and MUST NOT open the advanced editor. Activating the arrow segment MUST only toggle its menu and MUST NOT navigate to a conversation.

#### Scenario: Activate the primary segment
- **WHEN** a user activates the `新建专家` primary segment
- **THEN** ACECode starts the conversational-creation navigation without opening `ExpertEditor`

#### Scenario: Activate the arrow segment
- **WHEN** a user activates the dropdown arrow
- **THEN** the current page remains visible and the advanced-options menu opens without starting a new conversation

### Requirement: Conversational creation stages Expert Manager without sending
The conversational-creation action SHALL navigate to ACECode's real new-task composer in the expert page's current workspace and SHALL stage exactly `/expert-manager ` as the composer draft. The staged Skill invocation MUST remain unsent, MUST receive normal composer focus, and MUST remain editable before the user manually submits it. The navigation action MUST NOT create a first user message, execute the Skill, or request an assistant response.

#### Scenario: Enter conversational creation
- **WHEN** a user activates the `新建专家` primary segment
- **THEN** the real new-task composer opens in the same workspace with `/expert-manager ` visible and unsent

#### Scenario: Continue the staged request
- **WHEN** the user types after the staged Skill token
- **THEN** the additional text remains in the normal composer draft and no message is sent until the user submits it

#### Scenario: Navigate without automatic execution
- **WHEN** conversational creation opens
- **THEN** ACECode has not called the session message path or generated a user or assistant transcript item for the staged text

### Requirement: Conversational creation reuses the canonical composer draft lifecycle
The staged Expert Manager invocation SHALL use the existing one-shot new-task draft handoff and workspace-scoped home composer storage. It SHALL take precedence over the current home draft for the selected workspace, SHALL be consumed exactly once after application, and MUST NOT alter drafts belonging to other workspaces. The system MUST NOT add a second composer or a separate expert-creation draft store.

#### Scenario: Consume the navigation draft once
- **WHEN** the composer applies the staged `/expert-manager ` navigation draft and the user then edits it
- **THEN** a parent re-render does not reapply the original token over the user's edit

#### Scenario: Preserve another workspace draft
- **WHEN** workspace A has an unsent draft and conversational creation is opened for workspace B
- **THEN** workspace A's stored draft remains unchanged

#### Scenario: Inspect the expert page
- **WHEN** the user remains on the expert-components page
- **THEN** that page contains no generic, simulated, fixed, or duplicated chat composer

### Requirement: Advanced mode is the only dropdown option
The dropdown menu SHALL contain exactly one actionable item labelled `高级模式`. Selecting it SHALL close the menu and open the existing Agent-type `ExpertEditor` with a new expert form. It MUST NOT create a replacement editor, alter the `组建专家团` action, or navigate to the conversation composer.

#### Scenario: Select advanced mode
- **WHEN** a user selects `高级模式`
- **THEN** the original new-Agent `ExpertEditor` opens with the existing save, cancel, validation, and catalog-refresh behavior

#### Scenario: Inspect menu contents
- **WHEN** the dropdown menu is open
- **THEN** `高级模式` is its only action and no generic conversational-creation item is present

### Requirement: Split-control menu is keyboard and overlay safe
The arrow SHALL expose menu expansion state to assistive technology, and the menu SHALL use menu/menuitem semantics. Escape and an outside pointer action SHALL close the menu; Escape SHALL restore focus to the arrow. The menu SHALL be marked as an ACECode native-overlap overlay so Desktop browser surfaces do not cover it.

#### Scenario: Close with Escape
- **WHEN** keyboard focus is within the open menu and the user presses Escape
- **THEN** the menu closes and focus returns to the dropdown arrow

#### Scenario: Close from outside
- **WHEN** the menu is open and the user presses outside the split control and menu
- **THEN** the menu closes without starting conversational or advanced creation

#### Scenario: Render over a native browser surface
- **WHEN** the expert page menu overlaps an Agent Browser surface in Desktop
- **THEN** the menu is classified as an overlap overlay and remains visible above that surface
