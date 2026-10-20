# 语音识别

ACECode 的 Desktop 和 Web 输入框支持听写。识别由独立服务完成，ACECode 不包含 Python、模型或推理运行时，也不会自动下载模型。

## 配置

进入 **设置 → 工具 → 语音识别 → 配置**，选择协议、填写服务地址，再打开开关。配置自动保存，没有独立保存按钮：开关和选项即时保存，文本和数字输入在失焦或按 Enter 后保存，收起或离开设置也会补齐更改。失败保留输入并提供重试。未配置或关闭时，输入框不显示麦克风。

「测试连接」会先等待当前更改保存成功，再提交一秒静音 WAV 验证服务，不使用麦克风。

| 协议 | 服务地址示例 | 模型配置 | 实际请求 |
| --- | --- | --- | --- |
| SenseVoice | `http://192.168.1.50:8765` | 在独立服务中加载 SenseVoice 模型 | `/api/transcriptions`，multipart 字段 `audio` |
| OpenAI 兼容 | `http://192.168.1.50:8000/v1` | 填写服务提供的模型 ID，例如 `whisper-1` | `/v1/audio/transcriptions`，字段 `file`、`model` |
| whisper.cpp | `http://192.168.1.50:8080` | 在 whisper.cpp 服务启动参数中选择模型 | `/inference`，字段 `file` |

这里的 SenseVoice 协议对应独立 Python 服务约定：返回 `{"status":"ok","text":"识别文字"}`，静音返回 `{"status":"no_speech","text":""}`。其他 SenseVoice 服务若采用 OpenAI 兼容接口，应选择对应协议。服务地址填接口前缀，不要填写完整识别路径；OpenAI 兼容协议会在没有 `/v1` 后缀时自动补上。

旁人说话的过滤由识别服务负责：配套的独立 SenseVoice 服务会丢弃比说话人轻很多的语音片段（默认低 8 dB），阈值通过服务端环境变量 `SPEECH_SEGMENT_GATE_DB` 调整，ACECode 不提供对应设置。

API Key 可选，填写后通过 `Authorization: Bearer …` 发送。高级设置可调整最长录音（1–300 秒，默认 60 秒）、请求超时（5–600 秒，默认 120 秒），以及是否使用 ACECode 代理。内网默认直连。界面不提供语言选项，默认自动识别；SenseVoice 的语言由服务端控制。

## 内网 HTTP 和端口

识别服务可以使用普通 HTTP，不需要 HTTPS 证书。端口直接写在服务地址中，可以修改；例如 Mac mini 上服务监听 `0.0.0.0:9876`，ACECode 填 `http://Mac-mini的内网IP:9876`。允许该端口入站，并确认 ACECode 后端所在电脑能够访问它。

音频先提交给 ACECode 后端，再由后端调用识别服务，因此浏览器无需直接连接 Mac mini，也无需给识别服务配置浏览器 CORS。

**麦克风权限取决于打开 ACECode 的页面环境。** 浏览器只给 HTTPS 页面和本机地址（`localhost`、`127.0.0.1`）提供麦克风，这是浏览器的规则，服务端无法绕过。Desktop 的本机页面、浏览器的 `http://localhost:端口` 或 HTTPS 页面可以直接录音；在浏览器里打开另一台电脑的 `http://内网IP:端口`（例如远程 Web），需要先让浏览器把这个地址当作安全来源，见下一节。系统和浏览器仍需允许麦克风权限。

### 内网 HTTP 页面使用语音

不想配置证书时，可以保持 HTTP，只在浏览器里把 ACECode 的地址加入「安全来源」名单。地址要写完整来源：`http://` + 主机 + 端口，例如 `http://192.168.1.20:28080`，不带路径和 `?token=`。在这类页面上点麦克风，ACECode 会按当前浏览器显示具体步骤，并提供「复制本页地址」按钮（地址栏通常会隐藏 `http://`，手抄容易漏）。

| 浏览器 | 单台设置 | 公司统一下发 |
| --- | --- | --- |
| Chrome | 打开 `chrome://flags/#unsafely-treat-insecure-origin-as-secure`，填入地址，选 Enabled，重启浏览器 | 策略 `OverrideSecurityRestrictionsOnInsecureOrigin`（Chrome 69 起）；Windows 注册表 `HKLM\SOFTWARE\Policies\Google\Chrome\OverrideSecurityRestrictionsOnInsecureOrigin`，字符串值名 `1`、`2`…，值为地址 |
| Edge | 打开 `edge://flags/#unsafely-treat-insecure-origin-as-secure`，其余同 Chrome | 同名策略（Edge 77 起，组策略显示名「Control where security restrictions on insecure origins apply」）；注册表 `HKLM\SOFTWARE\Policies\Microsoft\Edge\OverrideSecurityRestrictionsOnInsecureOrigin` |
| Firefox | 打开 `about:config`，把 `media.devices.insecure.enabled` 和 `media.getusermedia.insecure.enabled` 都设为 `true`，重启浏览器 | — |
| Safari / iPhone、iPad 上的浏览器 | 没有可用的设置，请改用 HTTPS，或在电脑上使用 Chrome、Edge、Firefox | — |

