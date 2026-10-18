# Spec Delta

## Purpose

让支持提示词缓存标识的模型接口识别同一会话的连续请求，保留主会话和子会话各自的缓存身份，并在兼容网关拒绝扩展参数时维持正常调用能力，避免稳定提示词因缺少会话身份而重复计算。

## ADDED Requirements

### Requirement: ACEModel 请求携带会话缓存标识
系统 SHALL 对内置 ACEModel HTTPS 地址的 Responses 和 Chat Completions 请求发送当前会话的 `prompt_cache_key`，支持普通请求、流式请求以及完整 URL 配置。

#### Scenario: 同一会话连续请求
- **WHEN** 同一会话连续调用 ACEModel，包括工具结果返回后的请求
- **THEN** 请求携带相同且非空的缓存标识

#### Scenario: 子会话隔离
- **WHEN** 主会话与子会话交错使用同一个 provider
- **THEN** 每次调用携带所属会话的缓存标识，互不覆盖

### Requirement: 已验证端点边界
系统 SHALL 仅向已验证支持缓存键的端点启用该扩展；不得因模型名称、相似域名或包含 ACEModel 地址的路径而向其他地址启用扩展。

#### Scenario: 非标准端点
- **WHEN** 使用 HTTP、其他域名、非默认端口或不匹配的 API 路径
- **THEN** ACEModel 缓存扩展不启用，其他已有供应商策略保持原状

### Requirement: 显式拒绝后的兼容回退
系统 SHALL 在服务端明确拒绝 `prompt_cache_key` 时删除该字段后重试，并只对当前端点和模型记忆该回退；其他错误不得触发缓存参数降级。

#### Scenario: 当前模型不接受参数
- **WHEN** 端点明确返回缓存键不受支持
- **THEN** 后续请求对该端点和模型省略缓存键，切换模型或端点后独立判断
