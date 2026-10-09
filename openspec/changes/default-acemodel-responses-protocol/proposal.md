## Why

ACECode 已支持 Responses，但 ACEModel 新建模型和未声明协议的旧配置仍默认使用 Chat Completions。用户希望 ACEModel 默认使用 Responses，并能在模型设置中修改协议。

## What Changes

- ACEModel 的 OpenAI 运行时配置在未声明协议时默认使用 Responses；明确选择的协议优先。
- 模型目录提供默认协议，新增及批量新增模型使用该默认值，编辑继续使用已有的 API 协议选择框。
- Windows 安装器新建及升级缺省 ACEModel 配置时写入 Responses，保留已有协议选择。
- 普通 OpenAI 兼容配置继续使用原默认值；默认协议依据目录身份，独立于模型名称、URL 及能力设置。

## Capabilities

### New Capabilities

无。

### Modified Capabilities

- `web-model-management`: ACEModel 默认协议、设置修改、旧配置及安装器的一致性。

## Impact

涉及保存模型配置解析、Provider 工厂、模型目录及列表 API、前端模型草稿、Windows seeder 和相关测试。复用现有 Responses 实现；不改变认证、能力及上下文信息。本变更针对 ACEModel 覆盖 `add-openai-responses-protocol` 的统一缺省 Chat Completions 约定。
