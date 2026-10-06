## Context

动机见 proposal.md，行为要求见 `specs/` 下的三个能力。与实现方案直接相关的现状如下。

- **WhatsApp 通道**（`src/host/channels/`）：已在 master 上。`state.cpp` / `gateway.cpp` / `runtime.cpp` / `bridge.cpp` 多处写死了 WhatsApp，包括号码格式校验、会话键前缀、目录 `channels/whatsapp`、Baileys 桥。它由 daemon worker 托管（`worker.cpp` 构造 `channels::Runtime`），配置只能用终端向导，并且只在启动时读一次。本变更不改动这部分。
- **可复用的现有能力**：
  - 会话侧：`SessionClient`（create / resume / subscribe / send_input / respond_permission / respond_question / abort）、`SessionRegistry::switch_model`。
  - `/rc` 远程控制：跨工作区会话目录 `build_rc_session_catalog` 与 `rc::resume_session_target_exact`；出站有界队列 `rc::RemoteControlHub`；提问桥 `rc::ChannelQuestionBridge`；事件分类 `rc::classify_session_event`；UTF-8 安全分段 `rc::chunk_rc_session_output`。
  - 基础设施：跨进程 OS 文件锁 `channels::OwnerLock`；原子写 `atomic_write_file(path, content, /*restrict_permissions*/true)`；代理解析 `ProxyResolver::effective(url)`。
- **Desktop**：只运行一个共享 daemon，服务所有工作区。“退出后保留后台服务”由 `DaemonPool::set_keep_alive_on_exit` 控制。
- **侧栏图标**：侧栏的电脑图标来自会话列表里的 `remote_control_bound` 字段。它在 `server_helpers.cpp` 中按 `cfg.remote_control.bound_session_id` 计算，并会附带触发 `/rc` 专属的界面行为。
- **依赖**：vcpkg 的 curl 是 8.19.0，当前启用的特性是 `core;non-http;ssl;sspi`，没有开 WebSocket；端口本身提供 `websockets` 特性，对应 `CURL_DISABLE_WEBSOCKETS`。仓库里没有 WebSocket 客户端库，也没有 AES-GCM 实现。
- **参照实现**：
  - Hermes：`gateway/platforms/qqbot`、`plugins/platforms/telegram`。
  - WorkBuddy 5.6.2：解包 `app.asar`，读了 `main/server.js` 里的 `QQBindService`、`QQBotClient`、`QQBotApi`，以及腾讯官方的 `@tencent-connect/qqbot-connector` 1.1.0。该包没有开源许可，只参考协议，不复制代码。

## Goals / Non-Goals

**Goals:**
- 平台无关的通道核心与具体平台的传输层彻底分开：新增一个平台，只需实现传输接口并补一张设置卡片。
- QQ 与 Telegram 全部用 C++ 原生实现，不需要 Node 或 Python。
- 核心状态机（网关会话、回复预算、限速、格式转换、命令解析、访问控制）是不碰 IO 的纯逻辑，可以离线单测。
- 本期只在 Windows 上做构建与实测；代码保持三平台可编译，CI 照常构建。

**Non-Goals:**
- 把 WhatsApp 迁到新核心。后续单独立变更，见 Migration Plan。
- `/rc` 远程控制的任何改动。
- Telegram 托管机器人（像 Hermes 那样运营一个管理机器人）、QQ 频道、语音合成、流式消息编辑、按钮交互。

## Decisions

### D1. 新核心与 WhatsApp 并存，不重构旧代码
新核心放在 `src/host/channels/core/`，和现有的 `gateway.*` / `runtime.*` 等文件并列，互不调用。WhatsApp 的配置目录、归属锁、终端命令一律不动。
- 备选：现在就把 WhatsApp 改到新核心上。否决的原因有三：用户要求“先不动”；它从未做过真实账号验收；一旦改坏，回归不可见。
- 代价：短期内有两套相似的绑定和投影逻辑。新核心把 WhatsApp 网关里的出站投影模式（每个绑定一个 `RemoteControlHub` 加一个 `ChannelQuestionBridge`）抽成可复用组件，以后迁移 WhatsApp 时直接套用。

### D2. 分层与模块位置

