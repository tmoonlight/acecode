# 阶段 1：读取锁、解析与恢复（2026-10-01）

- 锁回归在修改前稳定失败：冻结 A 的历史读取后，列表、B 的草稿/权限、A 的追加都等待超过 1 秒。修改后通过；快照只包含读取开始时的记录，并发追加保留在文件中。
- 113 个定向 C++ 用例通过，覆盖 SessionFileReader、SessionHistoryConcurrency、SessionStorage、SessionManagerResume、SessionRegistry、SessionSerializer、MessagePayload、SessionResumeRestore、FileStateRestore 及 messages HTTP smoke。
- 独立显示快照涵盖标题、摘要、轮数、用量和 worktree；注册表只在锁内复制条目引用。
- 新文件读取器持有原生文件身份与固定大小；在锁外读内容，结束时复核身份和开头指纹，改写后重试一次。连续两次冲突返回 HISTORY_CURSOR_STALE。
- Windows 实测 MoveFileEx 在保留读句柄时返回错误 5。会话 JSONL 原子改写单独启用 ReplaceFileW 分支；其它 atomic_write_file 调用维持原默认分支。替换成功及旧身份失效测试通过。API 行为依据 [Microsoft ReplaceFileW 文档](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)。
- 反序列化复用已解析 JSON；响应直接构造 JSON。固定字段样例、旧格式、损坏行/末尾半行与消息 id 契约均通过。
- 文件状态只处理最新有效检查点后的调用，并按规范化路径保留最后一次有效操作。2000 次旧编辑读取 0 个当前文件；同一路径及其别名 50 次编辑只读取 1 次。
- 恢复时通过空闲队列门保护历史写入；隔离 daemon 恢复日志中 ConversationHistory idle mutation 警告为 0。

| 100MB 会话操作 | 初始基线 | 解锁后 | 单次解析及恢复优化后 |
|---|---:|---:|---:|
| 恢复 | 1068ms | 635ms | 402ms |
| since=0 | 2234ms | 1700ms | 971ms |
| 加载历史期间列表 | 930ms | 3.95ms | 3.90ms |
| 响应字节 | 103,353,724 | 相同 | 相同 |

此时仍是全量历史，分页前不宣称首屏与总长度无关。复测为同一合成数据的热文件缓存样本，详见相应 JSON；全量列表首次枚举受后台预热及系统缓存影响，尚待侧栏阶段独立验证。
