# Design

## Context

动机见 proposal.md「Why」，行为要求见 `specs/` 下三份规格。和实现方式有关的现状：

- **鉴权只有一档**：`src/apps/web/auth.cpp::check_request_auth` 只区分「本机免 token」「token 正确」「token 错误」。258 处路由各自调 `require_auth`(`server_helpers.cpp`)，没有角色概念。
- **局域网只有远程 Web 一条路**：`remote_web_proxy.cpp` 起一个逐字节转发的子进程，监听 `0.0.0.0`，上游从 `127.0.0.2` 连到 daemon(macOS 退到本机非回环 IPv4)，所以 daemon 能分辨「经转发进来」和「本机直连」(`is_trusted_local_client_address`)。`web.remote_port` 为 0 时先试 daemon 端口加 1。
- **Desktop 现在只有一个共享 daemon**(`main.cpp` 的 `kSharedDaemonSlotHash`)，但它的端口每次启动随机选(`daemon_pool.cpp::pick_free_loopback_port`)，所以远程转发端口也跟着变。
- **第二实例交接已经存在**：`src/base/ipc/open_request.*` 把 `--open-workspace/--open-session` 写进每用户一个的交接文件，再发拉前消息；首实例在 `set_existing_instance_focus_handler` 里读取。Windows 用注册的窗口消息，macOS 用分布式通知，Linux 目前是空实现。
- **侧边对话就是现成的「只读提问」**：`side_chat_tools.hpp::build_side_chat_toolset` 只放 `file_read/grep/glob/lsp`，依次过 Deny 规则、路径校验、危险路径；`send_to_main` 为空就不放转发工具。`SessionRegistry::ask_side_question` 要求会话已在内存，同步 HTTP 接口会一直占着 Crow 线程等模型。
- **读会话摘要也是现成的**：`ThreadService::read`(`thread_tools.cpp` 的 `read_thread` 背后)按回合返回有界摘要，可选是否带工具输出。
- **代理**：`proxy_resolver_win.cpp` 读取系统代理时只释放 `lpszProxyBypass`，从不使用；`host_matches_no_proxy` 只做域名后缀匹配，不认网段。公司电脑访问局域网 IP 会被送进代理。
- **加密**：Windows 构建不链 OpenSSL(`CMakeLists.txt` 只在非 Windows 平台 `find_package(OpenSSL)`)，Crow 也没开 SSL。
- **剪贴板**：`src/base/platform/clipboard.cpp` 已有 Windows 读文本；`web_host.cpp` 的 `WM_ACTIVATE` 已有 `window_focus_handler` 钩子。

## Goals / Non-Goals

**Goals:**
- 现有 258 处 `require_auth` 一行不改，访客能力只出现在新文件里，默认全部拒绝。
- B 侧网页永远不执行 A 发来的代码；A 发来的只有数据。
- 尽量复用：转发子进程、交接文件、侧边对话工具循环、会话摘要服务、脱敏函数。
- 每个阶段都能单独发布、单独有用。

**Non-Goals:**
- 不做 TLS。链接里预留 `f`(证书指纹)字段，解码器遇到它先忽略，留给后续变更。
- 不做局域网广播发现、不做 NAT 穿透。
- 不改远程 Web 的鉴权和开关语义，只改它在 `remote_port` 为 0 时的首选端口(见决策 5)。

## Decisions

### 用户确认的分享入口与权限

分享入口放在会话标题栏「属性」左侧，使用开口方框加上箭头的线框图标。图形遵循 `docs/design/interface-icons.md`：`share` 别名指向统一图形库的 `Share`，通过 `VsIcon` 以 16 px 渲染，使用 20×20 坐标、currentColor 和按尺寸计算的线宽；业务组件不另画 SVG。有效分享的会话在侧栏显示统一图形库的 Antenna 发射天线标记（18 px，与侧栏其它图标一致），保留“已分享”无障碍名称。分享对话框显式提供「只读 / 可交互」两种权限，映射到服务端 `allow_questions=false/true`，保留原方案默认允许提问。内容档位「只看对话 / 含工具输出」与权限独立。只读远程视图不显示聊天入口，服务端拒绝提问；可交互只开放独立的受限访客对话，模型额度和工具执行归分享者，不改变主会话历史或忙碌状态。

