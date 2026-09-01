# RC 会话命令首响与切换回执

id: rc-session-command-feedback
scope: ACECode 通用 remote-control 会话导航
status: ready
depends-on: []

## objective

保证 `/session`、`/sessions`、`/resume` 在耗时控制操作前保留即时
`思考中...` 首响，并把数字切换成功结果收敛为一条包含会话名和当前上下文
用量的中文回执。

## context

- `openspec/changes/add-rc-session-navigation/design.md`
- `openspec/changes/add-rc-session-navigation/specs/remote-control-session-navigation/spec.md`
- `docs/channel-plugin-protocol.md`

## path

- `src/remote_control/rc_session_navigation.*`
- `src/remote_control/session_channel_binder.*`
- `tests/remote_control/rc_session_navigation_test.cpp`
- `tests/remote_control/session_channel_binder_test.cpp`
- 本任务引用的 OpenSpec 与协议文档

## verification

- 首响在阻塞的 catalog/resume 工作完成前可见，且命令不进入 AgentLoop。
- 切换成功只产生一条最终成功回执，标题为空时回退 session id。
- 已知用量使用 `last_token_usage.prompt_tokens / context_window`。
- 无用量与无窗口分别返回明确 fallback 文案。
- 运行 RC 导航/绑定聚焦测试、`ChannelBoundaryGuard.*`、OpenSpec strict validation
  与 provider/company 标识扫描。