| 位置 | 内容 |
| --- | --- |
| `src/base/network/websocket_client.*` | 基于 libcurl 的通用 WebSocket 客户端：`CURLOPT_CONNECT_ONLY=2` 加 `curl_ws_send` / `curl_ws_recv`。代理沿用 `ProxyResolver`，Windows 上沿用 NoRevoke。读操作带超时并可中止。限制单帧大小，处理 ping/pong 与 close 帧。 |
| `src/base/platform/crypto/aes_gcm.*` | 只做 AES-256-GCM 解密。Windows 用 BCrypt，其他平台用 OpenSSL EVP。 |
| `src/adapters/im/`（新模块） | 平台传输层，不了解 ACECode 会话。包括统一传输接口 `im/transport.hpp`、`im/qqbot/*`、`im/telegram/*`，以及公共的文本分段与 Markdown 处理。 |
| `src/host/channels/core/` | 通道核心：宿主与生命周期、会话绑定路由、IM 内命令、访问与配对、配置与状态存储、出站投影。 |
| `src/apps/web/routes/routes_channels.cpp` + `handlers/channels_handler.*` | REST 与 WS 事件，校验与 JSON 整形写成纯函数。 |
| `src/apps/daemon/worker.cpp` | 装配，并在关闭序列里加一步。 |
| `web/src/lib/channelsSettings.js` + `components/ChannelsSettings.jsx` | 设置页，逻辑写在纯函数里。 |

- 新增 adapters 模块需要在 `src/layers.tsv` 增加一行：`src/adapters/im/`，模块名 `im`，rank 32。
- 传输层只依赖 base，核心依赖 adapters/im、domain/session 与 host/remote_control，方向都向下。
- `cmake/acecode_layer_libraries.cmake` 中，传输层归 `acecode_adapters`，核心归 `acecode_host`。
- 备选：传输层也放进 `host/channels`，像 WhatsApp 的 bridge 那样。否决的原因：分层文档规定外部服务集成属于 adapters；放进 adapters 后，传输层不能引用会话，解耦由编译器保证。

### D3. 传输接口
`im::Transport` 是统一的传输接口：
- **生命周期**：`start(callbacks)` / `stop()`，以及 `status()` 快照，状态取值为 `connecting` / `connected` / `retrying` / `failed`，附原因，并标明是否已停止重试。
- **入站**：入站消息统一为 `im::Inbound`，字段包括：会话地址（平台、账号、聊天类型、聊天 ID、发言人 ID）、消息 ID、文字、是否 @、引用、附件描述（含语音识别文字）、平台原始的回复上下文。
- **出站**：`send_text(address, text, reply_context)`、`send_file(address, path, name, mime, reply_context)`、`set_typing(address, on)`、`download(attachment) -> 本地临时文件`。
- **能力声明**：每个平台声明 `max_text_units`、`batch_turn_output`、`supports_typing`、`max_upload_bytes` 等，核心据此决定分段与是否按回合合并。

回复预算、限速、格式转换都由各传输层内部完成，核心不需要知道 QQ 的被动回复规则。

### D4. 跨进程归属：每个平台账号一把锁
- **锁文件**：`~/.acecode/channels/<platform>/owner.lock`，复用 `channels::OwnerLock`。
- **取锁**：平台开关打开后才去取锁。取锁失败时，平台进入“由其他 ACECode 进程托管”状态，每 5 秒重试一次，持有者退出后自动接管。持有者把自己的 PID 写进 `owner.json`，供页面显示。
- **释放**：关闭开关或进程退出时，先停传输层、退订会话，再释放锁。
- **Telegram 的额外检测**：平台返回 409（另一个程序在轮询）时，也按“被占用”处理，这样可以覆盖 ACECode 以外的程序。
- 备选：所有平台共用一把锁。否决的原因：QQ 和 Telegram 应当能分别由不同进程托管，单个平台故障也不应牵连另一个。

### D5. 配置与状态存储
每个平台一个目录：`~/.acecode/channels/qq/`、`~/.acecode/channels/telegram/`。

| 文件 | 内容 | 写入时机 |
| --- | --- | --- |
| `config.json` | 开关、凭据、机主、已批准的联系人与群、平台选项 | 只在设置页的显式操作时写，私有权限原子写入 |
| `state.json` | 会话绑定、每个 IM 会话创建过的会话列表、去重回执环（4096 条）、Telegram 的 update offset | 运行时写入 |
| 内存，不落盘 | 配对请求（10 分钟过期）、机主绑定码、QQ 暂存待补发的输出 | — |