### 1. 链接形状：固定 host，载荷放路径

`acecode://connect/<base64url>`。载荷不放在 `//` 后面的 host 位置：Chromium 按 URL 标准把这一段当「不透明主机」，虽然不转小写，但空格、`#`、`/`、`?`、`@` 等字符会让整条 URL 失效(`url/url_canon_host.cc`)；macOS 是否改写这一段的大小写没有查到确切结论，而 base64 区分大小写。不放 query 是因为一些聊天软件识别链接时会在 `?`、`=` 处截断。

base64url 只用 `A-Z a-z 0-9 - _`，Chrome 交给系统前做的转义(`EscapeExternalHandlerValue`：控制字符、空格、`" % < > \ ^ { | }` 和非 ASCII)一个都碰不到。Chrome 在 Windows 上曾有 2048 字符的外部协议上限，2025 年 10 月已移除(chromium 提交 7373ed9621)；旧版 Windows 的 `ShellExecute` 在 2083 字符处截断。1024 字符的上限离这两者都远。

载荷 = base64url(JSON 字节 + 4 字节 CRC32，大端)，不带填充。JSON 用短键：

```json
{"v":1,"n":"shao","t":"缓存方案讨论","a":["ACE-PC","192.168.1.20","10.8.0.3"],
 "p":12400,"c":"<share_id>.<secret>","e":1763251200}
```

- `v` 是主版本，解码器只接受自己认识的主版本，多出来的未知键忽略(前向兼容)。
- `c` 由公开的 `share_id`(8 字节，十六进制)和 `secret`(24 字节随机，base64url)组成。日志和界面只用 `share_id`，鉴权比较 `secret`。
- CRC32 用来发现聊天软件改坏、截断的链接，不是安全措施。真正的安全来自 `secret` 的随机性和链接只在私下传递。

备选：自定义二进制格式会更短，但调试和人工排查都难；JSON 加 base64url 大约 350 个字符，离 1024 的上限很远。

### 2. 访客走独立入口，不给 `require_auth` 加角色

新增 `routes_share.cpp`；`ShareStore::authorize` 管理凭据校验，Web `require_share_auth` 只负责请求边界：

| 接口 | 作用 |
|---|---|
| `POST /api/share/v1/hello` | 握手：协议版本、主人名、会话标题、档位、是否允许提问、额度 |
| `GET /api/share/v1/messages?limit&before` | 过滤后的分页历史，游标沿用现有格式 |
| `GET /api/share/v1/turns?cursor` | 给 AI 用的回合摘要(`ThreadService::read`) |
| `GET /api/share/v1/attachments/<id>/blob` | 只限这个会话里的图片 |
| `WS /ws/share/v1` | 订阅事件、心跳、`ask_start` / `ask_stop` |

凭据放在请求头 `X-ACECode-Share`，HTTP 和 WebSocket 都一样(客户端是 B 的 daemon，不是浏览器，可以随意设头)，不放 URL，避免出现在任何访问日志里。`require_share_auth` 返回一个只读的授权对象(share_id、会话 id、档位、能否提问)，后续处理只认这个对象里的会话 id，忽略请求里出现的任何其它会话标识。

公共请求入口先拒绝携带 `X-ACECode-Share` 访问非访客路径；现有 `require_auth` 另加一条规则：请求不是从本机直连来的(也就是经转发进来的)，而远程 Web 当前没开，就拒绝，哪怕 token 正确。这样「只开分享」时监听虽然开着，完整 token 也用不了。

备选：给 `require_auth` 加访客角色，再给每个路由标注允许的角色。改动面是 37 个文件、258 处，漏标一处就是越权，否决。

### 3. 分享记录存用户数据目录

`<data_dir>/shares/shares.json` 走 `utils/state_file` 的 JSON 事务，记录：share_id、secret、会话 id、会话所在 cwd(UTF-8 字符串)、档位、能否提问、创建和过期时间、是否撤销、当日提问计数。提问记录写在 `<data_dir>/shares/<share_id>.asks.jsonl`，每条分享最多保留 200 条。secret 和 daemon token 同级保存(用户目录权限)，因为主人要能重新复制链接，不能只存哈希。

