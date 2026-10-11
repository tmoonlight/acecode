## Why

消息通道目前只能切模型、逐次批准工具，无法像桌面端一样调整当前会话的权限模式和思考深度。用户需要在 IM 中直接切换默认、自动、完全审批模式，并用 `/yolo` 一步开启完全访问权限。

## What Changes

- 增加 `/mode [default|auto|yolo]`，查询或切换当前绑定会话的权限；`accept-edits` 兼容为 `auto`。
- 增加 `/yolo`，直接切换为 `yolo`，沿用现有完全访问权限语义。
- 增加 `/effort [档位|default]`，查询当前值与模型支持的档位，或设置思考深度；`default` 恢复模型预设。
- 帮助和状态回复展示新命令、当前权限及 effort；命令在忙碌时立即处理，不进入模型上下文。

## Capabilities

### New Capabilities

- `channel-session-controls`: 内置消息通道的会话权限与 effort 指令、回执和错误语义。

### Modified Capabilities

无。内置消息通道原规范仍在 `add-desktop-im-channels` 变更中，主规范尚未收录。

## Impact

修改 `channels/core` 的解析和路由、daemon 的会话服务装配、对应 C++ 测试及 `docs/channels.md`。复用 `SessionRegistry` 的权限和 reasoning 接口，保持默认新会话权限、授权检查、绑定隔离和消息去重。
