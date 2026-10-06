## Purpose

通过 Discord Bot,把 Discord 私信和服务器频道里的 @ 消息接入 ACECode 的消息通道。用户粘贴 Bot Token,连接在本机以网关 WebSocket 运行,不需要公网地址。

## ADDED Requirements

### Requirement: Token 配置、校验与邀请

Discord 卡片的连接向导 SHALL 说明在开发者后台创建应用、复制 Token 与打开 Message Content Intent 的步骤,并接受 Bot Token。保存前 SHALL 查询机器人与应用信息以校验 Token:失败时 SHALL NOT 保存并提示原因。校验成功后 SHALL 保存机器人 id 与应用 id,向导 SHALL 给出带发消息所需权限的邀请链接。

#### Scenario: 有效 Token
- **WHEN** 用户粘贴有效 Token 并继续
- **THEN** 向导连接网关,并显示把机器人拉进服务器的邀请链接

### Requirement: 网关连接

Discord 通道 SHALL 以 JSON 网关协议连接(不压缩),订阅服务器消息、私信与消息内容:
- SHALL 按网关下发的间隔(首次带随机抖动)发送心跳,未收到确认时视为失联并重连。
- 断线后 SHALL 优先续连原会话,失败再重新登录,登录之间留出间隔,避免耗尽每日登录额度。
- Token 无效、权限意图未开启等不可恢复的关闭原因 SHALL 停止重连,卡片提示连接失败,原因写入日志。

#### Scenario: 未开启消息内容意图
- **WHEN** 网关以未开启消息内容意图为由关闭连接
- **THEN** 通道停止重连,日志写明需要在开发者后台打开 Message Content Intent

### Requirement: 私信与频道 @ 消息

Discord 通道 SHALL 处理私信,以及服务器频道(含子区)里 @机器人 或回复机器人的消息;SHALL 忽略机器人自己和其他机器人发出的消息,并从文字中去掉对机器人的 @。私信以对方用户 id 标识,频道以频道 id 标识。

#### Scenario: 频道里 @机器人
- **WHEN** 已批准频道里的已授权成员发送 “@机器人 帮我看看”
- **THEN** 这段文字进入该成员在该频道的会话,回复发回原频道

### Requirement: 发送、限速与附件

回复 SHALL 以 Markdown 原样发送,按 2000 字符上限分段,第一段引用触发消息;发送时 SHALL 禁止 @everyone 与角色提醒。平台返回限流时 SHALL 按给出的等待时间后重发且不乱序。会话忙碌期间 SHALL 显示“正在输入”。附件 SHALL 双向:入站从 CDN 下载,出站以表单上传。

#### Scenario: 平台限流
- **WHEN** 发送被返回限流及等待时间
- **THEN** 通道按给出的时间等待后重发,后续消息顺序不变
