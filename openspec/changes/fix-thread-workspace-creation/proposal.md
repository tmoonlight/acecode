## Why

无工作区会话通过 `create_thread` 创建新会话时，只复制临时 cwd，遗漏 no_workspace，导致新会话同时被无工作区列表和可见工作区列表排除。创建工具也不能指定目标工作区或目录，无法完成用户明确要求的跨工作区创建。

## What Changes

- 创建工具支持可选 `workspaceHash`、`cwd`、`noWorkspace`，默认继承调用会话归属。
- 无工作区新会话使用独立缓存目录；普通目标目录注册为可见工作区，返回真实位置元数据。
- 续聊工具支持全局定位和可选 `workspaceHash`，使新会话创建后仍可从原会话继续发送任务。
- 补充无工作区、跨工作区、目标校验、失败清理及续聊回归测试，更新工具协议文档。

## Capabilities

### New Capabilities

- `thread-workspace-creation`: 定义 AI 创建会话的目标选择、可见性、返回身份与跨工作区续聊约定。已有 thread 工具规范仍在未归档变更中，本次不覆盖其历史记录。

### Modified Capabilities

无。复用既有 shared-daemon-workspaces 的工作区注册与会话归属机制。

## Impact

涉及 `ThreadService`、thread 工具 schema、会话工具单元测试和 `docs/daemon-api.md`。不变更 HTTP 路由，不改其他元数据修改工具的作用域，不自动修改已有用户会话，不发布或替换运行中的 Desktop。
