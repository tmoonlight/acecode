# Proposal

## Why

团队开会时常见的情况是「A 说我的 AI 怎么说，B 说我的 AI 怎么说」，大家只能转述。ACECode 目前只有远程 Web 一条跨机器通路：它把整个 daemon 的完整 token 交出去，拿到的人能操作所有会话、只读浏览全盘，不适合把「某一个会话」安全地给同事看或问。

本变更让 A 生成一条 `acecode://` 邀请链接，B 把它粘到浏览器地址栏、复制到剪贴板，或者粘进 ACECode 输入框，B 的 ACECode 就能识别出来，在 B 确认后直接连到 A 的机器(同一局域网内直连，不经过任何中转服务)，只读查看 A 分享的那个会话，并向它提问。这对应此前讨论的协作四级里的第 1 级「看得到」和第 2 级「问得到」。

## What Changes

- **邀请链接**：新增 `acecode://connect/<载荷>` 链接格式。载荷是 base64url 编码的小段 JSON，带格式版本、A 的显示名、多个候选地址(电脑名和各网卡 IP)、端口、分享码、过期时间和校验值。base64url 只是编码，不是加密；链接本身就是访问凭据，和远程 Web 的 token 链接一样要私下传。
- **三条识别入口**(都只会弹确认框，不会自动连接)：
  - 系统协议：Windows 安装器和 Desktop 首次启动都会在当前用户下注册 `acecode://`，不需要管理员权限。macOS 在 Info.plist 里声明。Windows 和 macOS 上，Chrome、Edge、Firefox 在系统里查到这个协议有对应程序时，会把地址栏里粘贴的链接当作网址打开，并拉起 ACECode(Linux 上的 Chrome 一律当搜索词，只能点击链接或走另外两条入口)；如果 ACECode 已经在运行，第二个进程把链接交给已运行的实例后退出，复用现有的 `open_request` 交接文件和拉前消息。
  - 剪贴板：Windows 上 Desktop 监听剪贴板变化(窗口在后台或托盘时也生效，用系统通知提示)，macOS 只在窗口被激活时检查一次。只匹配 `acecode://connect/` 前缀，同一条链接只提示一次。密码管理器标记为「不要被监视」的剪贴板内容一律跳过，读到的内容不记日志、不外发。设置里可以关掉。
  - 粘贴：在 ACECode 输入框里粘贴邀请链接时，提示「连接到 xxx」而不是当作普通文本发送。Web、安卓等没有系统协议的场景走这条。
- **A 侧分享**：
  - 会话标题栏的「属性」按钮左侧新增开口方框加穿出的上箭头样式的分享图标。链接权限明确分为「只读」和「可交互」：只读仅查看内容，可交互额外允许独立访客提问；可见内容档位与链接权限分别选择。
  - 会话可以生成分享，选「只看对话 / 含工具输出」和有效期，得到邀请链接；可以随时撤销。
  - 分享码使用独立的访客入口，永远不能通过现有 daemon 的 `require_auth`，只能访问这一个会话的只读数据和提问接口。
  - 局域网监听复用远程 Web 的转发子进程。「有分享」和「开远程 Web」是两个开关：只开分享时，经转发进来的请求只认分享码，不认完整 token。
- **A 侧提问**：访客提问复用侧边对话的只读工具循环，但更严：额外的禁读清单(`.env`、密钥、证书等)，不提供「转发给主会话」，答案发出前脱敏，每个分享码有频率和每日上限，流式返回，不占用 HTTP 服务线程。会话没打开时在后台自动打开。主人能看到谁问了什么、答了什么，这些都不写进主会话历史。
- **B 侧连接**：
  - 由 B 本机的 daemon 代为连接，B 的网页只和自己的 daemon 通信，从不加载 A 发来的页面或脚本。
  - 按候选地址逐个尝试；访问私有网段和不带点的主机名时强制直连，不走系统代理。
  - 连上后在 B 的界面里打开一个只读的「远程会话」视图，可以看历史、看实时进展、提问。
  - 连过的对端保存在用户数据目录，分享码只回显尾号。
- **B 侧 AI 工具**(最后一个阶段)：新增「读同事的会话」和「问同事的会话」两个工具，返回内容标记为外部来源、不可信。
- **不做**：房间和多个 AI 互相讨论(协作第 3 级)、跨机器写代码(第 4 级)、自动发现局域网里的其他人、跨公网 / NAT 穿透、TLS 加密(链接格式里预留证书指纹字段，留给后续变更)、分享 TUI 主会话(它不在会话登记表里)。

## Capabilities

### New Capabilities
- `invite-links`：邀请链接的格式、编码和校验，生成方式，以及三条识别入口(系统协议、剪贴板、粘贴)和连接前确认。
- `lan-session-share`：A 侧的会话分享，包括分享记录与撤销、独立于 daemon token 的访客鉴权、局域网监听、只读内容接口与过滤、受限的访客提问、额度与日志。
- `remote-session-client`：B 侧的对端连接，包括 daemon 代连、候选地址与直连、只读远程会话视图、提问界面、已保存对端，以及给 AI 用的读和问两个工具。

### Modified Capabilities

无。远程 Web 转发的需求还在未归档的 `proxy-remote-web-access` 变更里，没有进主规格。本变更复用这个转发子进程，只改一处行为：`web.remote_port` 为 0 时，首选端口从「daemon 端口加 1」改成固定的 12400(见设计决策 5)。命令行 daemon 的默认端口是 12399，对它来说首选端口不变。

## Impact

- **Desktop**：`src/apps/desktop/main.cpp`(第二实例交接邀请链接)、`src/base/ipc/open_request.*`(交接请求加邀请链接类型)、`src/apps/desktop/web_host.cpp`(窗口激活时检查剪贴板)、`src/base/platform/clipboard.cpp`(读文本前检查「不要监视」格式)、Windows 协议注册(新增文件，安装器 `src/apps/installer/windows_shell.cpp` 也要写和删)、`cmake/macos/ACECodeDesktopInfo.plist.in` 以及 macOS 宿主接收 URL 事件。Linux 目前仓库里没有 `.desktop` 文件，放到最后一个阶段。
- **Daemon / Web 服务**：新的访客路由和访客 WebSocket(`src/apps/web/routes/` 下新增文件)、`src/apps/web/auth.*`(访客码校验和转发来源判定)、`src/apps/web/remote_web_proxy.*` 与 `src/apps/daemon/worker.cpp`(只开分享时也启动监听)、`src/host/session_host/session_registry.*`(访客提问入口)、`src/engine/agent/side_question/`(访客专用的工具策略)。
- **B 侧客户端**：daemon 新增对端管理与转发接口，`src/base/network/proxy_resolver.*` 支持私有网段直连，新的模型工具注册在 TUI、daemon、headless 三处入口。
- **前端**：分享对话框、邀请确认框、远程会话视图、输入框粘贴识别，新增中文文案要走 i18n 目录流程。
- **配置**：`web` 段新增分享监听端口；`desktop` 段新增剪贴板识别开关；对端列表和分享记录不进 `config.json`。
- **文档**：`docs/daemon-api.md` 加分享和对端两节，新增主题文档，`CLAUDE.md` 加一行索引。
- **依赖**：不新增第三方库。
