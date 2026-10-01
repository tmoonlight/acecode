## ADDED Requirements

### Requirement: Tool executions retain distinct identities
系统 SHALL 为不同工具执行保留可区分身份，提供者复用 ID 时不得导致结果、事件或持久化引用串用。

#### Scenario: Reused IDs across responses
- **WHEN** 提供者在连续响应中复用同一工具调用 ID，先返回大输出，再返回短输出
- **THEN** 后续模型请求包含各次执行自己的真实结果或预览
- **AND** 调用、结果及执行事件使用匹配且互不冲突的 ID

#### Scenario: Duplicate IDs in one batch or after history replacement
- **WHEN** 同批请求包含重复 ID，或历史压缩、替换后再次出现已使用的 ID
- **THEN** 每次新执行仍获得独立身份，不复用旧执行的结果

## MODIFIED Requirements

### Requirement: Persisted output previews expose a recoverable path
系统 SHALL 写入本次完整工具输出，并提供指向该内容的模型可见预览。

#### Scenario: Text output is persisted
- **GIVEN** 工具结果需要持久化
- **WHEN** 持久化成功
- **THEN** 完整输出写入活动会话的 tool-results 目录下的独立文件
- **AND** 预览包含原始大小、绝对路径及前 2000 字节 UTF-8 安全文本

#### Scenario: Persistence file already exists
- **GIVEN** 同一提供者 ID 或清理后同名 ID 曾保存其他输出
- **WHEN** 新输出需要持久化
- **THEN** 新输出拥有独立文件和正确预览
- **AND** 旧文件内容保持可读，不被新输出覆盖

#### Scenario: Already persisted preview is delivered again
- **WHEN** 已经持久化的预览再次经过结果交付
- **THEN** 保留已有预览和引用，不重复持久化预览文本

### Requirement: Replacement decisions survive resume
系统 SHALL 持久化精确替换决定，并在恢复会话时重建可唯一匹配的替换状态。

#### Scenario: Replacement record exists
- **GIVEN** 会话存在 content-replacement 元记录且目标 ID 唯一
- **WHEN** 恢复会话
- **THEN** 匹配的工具正文在进入提供者历史前替换为保存的精确预览

#### Scenario: Historical ID is ambiguous
- **GIVEN** 多条历史工具结果复用同一 ID
- **WHEN** 恢复会话并重建替换状态
- **THEN** 不将按该 ID 存储的单份替换文本套用到这些结果
- **AND** 保留每条已有正文及结构化元数据

#### Scenario: Replacement meta is not shown or sent
- **GIVEN** 会话含替换元消息
- **WHEN** 回放界面或准备提供者请求
- **THEN** 元消息不出现在界面，也不发送给提供者
