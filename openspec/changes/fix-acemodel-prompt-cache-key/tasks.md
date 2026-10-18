# Tasks

## 1. 缓存策略修复

- [x] 1.1 扩展已验证 ACEModel 两种接口的缓存键传递，完成端点、主/子会话、协议转换及兼容回退测试，并同步设计说明。

## 2. 集成验证

- [x] 2.1 Windows 增量构建并运行 provider 与前缀稳定性相关测试，执行 OpenSpec 严格校验和差异检查。
- [x] 2.2 构建可运行产物，完成真实 ACEModel 连续请求验证，记录缓存用量及运行实例加载状态。

## 验证记录

- Windows Release 构建成功；`AceModelPromptCache.*:OpenAiPromptCache.*:OpenAiResponsesProviderTest.*:OpenAiResponsesTest.*:AgentLoopTermination.RequestPrefix*` 共 71 项测试通过。
- OpenSpec 严格校验及本次代码差异的 `git diff --check` 通过。
- 真实 Responses 验证会话 `cache_key_smoke_20261018_183726`：三次请求输入分别为 12,537 / 12,567 / 12,597 token，缓存读取为 0 / 12,416 / 12,416；后两次命中率为 98.80% / 98.56%。服务端账本核对一致。
- 2026-10-18 18:39（Asia/Taipei）确认 Desktop 会话空闲后正常退出并重启，当前 Desktop PID 54204、daemon PID 30508，健康检查成功，daemon 使用本次构建的 `build/Release/acecode.exe`。
- 修复产物 SHA-256：`aa1106339c24c0d41948a047f0aa7341b0ba34960eab15fc28aac4fcaf6f0627`。旧产物保留为 `build/Release/acecode.before-cache-key-20261018.exe`；其他原有 TUI/独立 Web 进程未重启。
- 原有四个 Responses 脏文件的 SHA-256 与实施前一致，未覆盖其中的用户修改；服务器配置未改变。
