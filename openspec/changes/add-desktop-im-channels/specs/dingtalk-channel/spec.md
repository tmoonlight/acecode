## Purpose

通过钉钉企业内部应用机器人的 Stream 模式,把钉钉私聊和群里的 @ 消息接入 ACECode 的消息通道。用户填 Client ID 与 Client Secret,连接在本机运行,不需要公网地址。

## ADDED Requirements

### Requirement: 凭据配置与校验

钉钉卡片的连接向导 SHALL 在第一步说明创建企业内部应用、添加机器人能力、选择 Stream 模式并发布的步骤,并接受 Client ID 与 Client Secret。保存前 SHALL 换取访问令牌进行校验:失败时 SHALL NOT 保存并提示原因。所有接口返回 SHALL 检查业务错误码,不能只看 HTTP 状态。

#### Scenario: 凭据错误
- **WHEN** 用户填入错误的 Client Secret 并继续
- **THEN** 向导提示凭据无效,原有配置不变

### Requirement: Stream 连接

钉钉通道 SHALL 每次连接前申请一次性连接票据,再以 WebSocket 接收消息:
- 每条机器人消息 SHALL 立即确认,再异步处理;系统 ping SHALL 原样回应;收到断开通知时 SHALL 确认后立即用新票据重连。
- 同一条消息 SHALL 只处理一次(按推送 id 与消息 id 双重去重)。
- 同一个应用同一时刻 SHALL 只被一个 ACECode 进程连接。

#### Scenario: 服务端要求断开
- **WHEN** 钉钉推送断开通知
- **THEN** 通道确认后关闭连接,并立即用新票据重连,期间的消息不重复处理

### Requirement: 私聊与群 @ 消息

钉钉通道 SHALL 处理私聊消息与群里 @机器人 的消息,私聊以发送人的员工 id 标识,群以会话 id 标识。

#### Scenario: 私聊问答
- **WHEN** 已授权用户私聊机器人发送文字
- **THEN** 文字进入该用户绑定的会话,回复发回该私聊

### Requirement: 回复与媒体

回复 SHALL 优先发到消息自带的会话回调地址(有效期内),过期或不存在时改用机器人接口按员工 id 或群会话 id 发送,因此在 Desktop 中输入的回复与重启后的回复同样能送达。文本以 Markdown 发送并按长度分段;群消息 SHALL 遵守每分钟条数限制,等待而不是乱序。图片与文件 SHALL 双向。

#### Scenario: 会话回调过期
- **WHEN** 距离对方最后一条消息已超过会话回调地址的有效期,用户在 Desktop 中向绑定的会话输入
- **THEN** 回复经机器人接口发到对应的私聊或群