不进 `config.json`：分享是运行数据，不是配置；而且 `config.json` 会被备份、会被手改。

### 4. 局域网监听：一个转发子进程，两个启动理由

`share_runtime.cpp` 统一计算转发子进程的期望状态，从「远程 Web 开」改成「远程 Web 开，或者有至少一条有效分享」，dangerous 模式下两者都不启动。分享增删、过期扫描(每分钟一次)之后重新计算期望状态，只启停转发子进程，daemon 本身不动，这和远程 Web 现在的开关方式一样。

### 5. 端口：固定首选值

`web.remote_port` 非 0 时照旧固定使用。为 0 时，首选端口从「daemon 端口加 1」改成固定的 `DEFAULT_WEB_PORT + 1`(12400)，失败再退到「daemon 端口加 1」，再退到系统分配。命令行 daemon 默认端口是 12399，首选值和原来一样；Desktop 的 daemon 端口是随机的，改完以后重启也能落在同一个端口上，旧链接继续可用。生成链接时写实际端口。

### 6. 访客内容过滤放在服务端，按消息投影

过滤在 `share_projection.*` 里做，输入是现有消息负载(`message_payload.cpp` 的输出)，输出是给访客的精简负载：

- 「只看对话」：保留 user / assistant 文本和工具行的名称与 `tool_summary`，去掉工具输入、输出、hunks、非图片附件、`metadata` 里除显示必需字段之外的内容。
- 「含工具输出」：保留全部，只去掉本机路径以外的敏感元数据(权限决定、审计信息)。
- 两档都对所有文本字段跑 `redact_secrets`。

实时事件走同一个投影函数，所以历史和实时显示一致。投影是纯函数，单测覆盖每种消息类型。

### 7. 访客提问：复用侧边对话的流式循环，换一套更严的工具策略

`build_side_chat_toolset` 加一个可选的「额外拒读判定」参数。访客提问时传入密钥类路径规则(`.env*`、`*.pem`、`*.key`、`id_rsa*`、`*.p12`、`*.pfx`、`.npmrc`、`.netrc`、`.git-credentials` 等)，`send_to_main` 传空。注意 `grep` 和 `glob` 的结果里也可能出现这些文件，所以判定要作用到结果过滤上，而不只是拦 `file_read`。答案流式输出先缓冲不完整的文本行与 PEM 块，再脱敏发送，避免秘密跨片段泄露；`side_chat_done` 的完整答案再脱敏一次。

调用 `SessionRegistry::stream_side_chat`(流式，跑在独立线程)，不用同步的 `ask_side_question`。会话不在内存时先走 `SessionRegistry::resume`(已有单飞)。额度检查在调用模型之前做。

### 8. B 侧：本机 daemon 代连，网页只跟本机说话

新增 `host/peers/`(对端存储、连接、候选地址探测)和 `routes_peers.cpp`：

- `POST /api/peers/connect {link}`：解码、握手、保存，返回对端 id。只有用户在确认框里点了连接，网页才会调用它。
- `GET /api/peers`、`DELETE /api/peers/:id`
- `GET /api/peers/:id/messages`、`GET /api/peers/:id/attachments/...`：转发到对端，原样返回投影后的数据。
- `WS /ws/peers/:id`：B 的网页连本机 daemon，本机 daemon 再连对端的 `/ws/share/v1`，双向中继，只允许订阅和提问两类消息。

对端存储放 `<data_dir>/peers.json`，所有工作区共用。HTTP 用 cpr，WebSocket 用现有的 `websocket_client`，都经过 `proxy_resolver`。

前端的远程会话视图复用现有对话渲染组件，但以只读模式挂载：不挂输入框和审批区，不请求 git 信息、模型列表这些本机会话才有的接口；`visual` 代码块按源码显示。

备选：B 的 Desktop 直接用 WebView 打开 A 提供的网页。实现最省，但等于在 B 的程序里运行 A 的代码，而且很难保证那个 WebView 拿不到 Desktop 的原生桥接函数；AI 工具也还得再写一套客户端。否决。

### 9. 私有网段直连

