## Purpose

通过 LINE Messaging API,把 LINE 私聊和群里的 @ 消息接入 ACECode 的消息通道。LINE 只能把消息推送到公网 HTTPS 地址,所以 daemon 另起一个只监听本机的回调服务,经用户的公网地址或自动建立的 Cloudflare 临时隧道接收消息。

## ADDED Requirements

### Requirement: 凭据配置与校验

LINE 卡片的连接向导 SHALL 说明建立官方账号、启用 Messaging API、打开 Use webhook 与关闭自动回应的步骤,并接受 Channel ID 与 Channel secret(可另填长期令牌代替)。保存前 SHALL 用它们换取短期访问令牌并查询机器人信息以校验:失败时 SHALL NOT 保存并提示原因。

#### Scenario: 凭据错误
- **WHEN** 用户填入错误的 Channel secret 并继续
- **THEN** 向导提示凭据无效,原有配置不变

### Requirement: 本机回调服务与签名校验

LINE 通道 SHALL 使用独立的回调服务,只监听 127.0.0.1,只提供回调、健康检查与出站媒体三个路径;公网转发 SHALL NOT 指向 ACECode 自身的本机服务端口。回调请求 SHALL 先用 Channel secret 校验签名,不通过的请求一律拒绝;通过后 SHALL 立即返回成功,再交给通道线程处理,同一事件只处理一次。

#### Scenario: 伪造的回调
- **WHEN** 公网上有人向回调地址发送签名不对的请求
- **THEN** 请求被拒绝,内容不进入任何会话

### Requirement: 公网地址与临时隧道

用户填写了公网地址时,SHALL 使用它;留空时 SHALL 自动启动 cloudflared 临时隧道指向回调服务,并在隧道就绪后取得公网地址。每次连接成功后 SHALL 通过接口把 LINE 的回调地址更新为当前公网地址。既没有公网地址又找不到 cloudflared 时,SHALL 停止重连,卡片提示连接失败,日志写明安装 cloudflared 或填写公网地址的办法。

#### Scenario: 自动隧道
- **WHEN** 用户没有填写公网地址,本机已安装 cloudflared,点击「连接」
- **THEN** 通道启动临时隧道,把回调地址更新为隧道地址后显示已连接

### Requirement: 私聊与群 @ 消息、回复与媒体

LINE 通道 SHALL 处理私聊消息,以及群和多人聊天里 @机器人 的消息。回复令牌仍在有效期内时 SHALL 用回复接口,否则用推送接口;推送额度用尽时 SHALL 暂存输出,等对方下一条消息时用新的回复令牌补发。文本按 5000 字分段,Markdown 转为纯文本。入站图片与文件 SHALL 下载导入会话;出站图片经回调服务的媒体地址发送,文件 SHALL 改发文字说明(LINE 机器人不能发送文件)。私聊期间 SHALL 显示加载动画。

#### Scenario: 回复令牌过期
- **WHEN** 回合耗时超过回复令牌的有效期
- **THEN** 答复改用推送接口送达
