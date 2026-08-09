## Why

ACECode 现有“新建专家”入口直接打开完整编辑器，要求用户先理解专家包字段，和已经内置的 `expert-manager` 对话式创建能力脱节。默认入口应让普通用户在真实聊天中逐步补充信息，同时保留原编辑器供需要精确配置的用户使用。阿斯顿

## What Changes

- 将专家组件页现有“新建专家”操作改为分段按钮：主操作保留现有名称、视觉尺寸和主按钮地位，右侧独立下拉箭头打开高级选项。
- 点击主操作时直接进入当前工作区的真实新任务对话，并在输入框中预置未发送的 `/expert-manager`  Skill token；不自动发送消息，也不在专家页新增通用聊天入口。
- 下拉菜单只提供“高级模式”；选择后继续打开现有的新建专家编辑器，保留原高级创建能力。
- 为菜单的关闭、键盘和语义行为以及对话草稿交接补充回归测试。

## Capabilities

### New Capabilities

- `expert-conversational-creation`: 专家页默认对话式创建入口、未发送的 Expert Manager Skill 预置，以及通往原专家编辑器的高级模式分流。

### Modified Capabilities

无。

## Impact

- WebUI：`ExpertComponentsPage` 的新建入口、`App` 到真实新任务 composer 的导航交接，以及相关 i18n 目录。
- 复用能力：现有 `expert-manager` Seed Skill、home composer 草稿消费和原 `ExpertEditor`；不新增后端 API 或会话数据字段。
- 验证：Web 架构/交互测试、完整 Web 测试与生产构建、OpenSpec 严格校验。

 