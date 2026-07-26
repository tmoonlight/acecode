## ADDED Requirements

### Requirement: Structured output attachments
The system SHALL represent assistant and tool-produced files as structured output attachments instead of relying on plain text paths, markdown image syntax, or embedded data URLs.

#### Scenario: Tool result carries an output image
- **WHEN** a tool returns output text and one or more image output attachments
- **THEN** the recorded tool result message SHALL contain the text output and structured attachment metadata for each image
- **AND** the transcript SHALL NOT store raw image bytes or full data URLs in the message content

#### Scenario: Assistant message carries an output image
- **WHEN** an assistant response is recorded with image output attachment metadata
- **THEN** the assistant message SHALL preserve the attachment metadata in its structured parts
- **AND** the desktop chat timeline SHALL be able to display the image after a session reload

### Requirement: Output image materialization
The daemon SHALL materialize supported output image sources into the active session attachment store before persisting them in the transcript.

#### Scenario: Data URL image output
- **WHEN** a tool returns an output attachment whose source is a `data:image/...;base64,...` URL
- **THEN** the daemon SHALL decode and validate the bytes, store them as a session attachment, and persist only the resulting attachment record

#### Scenario: Local path image output
- **WHEN** a tool returns an output attachment whose source is a local image path
- **THEN** the daemon SHALL read and validate the image bytes, store them as a session attachment, and persist only the resulting attachment record

#### Scenario: Invalid output image source
- **WHEN** an output attachment source cannot be decoded, read, validated, or stored
- **THEN** the tool result SHALL remain text-visible
- **AND** the failure SHALL be represented as a clear local error or warning instead of persisting a broken image reference

### Requirement: Output attachments preserve model-visible text semantics
Output attachments SHALL be display metadata by default and SHALL NOT silently change the provider-visible tool result content.

#### Scenario: Provider follow-up after image-producing tool
- **WHEN** the agent loop sends a tool result with output attachments back to the provider
- **THEN** the provider-visible tool result SHALL preserve the tool's text output contract
- **AND** the output attachment metadata SHALL NOT be converted into an image input part unless an explicit provider path opts in

### Requirement: Built-in local image surfacing
The shared built-in tool set SHALL expose a read-only `show_image` tool that intentionally surfaces a supported local raster image through the structured output-attachment pipeline.

#### Scenario: Surface a workspace-relative image
- **WHEN** `show_image` receives a supported image path relative to the active session working directory
- **THEN** the tool SHALL resolve the path and return a local-path output-attachment descriptor
- **AND** the existing output-attachment materializer SHALL enforce the active path policy and store the image durably

#### Scenario: Reject an invalid local image
- **WHEN** `show_image` receives a missing path, a non-regular file, or an unsupported file extension
- **THEN** the tool SHALL fail with a clear error
- **AND** it SHALL NOT emit an output-attachment descriptor
