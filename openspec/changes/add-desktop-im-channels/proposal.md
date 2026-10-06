## Why

ACECode 的 daemon 已能托管 WhatsApp 通道，但配置只能靠终端向导，通道核心写死了 WhatsApp，而且每个联系人只能对应一个固定的无项目会话。用户希望在 Desktop 设置页里像 WorkBuddy 一样扫码接上 QQ、填 token 接上 Telegram，开关即用，并且能在 IM 里新建、切换会话，包括切到某个工作区的会话继续工作。

2026-10-06 用户要求参考 hermes-agent,把飞书、钉钉、微信、Discord、LINE 也接进同一套通道核心,让国内外常用的 IM 都能远程使用 ACECode。

## What Changes

- 设置页「集成」分组新增「消息通道」:QQ、微信、飞书、钉钉、Telegram、Discord、LINE 以两列卡片排列,卡片用单色聊天图标。「连接」在还没配好时打开三步向导(配置机器人 → 连接 → 绑定机主),配好的直接连接;连上后按钮变成「取消连接」。卡片只显示一行简要状态与待批准请求,连接原因、隐私模式、收发计数等细节写进 daemon 日志(2026-10-05 验收反馈)。
- 新增平台无关的通道核心：会话绑定、IM 内命令、访问控制与配对、出站投影、持久化、跨进程单实例归属。它和现有 WhatsApp 实现并存，WhatsApp 的代码、配置和终端向导都不改。
- QQ 官方机器人，C++ 原生实现：
  - 用手机 QQ 扫码完成配置，走腾讯 q.qq.com 的绑定任务协议，来源名 `acecode`；扫码失败时可以手动填 AppID/AppSecret。
  - 本机直连 WebSocket 网关，不需要公网地址。
  - 支持私聊和群里 @机器人。被动回复窗口用尽后降级为主动消息。
  - 用 Markdown 发送；图片、文件双向；语音消息使用腾讯自带的识别文字。
- Telegram Bot API，C++ 原生实现：
  - 填 BotFather 给的 token，用长轮询收消息。
  - 机主扫码或点一次性链接即自动绑定。
  - 支持私聊和群里 @机器人。HTML 格式化，按平台长度分段并遵守限速；图片、文件双向。
- 微信(腾讯 iLink 机器人接口),C++ 原生实现:
  - 用微信扫码登录,扫码的微信号自动成为机主;长轮询收消息,只支持私聊。
  - 文字与图片、文件双向;媒体经 CDN 用 AES-128-ECB 加解密。
- 飞书 / Lark,C++ 原生实现:
  - 填 App ID 与 App Secret(可选飞书或 Lark),本机直连长连接收事件,协议帧手写 protobuf 编解码。
  - 私聊与群里 @机器人;Markdown 用富文本(post)发送,图片、文件双向。
- 钉钉,C++ 原生实现:
  - 填 Client ID 与 Client Secret,Stream 模式本机直连收消息。
  - 回复优先走消息自带的会话回调地址,过期后改用机器人接口;私聊与群里 @机器人,图片、文件双向。
- Discord,C++ 原生实现:
  - 填 Bot Token,本机直连网关;私信与服务器频道里 @机器人,Markdown 原生,附件双向,显示“正在输入”。
- LINE Messaging API,C++ 原生实现:
  - 填 Channel ID 与 Channel secret 换取短期令牌。LINE 只支持公网回调:daemon 另起一个只监听本机的回调服务,
    由用户填写的公网地址或自动启动的 Cloudflare 临时隧道转发进来,每次连接都自动更新 LINE 的回调地址。
  - 私聊与群里 @机器人;回复令牌有效时用回复接口,否则推送;图片经回调服务的媒体地址发送,文件改发文字说明。
- 机主绑定:QQ 与微信由扫码带出;Telegram 用一次性链接;飞书、钉钉、Discord、LINE 由机主私聊机器人发送设置页给出的 6 位一次性绑定码。
- 会话模型：
  - 每个 IM 会话（私聊联系人，或群里的某个发言人）同一时刻绑定一个 ACECode 会话；第一条消息自动建无项目会话。
  - 提供命令 `/new` `/sessions` `/resume` `/model` `/status` `/stop` `/help` `/approve` `/deny` `/aq`。
  - 机主可以切到任意会话，包括工作区会话；其他联系人只能在自己的通道会话之间切换。
