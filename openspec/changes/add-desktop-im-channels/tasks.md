## 1. 基础设施

- [x] 1.1 `vcpkg.json` 显式加入 `curl[websockets]`，重新配置后检查 `curl-config` 的 `supported_protocols`，确认含 WS/WSS。
- [x] 1.2 `src/layers.tsv` 登记新模块 `src/adapters/im/`（`im`，adapters，rank 32），`cmake/acecode_layer_libraries.cmake` 写好新目录的归属。验证：`layer_lint`、`check_layer_libraries.py` 通过。
- [x] 1.3 实现 `src/base/network/websocket_client.*`（libcurl connect-only 加 `curl_ws_*`）。要求：代理走 `ProxyResolver`；Windows 上 NoRevoke；读操作带超时、可中止；处理 ping/pong 与 close；单帧大小有上限。验证：用本机 Crow WebSocket 回显服务跑的 `WebSocketClient*` 单测通过，覆盖收发、关闭、超大帧拒绝、中止读取、断线报错。
- [x] 1.4 实现 `src/base/platform/crypto/aes_gcm.*`（Windows BCrypt，其他平台 OpenSSL EVP）。验证：`AesGcm*` 单测通过，覆盖 NIST 测试向量、connector 格式（IV、密文、认证标签依次拼接）的样例、篡改后解密失败。

## 2. 传输接口与公共工具（adapters/im）

- [x] 2.1 定义 `im/transport.hpp`：统一的入站与出站类型、回复上下文、状态快照、平台能力声明。实现 `im/text_chunk.*`：按 UTF-8 安全切分，按 UTF-16 单位计数，优先在段落处切。验证：`ImTextChunk*` 单测覆盖中文、emoji、超长单行、空文本。
- [x] 2.2 实现 Markdown 工具：转纯文本（供 QQ 回退用）、转 Telegram HTML。验证：`ImMarkdown*` 单测覆盖代码块、行内代码、粗体、斜体、删除线、链接、标题、列表、`& < >` 转义、未闭合标记。
- [x] 2.3 实现凭据脱敏工具：生成带尾号的展示值，并清除 URL 和错误文本中的 `bot<token>`、AppSecret。验证：`ImRedact*` 单测通过。

## 3. QQ 传输

- [x] 3.1 实现扫码绑定流程：申请任务、2 秒一次轮询、解密、过期刷新、10 分钟超时、取消；读取返回结果中的 `user_openid`；请求头带 `Accept: application/json` 和 `User-Agent: ACECode/<版本>`；HTTP 层可注入。验证：`QqBind*` 单测用假响应覆盖成功、过期、取消、服务错误；捕获日志，确认其中没有密钥和二维码内容。
- [x] 3.2 实现 QQ OpenAPI 客户端，包括：
  - 令牌获取，并在到期前 5 分钟刷新；
  - 获取网关地址；
  - 单聊和群聊的文本、Markdown 发送；
  - 用 `/files` 上传，再以 msg_type 7 发送媒体；
  - 解析错误码。

  验证：`QqApi*` 单测在本机假 HTTP 服务上通过。
- [x] 3.3 实现网关状态机（纯逻辑，不碰 IO）：Hello / Identify / Resume、心跳间隔、seq 记录、op 7 与 op 9 的处理、关闭码分类（4004、4006/4007/4009、4008、4900–4913、4914、4915）。验证：`QqGatewayState*` 单测逐条覆盖。
- [x] 3.4 实现回复预算与补发：私聊 60 分钟最多 4 条，群聊 5 分钟最多 5 条；全局递增 `msg_seq`；“窗口已过”的错误码立即降级为主动消息；主动消息被拒时暂存（上限 20 条），下一条入站消息到来时先补发。验证：`QqReplyBudget*` 单测通过。
- [x] 3.5 组装 QQ 传输层：WebSocket、状态机、OpenAPI、回复预算、Markdown 失败回退、入站附件下载（25 MiB 上限）、语音使用识别文字、消息去重；只订阅单聊与群 @ 事件。验证：`QqTransport*` 端到端测试在 Crow 搭的假网关上通过，覆盖连接、收私聊与群 @、回复、断线后 Resume、4008 延迟重连、4915 停止重试。

## 4. Telegram 传输

- [x] 4.1 实现 Bot API 客户端：`getMe`、`getUpdates`、`sendMessage`（HTML 与纯文本）、`sendChatAction`、`getFile` 与文件下载、`sendPhoto` / `sendDocument`（multipart）、`deleteWebhook`；能区分 409 的 webhook 与被占用两种情况，并读出 429 的 `retry_after`。验证：`TelegramApi*` 单测在假 Bot API 上通过。
- [x] 4.2 实现更新解析：区分私聊与群聊；判定是否 @ 本机器人（entities 与 caption_entities 中的 mention、text_mention、`/cmd@bot`，以及回复机器人的消息）；提取话题 ID、`/start <码>`、照片和文件描述，并识别不支持的类型。验证：`TelegramUpdate*` 单测通过。
- [x] 4.3 实现限速器（纯逻辑）：同一聊天至少间隔 1 秒，同一个群每分钟最多 20 条，遇 429 按要求等待后重发且保持顺序。验证：`TelegramRateLimit*` 单测用假时钟通过。
- [x] 4.4 组装 Telegram 传输层，包括：
  - 长轮询，offset 持久化回调；
  - webhook 与被占用两种状态；
  - 忙碌期间每 4.5 秒发一次“正在输入”；
  - HTML 失败回退纯文本；
  - 分段；
  - 照片与文件收发，带 20 MB / 50 MB / 10 MB 上限。

  验证：`TelegramTransport*` 端到端测试在假 Bot API 上通过。

