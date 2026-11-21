## Why
用户遇到额度耗尽时可能持续重连，需要在当前回合收到可采取行动的错误。
## What Changes
- 区分钱包、独立 Key、上游窗口与临时限流。
- 为 ACEModel 限流提供有界重试及可读错误。
## Impact
影响 ACEModel 错误响应及 ACECode 的 OpenAI 兼容流式对话。