- **文件损坏**：一律拒绝启动该平台，并在页面显示错误，不静默重置。这与 WhatsApp 的做法一致。
- **会话地址键**：用结构化的 JSON 数组，例如 `["qq",app_id,"group",group_openid,member_openid]`、`["telegram",bot_id,"private",user_id]`。群成员地址里带上群 ID，因为 QQ 的群成员身份按群区分。
- **接口返回**：只给出凭据的脱敏值（例如 `****abcd`）。日志与错误文本统一经过脱敏函数，Telegram 的 `bot<token>` URL 片段同样处理。

### D6. 会话绑定与切换
- **首条消息**：调用 `create_session(no_workspace=true, permission_mode="default", inherit_dangerous_mode=false)`，与 WhatsApp 一致。先写入绑定，再提交输入。
- **`/new`**：读取当前绑定会话的工作区（cwd / workspace_hash），在同一位置新建；当前是无项目会话时，就新建无项目会话。
- **`/sessions`、`/resume`**：
  - 机主：复用 `build_rc_session_catalog`，覆盖活跃会话、已登记工作区和无项目会话，排除已归档会话与子代理会话；选中后用 `resume_session_target_exact` 恢复。
  - 非机主：只能看到 `state.json` 中记录的“本 IM 会话创建过的会话”。
  - 列表结果按 IM 会话保存快照，编号只对最近一次列表有效。
- **忙碌判断**：用 `SessionInfo.busy`。会话忙碌时拒绝 `/new` 与 `/resume`，避免旧会话在后台无人看管地继续执行。
- **绑定转移**：维护一张反向索引“会话 → IM 会话”。绑定转移时，先退订旧的出站投影，向原 IM 会话发提示，再建立新投影。所有操作都带代次号，旧代次的事件一律丢弃，做法同 `/rc` 的 binder。
- **权限模式**：恢复已有会话时沿用该会话自己的权限模式（那是机主自己设置的），但同样不继承 daemon 进程级的危险标志。
- **普通文本的技能展开**：复用 Web 输入的 `try_expand_skill_command`，按会话的工作区扫描技能，保证 IM 与 Web 的行为一致。

### D7. 命令解析是纯逻辑
新增 `channels/core/commands.*`：输入文本和发送者角色（机主或普通联系人），输出命令种类、参数，或“用法错误”。
- `/sessions` 的 `more` / `search` 与编号选择，复用 `parse_rc_session_command` 的语义。
- `/aq`、`/approve`、`/deny` 沿用 WhatsApp 网关已有的处理方式。
- 系统回复文案集中放在 `channels/core/texts.*`，统一为中文，方便以后接入多语言。

### D8. 出站投影
- **事件分流**：每个绑定一个投影器，订阅会话事件。
  - 助手文本：经 `classify_session_event` 判定后投递。
  - 附件：来自 `output_attachments_from_content_parts`，以及 ToolEnd 事件里的 attachments。发送前必须重新解析，确认文件在该会话自己的附件目录内，与 WhatsApp 的做法一致。
  - 权限请求、提问：同 WhatsApp 网关。
- **订阅顺序**：先订阅，再读取待处理的请求快照，避免漏掉中间产生的请求。
- **按回合合并**：平台声明了 `batch_turn_output`（QQ）时，同一回合的助手文本先缓存。遇到回合结束、需要发权限请求或提问、或者缓存超过上限，就合并成段发出。Telegram 收到即发，忙碌期间每 4.5 秒发一次“正在输入”。
- **发送队列与失败统计**：发送经 `RemoteControlHub` 的有界 FIFO。失败计入该绑定的 `failed` / `dropped` 计数，在设置页显示；发送结果不确定时不重发。
- **Desktop 输入同步**：在 Desktop 里向已绑定会话输入时，因为投影器订阅的是会话事件，回复会自然发到 IM，不需要额外处理。

