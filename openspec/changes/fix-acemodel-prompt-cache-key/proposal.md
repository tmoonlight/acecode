# Proposal

## Why

会话 `20261018_065107_2e37` 的 32 次请求保持完整且稳定的消息前缀，但累计缓存命中率仅 20.43%。ACECode 只向官方域名发送缓存键，ACEModel 中转在缺少显式会话标识时按请求生成新的上游缓存身份，破坏了会话内的缓存连续性。

## What Changes

- 向已验证的 ACEModel Responses 和 Chat Completions 接口传递现有的会话级缓存键。
- 主会话、子会话分别使用自身的稳定标识，沿用显式参数拒绝后的按端点和模型回退。
- 保持其他自定义地址的兼容策略以及服务端隔离配置不变。
- 补充端点边界、会话隔离和协议转换测试，完成 Windows 构建及真实中转验证。

## Capabilities

### New Capabilities

- `provider-prompt-cache`: 已验证的模型接口使用稳定、按会话隔离的提示词缓存标识。

### Modified Capabilities

无。

## Impact

影响 OpenAI 兼容请求及 Responses 适配器的缓存能力判断，不新增配置或依赖，不改变 daemon API。需要使用包含修复的可执行文件；已有运行进程不会自动加载新代码。
