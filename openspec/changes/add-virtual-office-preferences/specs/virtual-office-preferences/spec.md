## ADDED Requirements

### Requirement: Persistent office opt-in
The Windows and macOS desktop application SHALL default Virtual Office to disabled and persist the user's choice. Menu, settings and native close SHALL share one state. Enabling after closing SHALL recreate the office and resume current-session updates.

#### Scenario: Reopen a closed office
- **WHEN** the user closes an enabled office and then selects Show Virtual Office
- **THEN** the menu and setting reflect the new state and the recreated office follows the current session

### Requirement: Discoverable controls
The gear menu SHALL place Show/Hide Virtual Office between Settings and Appearance. Settings SHALL place Virtual Office between Personalization and Usage and provide an Enable Virtual Office switch.

#### Scenario: Toggle from either entry point
- **WHEN** the user toggles the setting or gear action
- **THEN** the office visibility and both controls agree, including after restart

### Requirement: One-time invitation
Starting with version 0.9.37 the desktop app SHALL offer a one-time invitation with description “打开虚拟办公室，为您的工作增添更多乐趣！” and an animated office preview. Dismissal SHALL keep the default disabled. Preview resources SHALL exist only while the invitation is open.

#### Scenario: First eligible launch
- **WHEN** an eligible desktop version launches without a previous choice or invitation acknowledgement
- **THEN** it asks once, after other blocking startup surfaces, and enables the office only after affirmative consent

#### Scenario: Earlier version or acknowledged invitation
- **WHEN** the version is below 0.9.37 or the invitation was acknowledged
- **THEN** no automatic invitation is shown

### Requirement: Early session bridge registration
Desktop office bridges SHALL be installed before main-page navigation on both supported platforms. Hidden offices SHALL not retain session polling or subscriptions.

#### Scenario: Initial macOS navigation
- **WHEN** the Web UI first mounts on macOS
- **THEN** the office bridges are already available and enabling the office starts session linkage without reloading
