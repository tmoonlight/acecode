## Why

ACECode 目前缺少一组可由模型直接调用、且名称尽量与 Codex 对齐的会话管理工具；同时，部分模型后端会在本地估算尚未触顶时提前拒绝请求，导致普通压缩与手动压缩都无法继续，最终只能人工回退或编辑会话文件。需要把会话操作与无模型依赖的会话修复能力统一落到会话域中，让健康会话可以管理或修复其他会话，并让运行中的会话在明确的上下文溢出错误后获得有限、自洽的自动恢复机会。

## What Changes

- 新增与 Codex 命名对齐的会话工具：`create_thread`、`fork_thread`、`list_threads`、`read_thread`、`send_message_to_thread`、`wait_threads`、`set_thread_title`、`set_thread_pinned`、`set_thread_archived` 和 `delete_thread`。
- 新增 ACECode 扩展工具 `repair_thread`，用于从另一个健康会话主动诊断和修复指定会话。
- 新增共享的确定性会话修复服务：基于追加式 JSONL、最新有效 compact checkpoint 和现有 provider history recovery 进行诊断与恢复，不调用模型、不重放工具，也不改写用户可见历史。
- 在普通模型请求收到可确认的上下文溢出错误时接入有限、单调的自动修复阶段；仅在尚未产生助手输出或工具活动时自动重试，避免重复副作用。
- 对无法通过裁剪旧历史解决的超限，提供受限的紧急请求配置，优先缩减可选的请求局部上下文与工具定义；仍不可恢复时返回明确原因。
- 补充会话工具、修复服务、错误分类与自动恢复边界的测试和文档。

## Capabilities

### New Capabilities

- `codex-thread-tools`: 定义模型可调用的 Codex 对齐会话工具、参数、返回值和生命周期语义。
- `thread-repair`: 定义手动与自动会话修复、确定性历史裁剪、追加式 checkpoint、有限重试和不可恢复结果。

### Modified Capabilities

无。

## Impact

- 受影响代码主要位于 `src/tool/`、`src/session/`、`src/agent_loop.*`、会话注册与 daemon/web 会话接口，以及相应测试。
- 会新增模型可见工具定义和会话域服务，但不新增外部依赖，不引入通用授权层、会话配额或任意 JSONL 编辑接口。
- 会话删除沿用产品已有的删除语义；会话修复始终保留原始追加式记录，并通过新 checkpoint 表达恢复后的模型上下文。
