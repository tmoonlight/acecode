## ADDED Requirements

### Requirement: Confirm the active recommended reply
The Web UI SHALL activate the first highlighted action of an available final next-step card on Ctrl+Enter using the same reply path as clicking it, without modifying the composer draft or attachments.

#### Scenario: Confirm while composing
- **WHEN** an actionable next-step card is present and the user presses Ctrl+Enter in the main composer
- **THEN** exactly one reply containing the highlighted action label is sent
- **AND** the unsent composer content is preserved

#### Scenario: Competing interaction or unavailable card
- **WHEN** a dialog or menu is open, another editor/side chat/terminal owns focus, IME composition is active, the key repeats, or the card is unavailable or sending
- **THEN** the shortcut does not send a recommended reply

#### Scenario: Other key combinations
- **WHEN** the user presses Enter alone, Shift+Enter, or Ctrl+Shift+Enter
- **THEN** the card does not activate its highlighted reply

### Requirement: Show the confirmation shortcut on the primary button
The primary next-step action SHALL display compact Ctrl and Enter keycaps within the button and expose Control+Enter as its accessible shortcut. Other actions SHALL retain their existing presentation without this hint.

#### Scenario: Narrow layout
- **WHEN** the action label exceeds available button width
- **THEN** the label truncates while the shortcut hint remains visible without horizontal page overflow