### D9. QQ 扫码绑定协议
协议细节从 WorkBuddy 内置的官方 connector 中读取，并与 Hermes 交叉印证。
- **申请任务**：`POST https://q.qq.com/lite/create_bind_task`，请求体 `{"key": base64(32 字节随机密钥)}`，返回 `{"retcode":0,"data":{"task_id":...}}`。
- **二维码**：内容为 `https://q.qq.com/qqbot/openclaw/connect.html?task_id=<id>&source=acecode&_wv=2`。
- **轮询**：每 2 秒 `POST https://q.qq.com/lite/poll_bind_result {"task_id":...}`，返回的 `status` 含义：0 无、1 进行中、2 完成、3 过期。完成时附带 `bot_appid` 与 `bot_encrypt_secret`。
- **解密**：把 `bot_encrypt_secret` 做 base64 解码，按“前 12 字节 IV、末尾 16 字节认证标签、中间是密文”切分，用本地密钥做 AES-256-GCM 解密。
- **过期**：重新申请任务，二维码随之刷新。整个流程 10 分钟超时。
- **请求头**：带 `Accept: application/json`。缺少它时，q.qq.com 会返回反爬页面（见 Hermes 的注释）。还要带 `User-Agent: ACECode/<版本>`。
- **机主**：若返回结果里有 `user_openid`（Hermes 会读这个字段），扫码人直接成为机主。若没有，扫码成功后 10 分钟内第一个私聊的人自动成为机主。对未认证的新机器人来说，只有管理员本人能私聊，所以这个人就是扫码人。之后的其他人一律走配对审批。
- **在 daemon 中执行**：页面通过 REST 发起或取消扫码，二维码刷新与扫码结果通过 WS 推送。同一时间只允许一个扫码流程。
- 备选：在前端调用这些接口。否决的原因：密钥和解密都应留在 daemon 中，浏览器也受跨域限制。

### D10. QQ 网关与回复预算
- **网关状态机**（纯逻辑）：
  - 握手：Hello，然后 Identify 或 Resume。intents 只订阅 `1<<25`，即单聊与群聊，不含频道。
  - 心跳：按 `heartbeat_interval` 的 80% 发送。
  - 记录收到的 seq，Resume 时要用。
  - 收到 op 7 时重连，op 9 时按可否恢复分别处理。
- **关闭码**：按 WorkBuddy 已验证的表分类。
  - 4004：刷新令牌后重连。
  - 4006 / 4007 / 4009：重新 Identify。
  - 4008：等 60 秒再连。
  - 4900–4913：不可恢复会话，重新 Identify。
  - 4914（仅沙箱或机器人不在线）、4915（已封禁）：停止重试。
- **令牌**：在到期前 5 分钟刷新。
- **回复预算**（纯逻辑，按入站 msg_id 记账）：私聊 60 分钟内最多 4 条，群聊 5 分钟内最多 5 条。超出后改发主动消息。主动消息也被拒绝时，暂存到该 IM 会话的待补发队列（最多 20 条），等下一条入站消息到来时先补发，再处理新消息。所有发送共用一个全进程单调递增的 `msg_seq`，避免 40054005（消息被去重）。
- **发送失败按错误码分类**：若平台判定窗口已过，立即降级为主动消息，不依赖本地计时。这样即使官方文档里“60 分钟还是 5 分钟”的矛盾没有定论，行为也是对的。
- **不发“正在输入”**：QQ 的“正在输入”（msg_type 6）可能占用被动回复的额度，本期不发，待真实验收后再评估。
- **格式**：默认用 Markdown（msg_type 2）。失败时去掉标记，改用纯文本（msg_type 0）。单条文本上限按 4000 字符分段。
- **媒体**：
  - 入站：从 attachments 的 url 下载，上限 25 MiB；语音直接使用 `asr_refer_text`。
  - 出站：先用单聊或群聊的 `/files` 上传，拿到 `file_info`，再以 msg_type 7 发送。本期用 base64 直传，超出直传上限时改为文字说明；分片上传留给后续。

