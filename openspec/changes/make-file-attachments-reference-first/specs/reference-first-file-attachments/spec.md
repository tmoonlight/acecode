## ADDED Requirements

### Requirement: 普通文件使用引用优先序列化
系统 MUST 将普通文件附件序列化为固定规模的引用上下文，并且 MUST NOT 在 Provider 请求中自动内联附件正文。

#### Scenario: 文本附件不进入消息正文
- **WHEN** 用户消息包含一个正文中带有可识别标记的文本文件附件
- **THEN** Provider 请求包含该文件的引用元数据
- **AND** Provider 请求不包含该文件正文中的可识别标记

#### Scenario: 非文本文件同样生成引用
- **WHEN** 用户消息包含 PDF、SVG 或其他非图片普通文件
- **THEN** Provider 请求包含普通文件引用
- **AND** Provider 请求不生成图片 payload

### Requirement: 引用提供可按需读取的持久路径
系统 MUST 在普通文件引用中提供附件 ID、名称、MIME、大小与会话快照路径，并 MUST 明确说明文件内容尚未内联且仅在任务需要时读取。

#### Scenario: 只有会话快照的上传文件
- **WHEN** 普通文件附件没有原始本地路径
- **THEN** `read_path` 等于 `snapshot_path`
- **AND** 模型可通过 `file_read` 或合适的只读工具按需读取该绝对路径

#### Scenario: Desktop 本地来源文件
- **WHEN** 普通文件附件同时具有经过校验的 `source_path` 与会话 `snapshot_path`
- **THEN** 引用同时保留两个路径
- **AND** `read_path` 等于 `source_path`
- **AND** 引用说明原始路径失效时可退回会话快照
- **AND** 引用说明不得把会话快照当作用户原文件修改

### Requirement: 当前 Provider 使用一致的普通文件引用
系统 MUST 让 OpenAI-compatible、Grok、Copilot 与 Anthropic 请求使用同一份普通文件引用字段和按需读取语义。

#### Scenario: OpenAI-compatible 系列序列化普通文件
- **WHEN** OpenAI-compatible、Grok 或 Copilot 模型接收普通文件 part
- **THEN** 请求内容包含共享普通文件引用
- **AND** 请求内容不包含自动读取的附件正文

#### Scenario: Anthropic 序列化普通文件
- **WHEN** Anthropic 模型接收普通文件 part
- **THEN** 请求内容包含共享普通文件引用
- **AND** 请求内容不再只有“不支持文件块”的占位文本

### Requirement: Desktop 本地普通文件在入口使用来源引用
系统 MUST 让 Desktop 原生拖入、粘贴或选择的本地普通文件仅以经过服务端校验的来源路径建立附件记录，并且 MUST NOT 为该记录读取、Base64 编码或复制文件正文。

#### Scenario: 超过上传上限的本地 PDF
- **WHEN** 用户在 Desktop 中拖入一个超过 25 MiB 的本地 PDF 或其他普通文件
- **THEN** 原生 bridge 返回来源引用元数据而不是文件字节
- **AND** 附件创建成功且记录的 `size_bytes` 等于服务端读取的实际文件大小
- **AND** 记录包含 `source_path`，但不包含可用的 `snapshot_path` 或 `blob_url`
- **AND** Provider 请求只包含固定规模的引用上下文

#### Scenario: 普通小文件同样不复制
- **WHEN** 用户在 Desktop 中拖入一个不超过 25 MiB 的本地普通文件
- **THEN** 系统仍使用来源引用创建附件
- **AND** 系统不因文件较小而恢复 Base64 上传或会话 blob 复制

#### Scenario: 来源引用由服务端重新校验
- **WHEN** 客户端请求创建来源引用
- **THEN** 服务端重新规范化绝对路径并确认目标是现存普通文件
- **AND** 服务端忽略客户端对文件大小的声明并读取实际大小
- **AND** 相对路径、失效路径和图片来源引用被拒绝

#### Scenario: 浏览器文件仍保存快照
- **WHEN** 文件没有 Desktop 原生提供的可信绝对来源路径
- **THEN** 系统继续通过现有上传流程创建会话快照
- **AND** 现有 25 MiB blob 上限继续生效

### Requirement: 图片附件保持多模态能力路由
系统 MUST 保持图片附件现有的视觉模型 payload、非视觉模型降级和 `vision_analyze` 行为，不得因普通文件引用规则而停止发送受支持的图片内容。

#### Scenario: 视觉模型接收图片
- **WHEN** 视觉模型接收有效图片附件
- **THEN** Provider 请求继续包含图片 payload
- **AND** 图片不会被降级为普通文件引用

#### Scenario: 非视觉模型接收图片
- **WHEN** 非视觉模型接收有效图片附件
- **THEN** Provider 请求继续使用现有图片句柄和视觉能力提示

#### Scenario: Desktop 图片不使用来源引用绕过限制
- **WHEN** Desktop 原生 bridge 接收图片文件
- **THEN** 系统继续读取并上传图片字节
- **AND** 图片压缩与 25 MiB 安全边界继续生效
