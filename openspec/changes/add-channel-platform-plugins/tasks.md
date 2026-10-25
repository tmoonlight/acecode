## 1. 平台注册

- [x] 1.1 清单解析与扫描(`channels/core/plugin_platforms`):字段校验、`${manifest_dir}`、重名与非法清单跳过并记日志
- [x] 1.2 `PlatformSpec` 插件字段、`register_plugin_platforms` / `ScopedPluginPlatforms`、`Address::valid` 改为字符集校验
- [x] 1.3 daemon 启动时扫描并注册(`im_channels.cpp`)

## 2. 插件传输层

- [x] 2.1 `im/plugin/plugin_transport`:JSON lines 请求 / 响应 / 事件、initialize、状态与入站映射、子进程退避重启、stop
- [x] 2.2 `make_platform_transport` 为插件平台创建传输层;`configure` 与 `login` 操作
- [x] 2.3 测试插件程序 `fake_channel_plugin`(脚本化行为)与传输层单测

## 3. 本人账号与群聊代答

- [x] 3.1 `OwnerBinding::Self` 访问规则
- [x] 3.2 `settings` 存储、校验、`set_settings`、快照
- [x] 3.3 群缓冲、轮询汇总、代答会话创建与绑定、移除群
- [x] 3.4 会话 meta `channel_delegate`(存储、SessionManager、Registry 更新入口)
- [x] 3.5 AgentLoop:`<group-delegate>` 系统提示、工具白名单、非代答会话隐藏发群工具
- [x] 3.6 `send_to_group_chat` 工具(前缀、长度与次数上限),daemon 注册

## 4. 接口与设置页

- [x] 4.1 `PUT /api/channels/<p>/settings`、`POST /api/channels/<p>/login`,快照带插件信息与设置
- [x] 4.2 前端:插件卡片、登录按钮、群聊代答表单;纯逻辑放 `channelsSettings.js` 并补 Node 测试
- [x] 4.3 i18n 目录

## 5. 文档与验证

- [x] 5.1 `docs/channel-platform-plugin.md`、`docs/channels.md`、`docs/daemon-api.md`
- [x] 5.2 端到端测试:生产装配 + 假插件跑通 自聊、群聊汇总、发群工具、需要登录
- [x] 5.3 全量单测、`pnpm test`、`pnpm build`、ChannelBoundaryGuard
