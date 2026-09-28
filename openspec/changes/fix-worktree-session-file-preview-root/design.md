<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

会话元数据的 `cwd` 是项目存储及工作区归属目录；worktree 状态另存于 `worktree_session.worktree_path`。恢复时 AgentLoop 切入 worktree，但多处 Web 响应仍返回主工作区 `cwd`。`ChatView` 已能优先使用 `ref.worktree.path`，问题在于侧栏与跳转构造 ref 时漏传该字段。

## Decisions

- `cwd` 保持工作区归属语义；`working_cwd` 表示当前执行目录，优先使用有效 worktree 路径。会话存储路径继续从原 `cwd` 计算。
- 列表和搜索结果携带 worktree 元数据；恢复响应返回当前 worktree 状态，明确用 `null` 表示已退出或恢复时清除的状态，以覆盖可能过期的侧栏记录。
- 导航 ref 传递 worktree，并让恢复响应优先于旧列表记录。这样相对路径预览和工作树标签使用同一会话状态。

## Validation

- Web 路由测试验证工作树会话的列表、恢复与文件内容接口使用 worktree 目录。
- JavaScript 测试验证跳转 ref 的 worktree 优先级及相对文件路径。
- 运行 Web 测试、构建、定向 C++ 测试及 OpenSpec 严格验证。
