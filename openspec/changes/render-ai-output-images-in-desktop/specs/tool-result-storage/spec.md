## ADDED Requirements

### Requirement: Tool results preserve structured attachments
The tool-result storage pipeline SHALL preserve structured output attachment metadata independently from model-visible text output.

#### Scenario: Format tool result with attachments
- **WHEN** `ToolExecutor::format_tool_result` formats a `ToolResult` that contains output attachments
- **THEN** the returned tool message SHALL include the text output and the structured attachment metadata

#### Scenario: Large text replacement preserves attachments
- **WHEN** a tool result's text output is replaced by a persisted-output preview because of the large-result budget
- **THEN** any structured output attachments associated with that tool result SHALL remain attached to the recorded tool message

#### Scenario: Resume restores attachment metadata
- **WHEN** ACECode resumes a session containing a persisted tool result message with output attachments
- **THEN** the restored runtime message SHALL retain the attachment metadata for UI replay
