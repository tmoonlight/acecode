# 验收记录

日期：2026-10-11。范围：当前检出的内置消息通道 `/` 指令及 `/help`，未修改设置页 UI。

## 构建与静态检查

- `cmake --build build --config Release --target acecode_unit_tests --parallel 4`：通过。
- `openspec validate add-channel-mode-and-effort-commands --strict`：通过。
- 本次九个代码、测试与文档文件的 `git diff --check`：通过。
- `python scripts/layers/check_layers.py --strict`：全仓仍有两项与本次无关的工作区问题：`src/apps/desktop/web_host.cpp:73` 的 `web_host_focus.hpp` include 未解析；`src/host/session_host/thread_service.cpp` 为 1259 行，超过登记上限 1211。本次修改没有对应命中。

## 回归范围

使用 Windows Release 测试程序执行 `ChannelCommands.*:ChannelRouter.*:ImChannelsEndToEnd.*:SessionReasoningTest.*`。

最终结果：4 个测试套件共 55 项全部通过，耗时约 86 秒。

- 解析及路由：参数、别名、帮助、忙碌时执行、消息去重、绑定隔离、无绑定、无授权、会话删除、失败保留状态。
- daemon 生产装配及本机模拟 Telegram：三档权限切换与持久化、`/yolo` 放行正在等待的审批、effort 支持档位查询、更新与恢复预设、非法档位拒绝。
- 原有跨平台集成回归及 reasoning 忙碌更新、持久化失败回滚测试。
- 首轮 `/help` 回执等待超时：测试原先只等待权限状态更新就继续发送，未等限速队列中的回执；已改为逐条等待实际回复，保留失败时的回复快照用于诊断。

验证使用生产通道与会话实现、模拟平台服务器；未向真实聊天账号发送消息，未重启或替换用户正在运行的 Desktop/daemon。