### D11. Telegram
- **校验**：用 `getMe` 校验 token，同时读出 `username`，以及隐私模式状态 `can_read_all_group_messages`。
- **长轮询**：`getUpdates`，参数 `timeout=25`（不少代理会在 30–60 秒后断开空闲连接），只订阅 message。每批处理完先持久化 offset，再确认下一批。
- **409 冲突**：错误描述含 webhook 时，进入“已配置 webhook”状态；用户在页面确认后，才调用 `deleteWebhook`。其他 409 视为被另一个程序占用，每 60 秒重试一次。
- **机主绑定链接**：`https://t.me/<username>?start=<一次性码>`，码为 24 位 URL 安全字符，10 分钟有效。识别到 `/start <码>` 后，立即把发送者设为机主。页面上把链接画成二维码。
- **群里判定为 @ 的情况**：entities 或 caption_entities 中有指向本机器人的 `mention` 或 `text_mention`；`/cmd@本机器人`；`reply_to_message.from.id` 是本机器人。在话题群里，回复带上 `message_thread_id`。
- **格式**：Markdown 转 Telegram HTML，这是纯函数。代码块转为 `<pre><code>`；行内代码、粗体、斜体、删除线、链接分别转成对应标签；标题转为粗体；列表转为符号行；所有文本转义 `& < >`。为避免把标签切断，先按段落把 Markdown 源文本切成 3500 个 UTF-16 单位以内的块，再逐块转换。平台返回 “can't parse entities” 时，改用纯文本重发。
- **限速**（纯逻辑）：同一聊天至少间隔 1 秒，同一个群每分钟不超过 20 条。收到 429 时按 `retry_after` 等待后重发；429 表示平台明确拒绝了该消息，所以重发是安全的。
- **媒体**：
  - 入站：照片取不超过 20 MB 的最大尺寸，通过 `getFile` 加文件 URL 下载。
  - 出站：用 cpr Multipart 调 `sendPhoto` / `sendDocument`；图片超过 10 MB 时改为按文件发送。

### D12. Web 与 Desktop
- **导航**：设置导航「集成」分组加入 `{key:'channels', label:'消息通道'}`。
- **页面**（2026-10-05 验收反馈后改版，仿 WorkBuddy「远程通道」）：样式按 `acecode-frontend-style`。
  - 两列平台卡片，每张只有单色图标（默认聊天图标，不用彩色品牌 logo）、名称、一句说明和一个按钮。没开时按钮是「连接」，开着时是「取消连接」，点击立即生效。
  - 卡片上只显示一行简要状态：连接中、已连接（附机器人名）、连接中断正在重试、连接失败（附「处理」入口）、由其他进程托管。另外显示待批准请求，可以直接批准或拒绝。已配置的平台有「管理」入口，打开后可以更换机器人，或撤销机主与授权名单。
  - 「连接」在没有凭据或没有机主时打开分步向导，凭据与机主都齐时直接连接。向导分三步：
    1. 创建机器人：QQ 打开即获取二维码，可改为手动填写；Telegram 说明 BotFather 的步骤并粘贴 token。
    2. 连接：自动开启，连上后自动进入下一步；失败时可以重试，webhook 阻塞时可以移除。
    3. 绑定机主：Telegram 显示一次性链接与二维码，提示用手机相机扫；QQ 扫码时已带出机主，或提示私聊后批准请求。
  - 页面顶部一句话提示 Desktop 退出与“退出 ACECode 后继续运行后台进程”的关系。WhatsApp 说明与已绑定会话列表不再放在页面上：侧栏电脑图标已能找到绑定会话。
  - 备选：保留开关加整块细节（v1）。否决，因为用户验收时认为太复杂，要求“细节打印到日志里”。
- **日志**：`core/state_log.cpp` 比较同一平台前后两份快照，在每次 `publish_state` 时把变化写进 daemon 日志。内容包括状态与原因、平台附加信息（隐私模式、webhook、暂存待补发）、凭据更新（只记“已更新”，不写尾号）、机主与机主窗口、授权名单、待批准请求、会话绑定与收发计数。另外几处也写日志：入站消息的判定原因（只记来源与长度，不记内容）、IM 命令、QQ 扫码各阶段（不记二维码内容）、QQ 被动回复降级、暂存与补发、超大文件改发说明、Telegram 限频与 HTML 回退。
- **二维码**：用无依赖的 MIT 库 `qrcode-generator` 在前端生成内联 SVG。
- **REST 接口**（只接受本机来源，非本机返回 403，做法同 `routes_pty`）：

  | 方法与路径 | 用途 |
  | --- | --- |
  | `GET /api/channels` | 返回各平台快照 |
  | `POST /api/channels/:platform/enabled` | 开关 |
  | `PUT /api/channels/:platform/credentials` | 保存凭据，先校验再保存 |
  | `POST` / `DELETE /api/channels/qq/bind` | 发起或取消扫码 |
  | `POST /api/channels/telegram/owner-link` | 生成机主绑定链接 |
  | `POST /api/channels/telegram/remove-webhook` | 显式移除 webhook |
  | `POST /api/channels/:platform/requests/:id/{approve,reject}` | 审批配对请求 |
  | `DELETE /api/channels/:platform/access/:id` | 撤销授权 |