## 5. 通道核心（host/channels/core）

- [x] 5.1 实现每个平台的配置与状态存储：`config.json` 与 `state.json` 的结构校验、私有权限原子写、文件损坏时拒绝启动、脱敏快照、去重回执环、Telegram offset。验证：`ChannelStore*` 单测覆盖损坏拒绝、并发编辑、重启后读回。
- [x] 5.2 实现访问控制与配对，包括：
  - 默认拒绝；
  - 待批准请求 10 分钟过期；
  - 机主规则：第一个获批者成为机主；QQ 扫码返回的 openid 直接成为机主；扫码后 10 分钟内第一个私聊者成为机主；Telegram 的一次性 start 码；
  - 群与群成员分别审批；
  - 撤销授权。

  验证：`ChannelAccess*` 单测通过。
- [x] 5.3 实现命令解析与权限：`/help` `/status` `/stop` `/new` `/sessions [more|search]` `/resume` `/model` `/approve` `/deny` `/aq`；按机主或普通联系人决定可用命令；忙碌时拒绝 `/new` 与 `/resume`。验证：`ChannelCommands*` 单测通过。
- [x] 5.4 实现会话绑定路由：
  - 首条消息新建无项目会话（`default` 权限，不继承危险标志）；
  - `/new` 跟随当前绑定会话的位置；
  - 机主用会话目录选择，非机主只能看自己创建过的会话，编号快照只对最近一次列表有效；
  - 跨 IM 会话转移绑定，旧代次的事件丢弃；
  - 绑定的会话已删除时明确报错；
  - 普通文本像 Web 输入一样展开技能命令。

  验证：`ChannelRouter*` 单测用内存版 `SessionClient` 通过。
- [x] 5.5 实现出站投影：每个绑定一个有界队列和一个提问桥；投递最终助手文本；按回合合并（供 QQ 用）；发送前重新校验附件路径；投影权限请求和提问，先答者生效，另一端收到已处理提示；统计失败与丢弃；Desktop 输入的回复同步发往 IM。验证：`ChannelOutbound*` 单测通过。
- [x] 5.6 实现平台宿主生命周期：
  - 开关即时连接与断开；
  - 每平台一把归属锁，取锁失败进入待命，持有者退出后接管，`owner.json` 记录 PID；
  - 临时故障递增退避重试，致命错误停止重试；
  - 状态变化通知；
  - 重启后自动连接已开启的平台；
  - 统筹 QQ 扫码流程、Telegram 机主链接与移除 webhook。

  验证：`ChannelHost*` 单测通过，其中包括两个宿主在临时目录里争用同一把锁。

## 6. daemon 与 Web 接口

- [x] 6.1 在 `worker.cpp` 装配通道宿主：接入 `SessionClient`、权限请求快照、会话 cwd、`switch_model`、会话目录、恢复会话、技能展开、WS 广播；关闭序列增加一步，先停止接收入站消息再释放。验证：`daemon_shutdown_sequence` 相关测试通过；daemon 在通道全部关闭时正常启动、正常退出。
- [x] 6.2 实现 `routes_channels.cpp` 与 `handlers/channels_handler.*`：所有接口只接受本机来源；凭据先校验再保存；返回值脱敏；推送 `channels_state` / `channels_bind` / `channels_request` 三类 WS 事件；会话列表加 `channel_bound` 字段。验证：handler 单测通过；`web_server_smoke_test` 覆盖非本机请求返回 403、凭据脱敏、开关往返（使用假传输层）、`channel_bound` 标记。
- [x] 6.3 更新 `docs/daemon-api.md` 的通道接口与事件说明，并检查与实现一致。

## 7. 前端

- [x] 7.1 新增 `qrcode-generator` 依赖。实现 `web/src/lib/channelsSettings.js`：快照规整、状态文案、按钮可用性、扫码流程状态、WS 事件合并。验证：Node 测试登记到 `runTests.js`，`pnpm test` 通过。
- [x] 7.2 实现 `components/ChannelsSettings.jsx`，并把「消息通道」加入设置导航。样式按 `acecode-frontend-style`，包含：
  - 两张平台卡片；
  - 扫码与二维码，以及手动填写；
  - Telegram 的 token 输入、机主二维码、隐私模式提示；
  - 授权名单、待批准请求、已绑定会话；
  - Desktop 后台服务提示，以及 WhatsApp 的说明。

  新增中文文案先补 `i18n-en-overrides.mjs`，再执行 `pnpm i18n:catalog`。验证：`pnpm test`、`pnpm build` 通过。
