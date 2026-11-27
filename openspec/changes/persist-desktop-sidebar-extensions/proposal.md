## Why

Desktop 重新启动 daemon 时可能分配新的本机端口。“扩展 → 自定义”把固定项和顺序保存在按端口隔离的 localStorage 中，换端口后侧栏会恢复默认布局。

## What Changes

- Desktop 将扩展顺序和固定项保存到用户数据目录，重新打开时恢复。
- 首次使用时沿用当前来源仍能读取的旧偏好，普通浏览器继续使用自己的布局。

## Capabilities

### Modified Capabilities

- `webui-custom-sidebar` 增加桌面侧栏扩展布局的恢复约定。

## Impact

涉及侧栏扩展组件、daemon 的桌面状态接口及相关测试。