- **WS 事件**：`channels_state`（平台快照变化）、`channels_bind`（二维码刷新、成功、失败）、`channels_request`（新的配对请求，App 层监听后经 `desktopNotify` 弹系统通知）。
- **侧栏**：会话列表新增字段 `channel_bound: {platform}`，值来自核心的“已绑定会话”快照。侧栏在 `remote_control_bound || channel_bound` 时显示同一个电脑图标，悬停提示写明来自哪个平台。`/rc` 的专属行为仍然只看 `remote_control_bound`。
  - 备选：直接把通道绑定也写进 `remote_control_bound`。否决的原因：这会误触发 `/rc` 的会话选择提示和动画语义。
- **接口文档**：同步到 `docs/daemon-api.md`。

### D13. 依赖变更
- **curl 开启 WebSocket**：在 `vcpkg.json` 中加入 `{"name":"curl","features":["websockets"]}`。cpr 链接的就是这份 curl，所有三元组都会以带 WebSocket 的 curl 重新构建。
- **OpenSSL**：在非 Windows 平台上，把 OpenSSL::Crypto 显式链接给 AES-GCM 所在的库。
- 备选：通过 FetchContent 引入 IXWebSocket 或 mbedTLS。否决的原因：curl 本身已经提供 WebSocket，代理与 TLS 行为能和现有 HTTP 请求完全一致；而且用户明确要求打开 curl 的特性。

### D15. 更多平台:微信、飞书、钉钉、Discord、LINE(2026-10-06)
用户要求参考 hermes-agent 再接五个平台。协议细节先由调研整理成规格(来源:hermes-agent 适配器、各平台官方 SDK 与文档),
再按 QQ / Telegram 的分层实现:传输层在 `src/adapters/im/<平台>/`,核心不认识平台差异。
- **平台描述表**:`core/platforms.{hpp,cpp}` 集中列出平台名、展示名、凭据字段(是否密钥、是否必填、是否由用户填写)、
  账号字段、群成员身份是否按群隔离(只有 QQ)、用户 id 是否按机器人隔离(换机器人时清空名单)、机主绑定方式、
  是否有扫码流程。运行时、宿主、Web 接口与日志都从这里取,不再写死 qq / telegram。
  传输层的创建、凭据校验与扫码流程集中在 `core/platform_adapters.cpp`,这是唯一认识各平台类型的地方。
- **接入方式**(全部由本机主动连出,LINE 除外):
  - 微信:腾讯 iLink 接口,扫码登录拿到机器人令牌与扫码人(即机主),长轮询收消息,只有私聊;媒体经 CDN,
    用 AES-128-ECB 加解密(新增 `platform/crypto/aes_ecb`、`digest`)。
  - 飞书:先调长连接地址接口,再连 WebSocket;帧是 protobuf,手写编解码(`WebSocketClient` 新增 `send_binary`);
    每个事件帧立即确认。飞书要求长连接在线时才能保存“使用长连接接收事件”,所以向导在连接一步之后列出回后台要做的事。
  - 钉钉:Stream 模式,每次连接先申请一次性票据;回复优先走消息自带的会话回调地址,过期后改用机器人接口。
  - Discord:网关 JSON 协议(不压缩),意图 GUILDS | GUILD_MESSAGES | DIRECT_MESSAGES | MESSAGE_CONTENT;
    优先续连,保护每日登录额度;校验时取应用 id,向导给出邀请链接。
  - LINE:只有回调。分层规则 R7 不允许 adapters 引入 Crow,所以回调服务的接口定义在 `im/line/line_webhook.hpp`,
    Crow 实现在 `core/line_webhook_server.cpp`(channels 模块允许 Crow)。回调服务只监听 127.0.0.1、只开放回调 /
    健康检查 / 媒体三个路径;公网转发绝不能指向 daemon 主端口(本机请求不验令牌,会把终端等接口暴露出去)。
    公网地址由用户填写,留空时自动启动 cloudflared 临时隧道;每次连接都用接口更新 LINE 的回调地址。
  - 备选:LINE 回调直接挂在 daemon 主服务上。否决,原因同上。
- **机主绑定**:新增 6 位一次性绑定码(`AccessControl::issue_owner_pin`),机主私聊机器人发送即可,适用于没有深链接的
  飞书、钉钉、Discord、LINE;10 分钟过期、只能用一次,有未过期绑定码时私聊里猜错 10 次作废全部绑定码。
  `/api/channels/<平台>/owner-link` 对 Telegram 返回链接、对这四个平台返回绑定码。扫码绑定接口也改为
  `/api/channels/<平台>/bind`(QQ、微信),快照里的扫码状态改为 `binds:{平台:状态}`。
