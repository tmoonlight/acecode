## Why

用户已有独立部署的 SenseVoice 服务，需要在 ACECode 输入框直接听写，并能切换到其他语音识别接口。公司内网服务没有 HTTPS 证书，服务地址和端口必须可配置，客户端不捆绑 Python、模型或启动识别进程。

## What Changes

- 在设置的“工具”页增加“语音识别”，配置启用状态、协议、服务地址、模型、可选密钥和录音/请求限制。
- 支持现有 SenseVoice HTTP 接口、OpenAI 兼容转写接口和 whisper.cpp 接口，由 ACECode 后端转发音频。
- 已启用且配置完整时在发送按钮左侧显示听写按钮；录音时显示取消、实际音量波形、停止、发送。
- 停止仅插入可编辑文字；发送在成功识别后提交完整草稿；取消、失败、切换会话不能破坏草稿或回填到其他会话。
- 支持 HTTP 内网服务及自定义端口；区分识别服务传输与浏览器麦克风的安全上下文要求。

## Capabilities

### New Capabilities

- `speech-recognition`: 配置独立识别服务、录音生命周期、协议适配、草稿回填和错误处理。

### Modified Capabilities

无。

## Impact

涉及 base/config 持久化、adapters 下语音客户端、web 鉴权接口、设置工具页和共享 InputBar/RichComposer。沿用 cpr、React、Slate 和 Web Audio，不新增模型或 Python 运行依赖。更新 daemon API 文档、定向 C++/JS 测试与浏览器交互验证。保留当前检出中的其他未提交改动。
