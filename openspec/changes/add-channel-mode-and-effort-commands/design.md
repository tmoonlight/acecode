## Context

内置通道共用 `channels/core/commands` 和 `Conversations`。权限与 effort 的真实状态由 `SessionRegistry` 管理：`set_permission_mode` 同步模式、清理许可缓存并在 yolo 时解决待审批；`set_reasoning_effort` 校验能力、替换后续请求使用的 provider，持久化失败回滚。

## Goals / Non-Goals

**Goals:** 复用现有会话接口及生命周期，保持各平台一致的命令行为与中文回执。

**Non-Goals:** 不增加全局默认设置，不修改权限引擎、模型能力规则或新会话默认权限；不改前端。

## Decisions

1. 在既有命令解析器新增 Mode、Effort 和 Yolo 类型。模式参数在解析层归一化；只接受目标三档和既有 auto 别名，拒绝多余参数。
2. `ConversationDeps` 注入权限设置、模型状态读取、effort 设置回调，由 daemon 组合根接入 registry。通道不直接操作 SessionEntry 内部状态，也不创建第二套权限规则。
3. `/effort default` 映射为空 optional；查询从 `SessionModelState` 读取实际值及启用的 `supported_efforts`。成功回执使用 registry 返回状态，失败时查询并展示现有能力。
4. 控制命令沿用先授权、去重、恢复绑定的顺序，在普通输入/技能展开前截获。无绑定时沿用提示，不悄悄建会话。
5. 用解析单测、路由假服务测试和 daemon 生产装配端到端测试分别验证语法、隔离以及真实 registry 行为。

## Risks / Trade-offs

- [用户在待审批时发送 yolo] → 沿用桌面端语义，立即允许挂起请求，帮助中明确跳过工具权限确认。
- [模型能力变化或持久化失败] → 由 registry 再校验并保留原设置，通道不宣称成功。
- [已安装 daemon 未更新] → 本次验证源码和测试产物，部署需要重新构建并启动对应版本。
