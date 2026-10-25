## Why

消息通道目前只有七个内置平台(QQ、微信、飞书、钉钉、Telegram、Discord、LINE),平台的收发实现都写在 ACECode 里。私有部署、协议不公开的 IM 不适合内置进 ACECode 本体,应由外部插件接入。此前这类 IM 只能借 `/rc` 的通道插件协议接进来,但那套协议只管「绑定 / 解绑一个会话」,没有连接状态、登录态、群聊,也进不了设置页的「消息通道」。

2026-10-25 用户要求:

- 外部 IM 以卡片形式出现在「消息通道」,连接 / 取消连接与内置平台一致;只有装了对应的连接器增量包才出现,标准版看不到。
- 不光是「自己发给自己」,还要能在卡片上填群 ID,把群聊当作一个会话:AI 不把对话内容发到群里,只有调用发群消息工具时才发;按设定的轮询时间检查群消息有没有变化,有变化才由 AI 结合上下文判断要不要以用户的身份回复。
- 卡片表单里有专用的代答提示词,约定发哪些内容、对哪些指令有响应;@用户本人的消息优先回答;群消息默认不带标记,可以自定义前缀。
- 群聊代答会话只开放只读工具和发群消息工具。
- 账号可能被其它客户端踢下线:插件要能识别,识别后停止自动重连并拉起登录,不登录就不能继续使用该通道。

## What Changes

- **外部平台插件**:daemon 启动时扫描 `<data_dir>/connectors/*/channel-platform.json`,每份清单声明一个平台(名字、展示名、可执行文件、参数、账号模型、可选能力)。合法的清单注册进平台描述表,与内置平台走同一套运行时、会话路由、出站投影与设置页卡片。
- **通道传输协议 v1**:插件是一个常驻子进程,stdin/stdout 每行一个 JSON 对象(请求 / 响应 / 事件)。ACECode 只认这份通用协议;IM 的登录、长连接、消息库、防回环全部留在插件里。子进程意外退出时按退避自动重启。
- **本人账号模型(`account: self`)**:插件登录的就是用户本人的 IM 账号,没有机器人。连上即以该账号为机主,私聊只接受「自己发给自己」的消息,不需要绑定码与配对审批。
- **群聊代答(`watched_groups` 能力)**:
  - 卡片表单:群聊 ID、轮询间隔、消息前缀、代答提示词。
  - 每个群聊对应一个自动创建的无项目会话。群消息先缓存,到轮询时间且有他人发的新消息、会话空闲时,汇总成一条用户消息交给 AI;@用户本人的消息带标注、优先判断。
  - 代答会话的系统提示说明身份、规则与用户的代答约定;只开放只读内置工具与 `send_to_group_chat`,不开放 MCP 工具。AI 的普通回复、权限请求、提问都不会发到群里。
  - `send_to_group_chat` 只在代答会话里可见可用,发出的文字带上用户配置的前缀(默认无)。
- **需要登录**:插件在状态里报告 `login_required` 时,卡片显示「登录」,点了调用插件的 `login` 操作。
- 新增接口:`PUT /api/channels/<platform>/settings`、`POST /api/channels/<platform>/login`;`GET /api/channels` 的平台快照带上插件平台的展示信息与设置。
- `/rc` 远程控制与 channel-plugin v1 协议保持不变。

## Impact

- 代码:`src/adapters/im/plugin/`(插件传输层)、`src/host/channels/core/`(平台注册、本人账号访问规则、群聊代答)、`src/domain/session/`(会话元数据 `channel_delegate`)、`src/engine/agent/request/`(代答会话的系统提示与工具策略)、`src/host/session_host/tools/`(`send_to_group_chat`)、`src/apps/web/routes/routes_channels.cpp`、`web/src/lib/channelsSettings.js` 与 `components/ChannelsSettings.jsx`。
- 数据:`~/.acecode/channels/<插件平台>/config.json` 新增 `settings` 段;会话 meta 新增可选 `channel_delegate`(缺省省略,旧 meta 兼容)。
- 文档:`docs/channel-platform-plugin.md`(插件开发协议)、`docs/channels.md`、`docs/daemon-api.md`。