`proxy_resolver` 在所有代理来源之前加一条判定：目标是 IP 字面量且落在私有、链路本地或唯一本地网段，或者是不带点的主机名，就直连。这条判定对所有请求生效，而不只是对端请求：访问局域网地址本来就不应该走公网代理，模型服务的公网地址不受影响。同时补上 Windows 系统代理例外列表(`lpszProxyBypass`，包括 `<local>`)的解析。

### 10. 系统协议与第二实例交接

- **地址栏能不能用**：Chromium 的 `ChromeAutocompleteSchemeClassifier::GetInputTypeForScheme` 对没见过的协议，在 Windows 和 macOS 上去问系统有没有对应程序，有就当网址打开(随后弹「打开 ACECode?」)，没有就当搜索词；Linux 上一律当搜索词。Firefox 的 `URIFixup` 同理：系统里有对应程序才当网址。Edge 用的是同一套 Chromium 代码，但没有单独核实，放进验收任务。
- **Windows 注册**：`HKCU\Software\Classes\acecode`，默认值 `URL:ACECode`，`URL Protocol` 为空字符串，`DefaultIcon` 指向程序图标，`shell\open\command` 为 `"<acecode-desktop.exe>" --open-invite "%1"`(`%1` 必须带引号，否则含空格的参数会被拆开)。`HKCU\Software\Classes` 只影响当前用户，并且优先于全机设置，不需要管理员权限。安装器写入，卸载器只在命令行指向本次安装时删除。Desktop 启动时检查：没有注册，或注册的程序已不存在，就写成自己。系统每次打开链接都会新起一个进程，所以下面的交接是必需的。
- **交接**：`DesktopOpenRequest` 加一个 `invite` 字段，和现有的 workspace/session 两种请求互斥。第二实例写交接文件、发拉前消息后退出；首实例在拉前回调里读取，页面就绪前收到的先存着，就绪后派发给网页弹确认框。
- **macOS**：Info.plist 声明 `CFBundleURLTypes`，在宿主里实现 `application(_:open:)`(或接收 `kAEGetURL` 事件)。Launch Services 的文档写明：程序已经在运行时只激活它，再把 URL 事件发给它，所以不需要交接文件。
- **Linux**：仓库里还没有 `.desktop` 文件，放到最后一个阶段，和打包一起做。`.desktop` 里写 `MimeType=x-scheme-handler/acecode;`，`Exec` 用 `%u` 接收一个 URL，再用 `xdg-mime default` 设成默认程序。已运行实例的接收可以用 desktop entry 规范里的 D-Bus `Open` 方法，或者给现有的空实现补一个本地套接字。Linux 上的 Chrome 地址栏不认自定义协议，只能靠点击链接、剪贴板和粘贴这三种方式。

### 11. 剪贴板识别

- **Windows**：Desktop 宿主窗口调用 `AddClipboardFormatListener`(微软推荐的做法，不要轮询 `GetClipboardSequenceNumber`)，收到 `WM_CLIPBOARDUPDATE` 后：先查是否带 `ExcludeClipboardContentFromMonitorProcessing` 格式，或 `CanIncludeInClipboardHistory` 的值为 0，任一成立就跳过。微软文档只说这两个格式控制剪贴板历史和云同步，「监视程序也应跳过」是行业惯例，但密码管理器复制密码时通常会设置它们，按最保守的理解处理。否则读 `CF_UNICODETEXT`，只截取前 8 KiB 做前缀匹配，没匹配上就立刻丢弃。匹配上的链接按 SHA-256 记在内存里，已经提示过的不再提示，进程退出就清空。窗口在前台时直接弹确认；不在前台时走现有通知通道(`notifications_win.cpp`)，点击后拉起窗口再弹确认。
- **macOS**：只在应用被激活时检查一次。macOS 15.4 起有剪贴板隐私提示的开发者预览：程序在用户没有粘贴动作时读剪贴板会弹提示(`accessBehavior`)；有报告说 macOS 26 默认还没打开，以后的版本不确定。所以先用 `detectPatterns(for:)` 探测(苹果文档写明不会提示用户)，探测到像链接的内容才真正读取；`accessBehavior` 为 `.alwaysDeny` 时整段跳过。
- **业界参照**：淘宝的「淘口令」就是打开 App 时读剪贴板识别口令。2020 年有研究发现几十个 iOS App 每次打开都读剪贴板，iOS 14 加了粘贴提示后 TikTok 等被曝光并停止这种做法。所以这里的约束是：只认自己的前缀、读完立刻丢、不留记录、可以关，macOS 不在后台读。
- **开关**：`desktop.detect_invite_links_in_clipboard`，默认 `true`，放在设置 > 常规。

