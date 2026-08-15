## Context

会话附件上传后已经由 `AttachmentStore` 保存为独立 blob，并在 `AttachmentRecord` 中持久化 `id`、`name`、`mime_type`、`size_bytes` 与 `path`。Desktop 从本机拖入文件时虽然还能把经过服务端校验的绝对 `source_path` 写入 metadata，但原生 bridge 会先读取整份文件并转成 Base64，前端随后再次解码、读取并上传，`AttachmentStore` 最终再复制一份会话 blob。普通文件因此在引用语义生效前就会命中 25 MiB 限制。浏览器上传没有可信本地来源路径，仍需要会话快照。

当前 OpenAI-compatible 序列化会对文本类文件调用 `read_attachment_bytes`，最多把 128 KiB 内容直接加入用户消息；Anthropic 序列化则只输出“不支持文件块”的占位文本。两者都没有形成一致、可按需读取的文件句柄。图片附件另有视觉能力 gate 和 `vision_analyze` 路径，本变更不能破坏它。

`file_read` 已允许读取任意只读绝对路径，因此会话快照无需新增协议或工具即可被模型按需读取。

## Goals / Non-Goals

**Goals:**

- 普通文件附件在 Provider 请求中只占用固定规模的引用上下文，不随文件正文大小增长。
- Desktop 原生来源的普通文件在附件创建阶段也只传固定规模的路径元数据，不复制文件正文，并允许引用超过 25 MiB 的本地文件。
- 给所有当前可用 Provider 提供同构且可执行的读取路径。
- 对普通上传保留会话快照，对 Desktop 来源引用保留本机原始路径的工作语义；两种记录都保持持久化、恢复和搜索兼容。
- 保持浏览器上传和图片多模态行为不变。

**Non-Goals:**

- 不新增通用文档解析、向量检索或 `attachment_search` 工具。
- 不让 Provider 原生托管 ACECode 的普通文件 blob。
- 不取消浏览器上传、图片或其他需要保存 blob 的附件大小限制。
- 不让无可信本地路径的浏览器文件伪装成来源引用。
- 不改变消息 JSONL 中 attachment part 的结构或用户可见拖放交互。
- 不允许模型把会话快照当成用户原文件进行修改。

## Decisions

### 1. 在 session 层集中构造普通文件引用

新增共享的附件提示上下文 helper，由 `AttachmentRecord` 生成稳定文本。OpenAI-compatible 与 Anthropic 序列化都调用该 helper，避免两条 Provider 路径各自维护字段和提示语。

没有选择在 Web 路由里提前插入一段额外正文，因为那会让持久化消息同时保存 file part 和重复文本，并使非 Web 入口难以复用。file part 仍是规范化的会话数据，Provider 边界负责把它降级为文本引用。

### 2. 引用按记录类型暴露工作路径与会话快照

引用 JSON 包含：

- `attachment_id`、`name`、`mime_type`、`size_bytes`；
- `snapshot_path`：`AttachmentRecord::path` 指向的会话 blob；
- `source_path`：metadata 中存在时的原始本地绝对路径；
- `read_path`：优先取 `source_path`，否则取 `snapshot_path`。

提示语明确说明正文未被内联、仅在需要时读取；若原始路径失效则可退回 `snapshot_path`；若用户要求修改原文件，只能使用 `source_path`，不得修改 `snapshot_path`。

浏览器上传和图片继续保留 `snapshot_path`。Desktop 原生普通文件引用只保留 `source_path`，不创建会话 blob；此时引用明确说明没有快照后备。没有把所有附件都改成只保留 `source_path`，因为浏览器上传没有可信本地路径，而且图片 Provider 需要实际字节。也没有强制 Desktop 普通文件继续保存 `snapshot_path`，因为那会重新引入整文件读取、复制和大小限制。

### 3. 序列化阶段不读取普通文件字节

删除文本 MIME 的特殊内联分支。所有非图片 file part，以及被 MIME 判定为非视觉图片的误标 part，都生成相同引用。引用大小只与元数据和路径长度有关。

这项规则位于 Provider 序列化边界，不改变 blob 的保存方式；历史消息重新发送时也自动采用新规则，无需迁移 JSONL。

### 4. Desktop 普通文件在入口建立来源引用

原生文件 materializer 先规范化并检查路径、名称、MIME 与实际大小。若文件属于图片路由，则继续读取字节并执行现有 25 MiB 检查；若属于普通文件路由，则返回 `reference_only: true`、绝对路径和元数据，不打开文件正文，也不返回 `data_base64`。

前端把该标记保存在仅限本次 composer 生命周期的 file-like 对象中。创建附件时，来源引用走显式的 `reference_only` 请求分支，只提交路径元数据；浏览器文件和图片仍走现有 Base64 上传分支。分支发生在 `normalizeImageFile`、`FileReader` 和上传大小检查之前，确保普通大文件不会被误读。

服务端不信任客户端提供的大小或路径形态：它重新 canonicalize `source_path`、确认目标仍是普通文件、读取实际大小，并拒绝把图片创建为无 blob 引用。`AttachmentStore` 为来源引用只写 metadata JSON，`path` 与 `blob_url` 为空，metadata 标记 `storage: source_reference`。加载时只有存在 `path` 的历史/上传记录才自动补全 blob URL。

### 5. Provider 适配保持现有继承边界

- `OpenAiCompatProvider` 使用共享引用；Grok 与 Copilot 继承或复用该请求构造逻辑，因此自动获得相同行为。
- `AnthropicProvider` 在遇到 file part 时解析同一份 `AttachmentRecord` 并使用共享引用，不再仅输出“附件被省略”。
- 无效附件 metadata 继续输出明确的不可用占位文本，不尝试猜测路径。

### 6. 图片路径保持原样

视觉模型仍接收 `image_url` 数据；非视觉模型仍得到 `vision_analyze` 或配置提示。SVG、PDF 或 MIME 不匹配的 part 仍按普通文件引用处理。这样不会把“普通文件引用优先”错误扩展为“图片也只传路径”。

## Risks / Trade-offs

- [模型可能在任务确实需要正文时忘记读取] → 引用中给出明确的 `read_path` 与按需读取指令，并通过测试锁定措辞和字段。
- [原始文件在发送后发生变化或消失] → 同时提供 `snapshot_path` 作为发送时内容的后备副本，并提示读取失败时回退。
- [来源引用在恢复会话前被移动、删除或修改] → 引用明确标注没有会话快照；读取失败时要求用户重新关联，文件内容以使用时的当前版本为准。
- [暴露会话存储绝对路径增加提示中的本地路径信息] → 该路径只进入已授权的模型请求，且现有 Desktop 路径机制已经会发送原始绝对路径；不写入新的公开 API 字段。
- [模型误改会话快照导致附件记录与 blob 不一致] → 引用明确禁止修改 `snapshot_path`；普通权限模式也会阻止工作区外写入。危险模式仍遵循其既有的外部写入权限语义。
- [二进制文件不能由 `file_read` 直接理解] → 指令允许选择其他合适的只读检查工具；本变更只建立引用语义，不承诺新增格式解析能力。

## Migration Plan

无需数据迁移。部署后，新旧会话中的 file part 都在下一次 Provider 序列化时生成引用；新来源引用使用相同的 `AttachmentRecord` JSON，仅以空 `path`/`blob_url` 和 metadata 存储标记区分。回滚时已有来源引用仍可被旧版本解析，但旧版本无法提供 blob，因此完整回滚前应让用户重新添加这些文件。

## Open Questions

无。后续若引入文档检索能力，可让引用继续复用 `attachment_id`，而不改变本次路径语义。
