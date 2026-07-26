## ADDED Requirements

### Requirement: Chat timeline renders output image attachments
The Web UI chat timeline SHALL render structured image attachments attached to user, assistant, and tool result messages using a shared thumbnail and preview experience.

#### Scenario: Assistant output image thumbnail
- **WHEN** an assistant message contains an image attachment content part
- **THEN** the chat timeline SHALL show an image thumbnail in the assistant message
- **AND** clicking the thumbnail SHALL open a larger image preview

#### Scenario: Tool output image thumbnail
- **WHEN** a tool result payload or resumed tool result message contains image attachment metadata
- **THEN** the corresponding `ToolBlock` SHALL show the image thumbnail with the tool result
- **AND** clicking the thumbnail SHALL open a larger image preview

#### Scenario: Non-image output attachment fallback
- **WHEN** an assistant or tool result message contains a non-image output attachment
- **THEN** the chat timeline SHALL show a compact file chip containing the attachment name and type instead of trying to render it as an image

#### Scenario: Legacy messages remain unchanged
- **WHEN** a chat message has no structured attachments
- **THEN** the Web UI SHALL preserve the existing markdown, text, diff, and tool output rendering behavior

### Requirement: Tool event payload exposes output attachments
The daemon's tool result event payload SHALL include a stable `attachments` array so the Web UI can render output artifacts during live streaming and after resume.

#### Scenario: Live tool output attachment event
- **WHEN** a tool completes with one or more output attachments
- **THEN** the live `tool_end` payload SHALL include an `attachments` array containing the stored attachment metadata

#### Scenario: Resumed tool output attachment event
- **WHEN** the Web UI reloads a session whose JSONL contains a tool message with output attachments
- **THEN** the REST replay events SHALL include the same attachment metadata in the tool result payload
