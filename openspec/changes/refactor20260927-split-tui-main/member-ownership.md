# B-13 成员与启动登记

实现日期:2026-09-28。以下是源代码装配记录,不是构建或运行验收。

| 组 | 实际成员 | 创建时点与生命周期 |
| --- | --- | --- |
| A | options, environment, services, provider accessor | TuiApp 构造只保存参数; Environment / Services 阶段创建外部资源 |
| 公共状态 | callbacks, auth_done, mcp_first_turn_wait_done, anim_tick, version/cwd/exit id, layout/session flags | 声明在其消费者之前;后台任务与组件停止后才析构 |
| B | state, screen_host, viewport, geometry | Screen 阶段才创建屏幕,注册紧随屏幕创建;所有 worker 在成员析构前显式停止 |
| C | token tracker, permissions, SessionManager, turn observation, abort flag, submitter, overlay gate, turn lifecycle, title runner, bridge, AgentLoop, SubagentHost, commands, command factory | AgentAssembly 与 MainSession 为独立步骤;SessionManager 早于 AgentLoop 声明;三次回调安装位置保留 |
| D | model pool, session finalize, console control, notification, inbound submit, MCP status, update/auth/animation tasks | 每个 unique_ptr 仅在对应启动阶段构造,注册对象显式释放可重复调用 |
| E | clipboard, input context, router, input component, frame renderer, full-screen surfaces, root | Components 阶段最后装配;surfaces 的 owner slot 只在事件期间读取 |
| 退出根 | TuiShutdownSequence, LifetimeToken | run 的 ScopeExit 与析构共用同一个幂等序列;先 abort/wake/join,再 revoke 应用回调 |

正常退出、阶段返回 false、阶段异常和 Loop 异常均调用同一序列。尚未创建的资源通过拥有者是否存在判断,主会话落盘通过 start_session 成功标记判断。括号粘贴在 Loop 异常路径也由 ScopeExit 恢复。

逐阶段异常测试使用生产 TuiInitSequence/TuiShutdownSequence 与可注入 lifecycle,检查 worker 收尾和序列继续执行;真正终端和进程资源的验证留到统一 Windows 验收。SubagentHost 提前关停由 O-05 单独实现。