多个地址在 flags 里用英文逗号分隔。策略修改后要重启浏览器，可在 `chrome://policy` / `edge://policy` 确认已生效。这个名单会让浏览器对该地址放开所有只限安全页面的能力，只填受信任的内网 ACECode 地址；服务器 IP 或端口变化后需要同步更新。

普通浏览器首次使用时由浏览器弹出原生麦克风授权。原生 Desktop 对 ACECode 自身页面直接处理网页麦克风权限，不额外弹网页确认；操作系统的隐私授权仍需允许（例如 macOS 首次运行的系统提示）。外部网页、摄像头等权限保持原有行为，Edge app 降级模式仍走浏览器授权。

浏览器限制参见 [MDN getUserMedia 安全上下文说明](https://developer.mozilla.org/en-US/docs/Web/API/MediaDevices/getUserMedia#privacy_and_security)；whisper.cpp 的服务端参数和接口见[官方服务文档](https://github.com/ggml-org/whisper.cpp/blob/master/examples/server/README.md)。

## 输入框操作

- 点击麦克风开始录音，底部显示实际音量波形。
- 悬停提示“右键配置麦克风”；右键可选择当前电脑的实际音频输入设备，选项即时保存。首次获取完整设备列表可能需要麦克风授权，菜单获取设备名称后立即释放临时音频流。
- 默认 `Ctrl+D` 开始／停止录音，停止后识别回填。在 **设置 → 快捷键 → 开始／停止录音** 可修改；快捷键录入、其他编辑区域和弹窗不会误触录音。
- 右键菜单的“按住录音”默认关闭。开启后，按住麦克风或录音快捷键开始，松开即停止并识别；尚未获得权限就松开时取消等待。设备选择与按住偏好仅保存在当前电脑的浏览器/桌面环境中。
- 点击停止方块：结束录音并把识别文字插入原光标位置，保留已有草稿和附件标签。
- 点击发送箭头或按发送快捷键：识别后发送合并后的完整草稿一次。
- 点击取消或按 Escape：丢弃本次录音，保留原草稿。
- 达到录音时长上限时自动停止并回填，不自动发送。

开始、等待权限、识别和取消使用同一工具栏空间，不改变输入框外框尺寸；识别文本新增导致的正常文本换行仍会扩展编辑区。

识别失败或没有识别到语音时不会发送消息。切换会话、关闭输入框或改变配置会取消本次听写，迟到结果不会插入其他会话。音频使用内存中的单声道 16 kHz PCM16 WAV（降采样前先滤除 8 kHz 以上的成分，避免键盘声、嘶声等高频噪声混入语音频段），不写入 ACECode 的会话历史或临时音频文件；独立识别服务的存储行为由其自身配置决定。

## 验证边界

浏览器自动化使用模拟麦克风验证真实 WebAudio 采集、WAV 编码和输入框交互；不能代替真实设备授权验证。macOS 的麦克风用途声明已接入，仍需在 macOS 上做真实设备验收。

Linux Desktop 由系统 WebKitGTK 经 GStreamer 采集声音，与 Windows / macOS 一样只对 ACECode 自己导航到的那个本机页面放行纯音频采集。2026-10-20 在 WSL 的 Ubuntu 22.04（WebKitGTK 2.50.4）上用生产权限代码和 WebKitGTK 模拟采集设备实测：可信页面能录到声音，别的端口的本机页面被拒绝；WSLg 的远程桌面声源不出数据时 `getUserMedia` 会一直等待，那是该环境没有接入麦克风。编译时没有启用媒体流的老版本 WebKitGTK 无法网页录音，Desktop 内会提示升级系统的 WebKitGTK。真实麦克风尚未在 Linux 实机验收。

Windows 原生权限回归：构建 `microphone_permission_smoke`，运行 `python tests/desktop/microphone_permission_smoke.py build/tests/Release/microphone_permission_smoke.exe`。该测试使用生产 WebHost、独立 WebView2 用户目录和模拟音频设备，不启用自动接受权限提示的测试开关，验证首次及再次采集。
