## Why

ACECode 目前会把文本类普通附件最多 128 KiB 的内容自动拼进模型正文，既消耗上下文，也让“拖入一个文件”和“把文件全文粘贴进消息”失去区别；同时 Anthropic 路径只提示附件被省略，没有给模型可按需读取的有效引用。Desktop 虽然能够取得外部文件的绝对路径，却仍会先把整份文件读取、Base64 编码并复制进会话附件存储，导致普通文件在引用生成前就被 25 MiB 限制拦截。外部拖入的普通文件应像 Grok Build 与 Codex 的文件路径机制一样，以持久引用为主，并仅在任务需要时读取内容。

## What Changes

- 普通文件附件发送给模型时只提供附件 ID、名称、MIME、大小、会话快照路径，以及可用时的原始本地路径，不再自动内联文本内容。
- 引用明确标注内容尚未进入正文，并指导模型仅在任务需要时通过 `file_read` 或合适的只读工具读取 `read_path`。
- 有原始本地路径时将其作为工作路径；会话快照路径继续作为无原始路径上传和原始文件失效时的持久读取后备，并明确禁止把快照当作用户原文件修改。
- Desktop 拖入、粘贴或原生选择的本地普通文件直接创建来源引用记录，只传名称、MIME、实际大小和经过校验的绝对路径，不读取、Base64 编码或复制文件正文，也不受 25 MiB 上传快照限制。
- 浏览器上传和图片附件继续保存会话快照；图片仍遵循现有压缩、多模态路由和 25 MiB 安全边界。
- OpenAI-compatible、Grok、Copilot 与 Anthropic 序列化路径统一使用同一份普通文件引用格式。
- 图片附件继续走现有多模态或 `vision_analyze` 能力路由，不改变图片字节发送与降级行为。

## Capabilities

### New Capabilities

- `reference-first-file-attachments`: 规定普通文件附件的持久引用字段、按需读取语义、Provider 一致性，以及与图片附件的边界。

### Modified Capabilities

无。

## Impact

- 影响 `src/session/` 中附件引用上下文的共享构造逻辑。
- 影响 Desktop 原生文件 materializer、Web 附件创建路由和前端附件接线，使本地普通文件可以建立无 blob 的来源引用记录。
- 影响 `src/provider/openai_provider.cpp` 与 `src/provider/anthropic_provider.cpp` 的普通文件附件序列化。
- 更新 Desktop、附件存储、HTTP、前端与 Provider 测试，验证大文件不复制、正文不内联、引用包含可读取路径、不同 Provider 行为一致。
- 保持现有 JSONL 附件 part 兼容；来源引用没有 blob，因而 blob 下载端点对其返回不可用。