- [x] 7.3 实现以下两项：
  - App 层监听 `channels_request`，经 `desktopNotify` 弹系统通知；
  - 侧栏在 `channel_bound` 时显示 `/rc` 同款电脑图标，悬停提示来自哪个平台，`/rc` 的专属行为仍只看 `remote_control_bound`。

  验证：侧栏图标判定与通知抑制的 Node 测试通过。

## 8. 文档与验证

- [x] 8.1 更新文档：`docs/channels.md` 增加 QQ 与 Telegram 章节（配置、命令、授权、限制）；同步帮助站对应页面；在 `CLAUDE.md` 记录实现要点。验证：文档与实际行为逐项核对一致。
- [x] 8.2 在 Windows 构建 `acecode`、`acecode-desktop`、`acecode_unit_tests`；运行通道、远程控制、会话相关测试和前端测试；把结果记录到 `verification.md`。
- [ ] 8.3 在 Windows 上和用户一起做真实账号验收，结果与限制逐项记入 `verification.md`。
- [x] 8.5 按验收反馈简化设置页（design D12）：两列卡片，单色聊天图标，「连接 / 取消连接」按钮，未配好时走三步向导；页面只留简要状态与待批准请求，细节由 `core/state_log` 等写进 daemon 日志；更正 Telegram 群隐私模式的说明（开启时 @ 消息照常送达）。验证：`ChannelStateLog*`、`ChannelAccess*` 单测与前端 `channelsSettings.test.js`（含“页面不展示细节”的守卫）通过；通道相关套件、所有权与分层检查、`pnpm test`、`pnpm build` 通过。

  QQ：
  - `source=acecode` 扫码；
  - 私聊；
  - 群 @；
  - 在 IM 中审批，以及与 Desktop 的先答者生效；
  - 超出回复窗口后的降级与补发；
  - 图片和文件双向；
  - 语音识别文字。

  Telegram：
  - token 校验；
  - 扫码绑定机主；
  - 私聊；
  - 群 @，以及隐私模式提示；
  - 媒体收发。

  通用：
  - `/new` 与 `/sessions` + `/resume` 切到工作区会话；
  - 非机主切换受限；
  - 侧栏图标随绑定移动；
  - 开关的即时性；
  - Desktop 退出时，“退出后保留后台服务”开与关两种情况的表现；
  - 现有 WhatsApp 不受影响。
- [x] 8.4 运行 `openspec validate add-desktop-im-channels --strict` 与 `git diff --check`，确认都通过。

## 9. 更多平台(微信、飞书、钉钉、Discord、LINE)

- [x] 9.1 平台描述表 `core/platforms.*` 与 `core/platform_adapters.*`:运行时、宿主、Web 接口改为按描述表工作;扫码与机主接口改为按平台路由(`/api/channels/<平台>/bind`、`/owner-link`);可选凭据字段可清除。验证:通道相关套件全部通过。
- [x] 9.2 6 位一次性绑定码与防猜测(`AccessControl::issue_owner_pin`)。验证:`ChannelAccess*` 单测。
- [x] 9.3 新增 `platform/crypto/aes_ecb`、`digest`(MD5、HMAC-SHA256),`WebSocketClient::send_binary`。验证:`CryptoDigest*`、`CryptoAesEcb*` 标准向量单测。
- [x] 9.4 微信传输层与扫码登录(`src/adapters/im/weixin/`)。验证:协议、登录、传输层单测在假 iLink 服务上通过。
- [x] 9.5 飞书传输层(`src/adapters/im/feishu/`,含 protobuf 帧编解码)。验证:帧编解码字节向量、传输层单测在假服务上通过。
- [x] 9.6 钉钉传输层(`src/adapters/im/dingtalk/`)。验证:Stream 帧、回复路由、传输层单测在假服务上通过。
- [x] 9.7 Discord 传输层(`src/adapters/im/discord/`)。验证:网关状态机、传输层单测在假网关上通过。
- [x] 9.8 LINE 传输层、本机回调服务与 cloudflared 隧道(`src/adapters/im/line/`、`core/line_webhook_server.*`)。验证:签名校验、回调服务、隧道状态与传输层单测通过。
- [x] 9.9 核心接线:五个平台的传输层创建、凭据校验、微信扫码流程接入 `platform_adapters.cpp`;端到端测试覆盖每个平台的连接、私聊、机主绑定码。
- [x] 9.10 设置页:七张卡片、按平台定义的三步向导(扫码 / 填写凭据 / 连接后清单 / 绑定码),i18n 目录。验证:`channelsSettings.test.js`、`pnpm test`、`pnpm build`。
- [x] 9.11 文档:`docs/channels.md`、`docs/daemon-api.md`、帮助站、`CLAUDE.md` 更新为七个平台。
- [x] 9.12 Windows 构建与全量相关测试、所有权与分层检查;结果记入 `verification.md`。
