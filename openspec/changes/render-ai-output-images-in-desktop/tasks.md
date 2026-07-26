## 1. Backend Attachment Model

- [x] 1.1 Add a structured output attachment field to `ToolResult` and helper conversion between attachment records and content parts.
- [x] 1.2 Teach `ToolExecutor::format_tool_result` to carry output attachments onto tool messages without changing text output.
- [x] 1.3 Add daemon/session materialization helpers for data URL and local-path output images using the existing attachment store.

## 2. Agent Loop And API Payloads

- [x] 2.1 Persist attachment-bearing tool results in `AgentLoop` JSONL records and keep attachment metadata when large text output is replaced.
- [x] 2.2 Include output attachment metadata in live `tool_end` event payloads.
- [x] 2.3 Include output attachment metadata in REST replay/resume tool events.

## 3. Desktop/Web Rendering

- [x] 3.1 Extract the user-only attachment strip into a shared attachment renderer with image thumbnail and click-to-preview behavior.
- [x] 3.2 Render structured attachments on assistant messages.
- [x] 3.3 Render structured attachments on `ToolBlock` for live and resumed tool results.
- [x] 3.4 Preserve legacy text/markdown/diff rendering when no attachments are present.

## 4. Tests And Validation

- [x] 4.1 Add C++ tests for tool result formatting, session serialization, payload encoding, and large-result replacement preserving attachments.
- [x] 4.2 Add frontend tests for attachment extraction/rendering on assistant and tool messages.
- [x] 4.3 Run OpenSpec validation for `render-ai-output-images-in-desktop`.
- [x] 4.4 Run the relevant C++ unit tests.
- [x] 4.5 Run `pnpm test` and `pnpm build` in `web/`.

## 5. Legacy `show_image` Migration

- [x] 5.1 Add and register a read-only `show_image` built-in that resolves supported local image paths and emits current output-attachment descriptors.
- [x] 5.2 Add unit tests for the tool contract, path and format validation, UTF-8 paths, and shared built-in registration.
- [x] 5.3 Run OpenSpec validation and the relevant C++ tests/build for the migrated tool.
