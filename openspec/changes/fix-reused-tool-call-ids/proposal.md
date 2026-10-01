## Why

部分兼容服务在连续响应中复用 `call_0`。ACECode 将跨消息复用误判为同一调用重复，删除后续工具调用及成功结果，只留下 assistant 正文，导致上游以最后一条消息不能是 assistant 为由返回 HTTP 400。

## What Changes

- 区分同一 assistant 工具数组的重复项与后续 assistant 消息中的 ID 复用。
- 对跨消息冲突的调用 ID 与对应结果进行确定性重映射，保留真实输出以及缺失结果的中断语义。
- 保证多次历史投影幂等、原始消息不变、新 ID 不与已有或后续原始 ID 冲突。
- 覆盖共享历史修复和 OpenAI / Anthropic 请求构造的回归测试。

## Capabilities

### New Capabilities

- `provider-tool-call-identity`: 提供者历史投影中工具调用 ID 的唯一性与结果配对。

### Modified Capabilities

无。现有 `session-storage` 的原样持久化约定保持；未归档的 `harden-openai-tool-history` 对同一数组重复项去重的约定保持。

## Impact

影响 `src/domain/session/session_history_recovery.*` 及相关测试。Agent 请求、恢复、压缩及两个提供者的现有共同入口自动使用修复，不新增网络依赖或修改磁盘会话、UI、API 字段。
