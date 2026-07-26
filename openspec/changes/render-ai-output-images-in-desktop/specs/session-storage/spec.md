## MODIFIED Requirements

### Requirement: ChatMessage JSONL serialization
系统 SHALL 能够将 `ChatMessage` 序列化为单行 JSON,并从单行 JSON 反序列化恢复。序列化 MUST 保留 role, content, tool_calls, tool_call_id 全部字段。序列化 MUST 保留结构化 `content_parts` 字段,包括 user 输入附件、assistant 输出附件和 tool 输出附件的 metadata 引用。空字段 MAY 省略。

#### Scenario: Serialize a user message
- **WHEN** 一条 `ChatMessage{role="user", content="fix the bug"}` 被序列化
- **THEN** 输出为单行有效 JSON: `{"role":"user","content":"fix the bug"}`

#### Scenario: Serialize an assistant message with tool calls
- **WHEN** 一条包含 tool_calls 的 assistant 消息被序列化
- **THEN** 输出 MUST 包含完整的 tool_calls 数组(含 id, function.name, function.arguments)

#### Scenario: Serialize a message with output image content parts
- **WHEN** 一条 assistant 或 tool 消息包含 image 类型的 `content_parts`
- **THEN** 输出 MUST 包含完整的 `content_parts` 数组以及每个附件的 id, name, mime_type, size_bytes, path/blob_url 元数据
- **AND** 输出 MUST NOT 包含图片原始 bytes 或完整 data URL

#### Scenario: Roundtrip fidelity
- **WHEN** 一条 ChatMessage 经过序列化再反序列化
- **THEN** 所有字段 MUST 与原消息完全一致