- 被绑定的会话在侧栏显示和 `/rc` 相同的电脑图标。在 Desktop 里往已绑定的会话输入时，回复照常同步到 IM。
- 网络请求跟随 ACECode 的代理设置。通道相关接口只允许本机访问。
- 依赖：
  - vcpkg 为 curl 打开 `websockets` 特性。
  - 新增 AES-256-GCM 解密：Windows 用 BCrypt，其他平台用 OpenSSL。
  - 新增 AES-128-ECB、MD5 与 HMAC-SHA256(同样 BCrypt / OpenSSL);WebSocket 客户端支持发送二进制帧。
  - 前端新增二维码渲染。
- 非目标（本期不做）：
  - WhatsApp 的任何改动，包括官方 Cloud API。
  - 企业微信、飞书与钉钉的扫码建应用(本期手填凭据);LINE 的长期隧道或公网部署(本期只提供用户自填公网地址与临时隧道两种方式)。
  - 语音转写或合成（QQ 自带的识别文字除外）。
  - 按钮式交互、流式输出、Telegram 话题多会话。
  - 定时任务推送到 IM、多账号。
  - Linux/macOS 实机验证（本期只验 Windows）。

## Capabilities

### New Capabilities
- `im-channels`: 平台无关的消息通道，包括设置页开关与状态、daemon 托管与跨进程归属、IM 会话与 ACECode 会话的绑定和切换、IM 内命令、访问控制与配对、出站投影、侧栏标识、代理与本机访问限制。
- `qq-bot-channel`: QQ 官方机器人接入，包括扫码配置与手动兜底、WebSocket 网关、私聊与群 @、被动回复窗口与降级、Markdown、媒体与语音识别文字。
- `telegram-channel`: Telegram Bot API 接入，包括 token 配置、机主绑定链接、长轮询、私聊与群 @、格式化分段限速、媒体收发。
- `weixin-channel`: 微信 iLink 机器人接入,包括扫码登录与机主、长轮询、私聊、媒体加解密。
- `feishu-channel`: 飞书 / Lark 接入,包括凭据校验、长连接帧协议与确认、私聊与群 @、富文本发送、媒体收发。
- `dingtalk-channel`: 钉钉接入,包括凭据校验、Stream 连接与确认、私聊与群 @、会话回调与机器人接口两种回复、媒体收发。
- `discord-channel`: Discord 接入,包括 token 校验与邀请链接、网关心跳与续连、私信与频道 @、分段与限速、附件。
- `line-channel`: LINE 接入,包括凭据与短期令牌、本机回调服务与签名校验、公网地址与临时隧道、回复与推送、媒体。

### Modified Capabilities
（无。WhatsApp 与 `/rc` 的行为保持不变。）

## Impact

- C++ 代码：
  - 新增 `src/host/channels/` 下的通用核心子目录。
  - 新增 adapters 模块 `src/adapters/im/`，承载 QQ 与 Telegram 的传输层。
  - 新增 `src/base/network/` WebSocket 客户端和 `src/base/platform/` AES-GCM 解密。
  - 改动 `src/apps/daemon/worker.cpp` 的装配；`src/apps/web/` 增加 REST、WS 事件，并在会话列表里标记已绑定的会话。
  - 同步更新 `src/layers.tsv`、`cmake/acecode_layer_libraries.cmake`。
- 前端：设置导航、通道设置组件及其纯逻辑模块、侧栏图标判定、i18n 目录。
- 依赖：`vcpkg.json` 增加 `curl[websockets]`（CI 依赖缓存会失效一次）；`web/package.json` 增加一个二维码库。
- 数据:`~/.acecode/channels/<平台>/`(qq、weixin、feishu、dingtalk、telegram、discord、line),凭据以私有权限保存。
- 外部服务：
  - q.qq.com 的绑定接口没有公开文档，可能变动。
  - QQ OpenAPI 与网关。
  - api.telegram.org。
  - ilinkai.weixin.qq.com(微信 iLink,接口无公开文档)、open.feishu.cn / open.larksuite.com、api.dingtalk.com / oapi.dingtalk.com、
    discord.com、api.line.me 与 Cloudflare 临时隧道(trycloudflare.com,官方不保证可用性)。
- 文档：`docs/channels.md`、`docs/daemon-api.md`、帮助站。
- 测试：C++ 单测用本地假网关和假 Bot API 服务器；前端用 Node 测试；Windows 本机构建验证，加真实账号验收。