### 12. 粘贴识别

输入框的粘贴处理(`composerFileIntake.js` 一带)在处理文本之前调用纯函数 `extractInviteLink(text)`：粘贴的文字去掉首尾空白后只是一条邀请链接，或者是「说明文字 + 一条链接」的形式，就拦下粘贴并弹确认；取消就按普通文本粘贴。纯函数放 `web/src/lib/inviteLink.js`，和 C++ 解码器共用一组测试向量(`tests/fixtures/invite_links/*.json`)，保证两边解码结果一致。

## Risks / Trade-offs

- [局域网明文传输，同网段的人能嗅探到对话和分享码] → 第一版明确告知(分享对话框里写「仅限可信局域网」)；有效期默认 1 天；链接格式预留证书指纹字段，后续变更用 A 的自签证书、指纹放进链接里，由 B 校验，不需要 CA。这正是 Syncthing(设备 ID 就是证书哈希，线下交换)、KDE Connect(配对后把对方证书作为唯一信任根)、LocalSend(握手时校验证书 SHA-256 指纹)的做法。
- [对方电脑没装 ACECode，或者在 Linux 上用 Chrome，把链接粘进地址栏回车，链接会作为搜索词发给搜索引擎] → 分享码只在局域网里有用，外人拿到也连不上；默认 1 天过期，主人可以随时撤销；复制邀请时附带的说明文字写「用 ACECode 打开」，引导对方把链接粘进 ACECode。
- [链接被转发给不该看的人] → 链接就是凭据，这一点在分享对话框里写清楚；主人能随时撤销，撤销会断开所有已建立的连接；提问记录里能看到访客自报的名字。
- [访客通过提问套出密钥] → 拒读清单、工具结果过滤、答案脱敏三层。脱敏靠正则，挡不住所有形式，所以默认档位是「只看对话」，并且主人能关掉提问。
- [访客提问花主人的额度] → 每个分享码同时只能有一个提问，每天 30 次，提问使用主人当前会话的模型，主人在详情里能看到用量。
- [恶意网页触发 `acecode://` 链接，让 B 的 ACECode 去连攻击者的机器] → 确认之前不发任何网络请求；确认框显示地址；对端返回的一切都按不可信数据处理；AI 工具只能用已保存的对端。
- [后台剪贴板监听引起隐私顾虑] → 只做前缀匹配，不落盘、不写日志；尊重「不要监视」标记；设置里能关；macOS 只在激活时检查。
- [多网卡时候选地址顺序不对，连接慢] → 每个地址最多等 3 秒，成功的地址会被记住、下次优先；生成链接时把虚拟网卡(Hyper-V、VMware、WSL、Docker)排到最后。
- [两边版本不一致] → 握手里交换协议版本；对端版本更高但主版本相同就继续，主版本不同就提示双方升级。

## Migration Plan

全部是新增能力，没有数据迁移。按阶段发布：

1. 链接格式、A 侧分享与只读接口、B 侧代连与只读视图、Windows 协议注册与第二实例交接、粘贴识别、私有网段直连。
2. 访客提问(A 侧受限提问 + B 侧提问界面)和额度。
3. 剪贴板识别(Windows 后台监听、macOS 激活时检查)。
4. AI 读取 / 询问对端的工具。
5. macOS 协议声明与 Linux `.desktop` 注册。

回滚：关闭所有分享即可让局域网监听停止(远程 Web 没开时)；协议注册是当前用户范围的一个注册表键，卸载器负责删除。

## Open Questions

- macOS 的 `detectPatterns(for:)` 会不会把 `acecode://` 这种自定义协议识别成 `probableWebURL` 或 `links`，没有查到结论，要在 Mac 上实测。识别不出来就退回成激活时直接读剪贴板(可能触发隐私提示，此时用户会看到系统提示)。只影响阶段 3 的 macOS 实现细节，不改规格。
- 访客自报名字的来源：先用 B 的电脑名，以后再看要不要在设置里加「显示名」。
