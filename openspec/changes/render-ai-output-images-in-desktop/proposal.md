## Why

ACECode already accepts user image attachments and sends them to vision-capable providers, but assistant and tool-produced images still collapse into text, file paths, or opaque tool output. Desktop users need generated screenshots, image artifacts, and image-capable tool results to appear in the chat transcript as durable visual attachments after live streaming and session resume.

## What Changes

- Extend tool result data with structured output attachments for images and other files.
- Persist assistant/tool image outputs as session `content_parts` attachment references instead of embedding large data URLs or relying on text path scraping.
- Add daemon-side materialization for image data URLs and local image paths produced by tools so transcript JSONL stores metadata references only.
- Add a read-only `show_image` built-in so the agent can intentionally surface a supported local image through the output-attachment pipeline.
- Render image attachments for assistant messages and tool result blocks in the desktop/web chat timeline, with thumbnails and click-to-preview.
- Keep provider-facing tool result text compatible; output image attachments are displayed in the UI but not automatically sent back to the provider as inline image content unless an existing provider path explicitly consumes `content_parts`.
- Preserve TUI behavior with a readable `[image: name]` style fallback rather than terminal image rendering.

## Capabilities

### New Capabilities
- `assistant-output-attachments`: Assistant and tool-produced files, especially images, are represented as durable output attachments that can be rendered in chat.

### Modified Capabilities
- `session-storage`: Session JSONL must round-trip assistant/tool `content_parts` and output attachment metadata across resume.
- `webui-rich-rendering`: Desktop chat rendering must show assistant and tool output image attachments, not only user-uploaded attachments.
- `tool-result-storage`: Tool results may include structured attachments; large text result replacement must preserve attachment metadata.

## Impact

- C++ core: `ToolResult`, `ToolExecutor::format_tool_result`, `AgentLoop` tool-result persistence, session serialization, attachment store helpers, tool event payloads.
- Daemon/API: attachment blob endpoints are reused; message and SSE payloads gain structured output attachment metadata.
- Desktop/web: chat transcript projection, `Message.jsx`, `ToolBlock.jsx`, shared attachment thumbnail/preview component, frontend tests.
- Tests: C++ serializer/tool event/agent-loop metadata tests plus JS component/projection tests for assistant/tool attachments.