- **可选凭据字段**:设置页显式提交空串表示清除可选字段(如 LINE 公网地址改回自动隧道);校验只规整它认识的字段,
  用户填的其它可选设置原样保留。
- **起草方式**:五个平台的传输层由并行子代理按统一约定起草(`scratchpad/im_transport_contract.md`),每个文件用
  “只编译不链接”的单文件检查自查,不碰共享构建目录;核心接线、设置页、文档由主会话完成并统一构建与测试。

### D14. 测试策略
- **纯逻辑单测**：网关状态机与关闭码分类、回复预算、限速器、Markdown 转 HTML、UTF-16 分段、命令解析、访问与配对（过期、机主规则）、配置与状态的校验和损坏拒绝、绑定转移与代次丢弃、AES-GCM（官方 connector 格式的已知向量）。按仓库惯例，测试注释用中文，写清触发场景和期望行为。
- **本机假服务**：用 Crow 搭假的 QQ OpenAPI 加 WebSocket 网关，以及假的 Telegram Bot API，端到端跑传输层：断线恢复、限频、409、webhook、媒体。核心用内存版 `SessionClient`，覆盖首条消息、`/new`、`/resume`、权限请求先答者生效、撤销授权。
- **前端**：`channelsSettings.js` 的 Node 测试，以及侧栏图标判定测试。
- **Windows 实测与真实账号验收**：按任务清单逐项记录到 `verification.md`。

## Risks / Trade-offs

- **[腾讯绑定接口无公开文档，`source=acecode` 可能不被接受，接口也可能变动]** → 手动 AppID/AppSecret 始终可用。真实验收的第一项就是验证扫码；协议常量集中在一个文件里，便于修改。
- **[QQ 被动回复规则的文档前后矛盾，主动消息可能被用户关闭]** → 失败时按错误码立即降级；暂存后在下一条消息时补发。暂存只在内存中，进程重启会丢失，但会话记录本身是完整的，页面会显示待补发条数。
- **[curl 开启 websockets 会改变 vcpkg 依赖构建]** → CI 依赖缓存失效一次。Deepin 和旧 glibc 打包任务需要确认仍走 vcpkg 的 curl。
- **[凭据泄露]** → 私有权限写入，接口只返回脱敏值，日志和错误统一脱敏，Telegram 的 URL 中 token 也要脱敏。设置接口只接受本机来源。
- **[群里有人注入提示词，或非机主借机执行命令]** → 非机主只能在自己的无项目会话中操作。权限模式是 `default`，写操作必须审批，且审批只接受发起人本人或本地操作者，沿用 WhatsApp 的规则。非机主不能切到工作区会话。
- **[未认证的 QQ 机器人只有管理员能用]** → 页面明确提示；需要给别人用时，引导用户去做个人认证。
- **[Desktop 退出后通道断开]** → 页面提示与“退出后保留后台服务”的关系，不擅自修改该设置。
- **[两套绑定逻辑并存（WhatsApp 旧网关与新核心）]** → 新核心的投影组件按“可承载 WhatsApp 传输”的标准设计，后续迁移时删除旧网关。

## Migration Plan

- 新装与升级时，所有新平台默认关闭，不迁移任何数据。WhatsApp 的目录与行为不变。
- 回滚：关闭开关即可停止。旧版本二进制会忽略 `channels/qq`、`channels/telegram` 目录。
- 后续把 WhatsApp 迁入新核心另起变更：为 WhatsApp 实现 `im::Transport`（Baileys 桥或 Cloud API），一次性导入旧的 `channels/whatsapp` 配置后删除旧网关。

## Open Questions

- QQ OpenAPI 域名：官方文档自 2026-08 起写的是 `api.bot.qq.com`，WorkBuddy 5.6.2 仍在用 `api.sgroup.qq.com` 与 `bots.qq.com`。默认采用文档域名，旧域名作为常量备选，真实验收时确认。
- QQ 的“正在输入”是否占用被动回复额度，真实验收后再决定是否开启。
- QQ 大文件的分片上传（Hermes `chunked_upload.py` 可作参考）在后续变更中实现。
