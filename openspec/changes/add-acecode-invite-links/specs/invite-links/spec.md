# Spec Delta

## Purpose

定义 `acecode://` 邀请链接的格式和识别方式，让用户把链接粘到浏览器、复制到剪贴板或粘进 ACECode 时，都能在确认后连接到同事分享的会话。

## ADDED Requirements

### Requirement: 邀请链接格式
邀请链接 SHALL 形如 `acecode://connect/<载荷>`。载荷是 UTF-8 JSON 的 base64url 编码，不带填充，包含：格式版本、主人显示名、会话标题(截断到 80 字节)、候选地址列表、端口、分享码、过期时间、校验值，以及一个预留的证书指纹字段(本变更不使用)。整条链接 SHALL 不超过 1024 个字符。

#### Scenario: 生成的链接可以往返解码
- **WHEN** 主人生成一条邀请链接，再把它原样解码
- **THEN** 解码结果 SHALL 与生成时的字段逐一相同

#### Scenario: 候选地址很多
- **WHEN** 主人电脑上有很多网卡地址，全部写进去会超过长度上限
- **THEN** 系统 SHALL 按电脑名优先、有线和无线网卡其次、虚拟网卡最后的顺序截断地址列表，保证链接不超过 1024 个字符

### Requirement: 解码时严格校验
解码器 SHALL 拒绝：未知的主版本、缺少必填字段、校验值不匹配、已过期、超过长度上限、地址不是合法主机名或 IP 的链接。拒绝时 SHALL 给出能区分原因的错误，并且 SHALL NOT 发起任何网络请求。

#### Scenario: 链接在聊天软件里被截断
- **WHEN** 用户复制到的链接少了末尾几个字符
- **THEN** 解码 SHALL 因校验值不匹配失败，提示链接不完整

#### Scenario: 链接已过期
- **WHEN** 链接的过期时间早于当前时间
- **THEN** 系统 SHALL 提示链接已过期，请对方重新生成，不尝试连接

### Requirement: 从文字中宽松提取
系统 SHALL 能从一段文字里找出邀请链接，例如聊天消息「shao 邀请你查看会话：acecode://connect/…」。提取时 SHALL 忽略链接前后的中英文标点、末尾多出的 `/`，并接受被百分号编码过的链接。一段文字里有多条链接时，SHALL 只取第一条。

#### Scenario: 链接前后有中文标点
- **WHEN** 文字是「链接：acecode://connect/<载荷>。」
- **THEN** 系统 SHALL 提取出不含前后标点的链接并成功解码

#### Scenario: 浏览器在末尾加了斜杠
- **WHEN** 浏览器把链接交给 ACECode 时，在末尾多加了一个 `/`
- **THEN** 系统 SHALL 去掉这个斜杠后正常解码

### Requirement: 复制邀请
主人复制邀请时，剪贴板 SHALL 得到一句带链接的说明文字(包含主人显示名、会话标题，并说明要用 ACECode 打开)，方便直接发到聊天软件。分享对话框里 SHALL 同时提供只复制链接本身的操作。

#### Scenario: 复制邀请并发给同事
- **WHEN** 主人点击复制邀请，然后把剪贴板内容粘进聊天软件
- **THEN** 同事收到的文字 SHALL 包含主人显示名、会话标题和完整的邀请链接

### Requirement: 系统协议注册
Windows 上，安装器和 Desktop SHALL 在当前用户范围注册 `acecode` 协议，不需要管理员权限。Desktop 启动时如果发现没有注册，或注册指向的程序已不存在，SHALL 重新注册，指向当前程序；如果注册指向另一个仍然存在的 ACECode 安装，SHALL 保持不动。卸载时 SHALL 只删除指向本次安装的注册。macOS 版 SHALL 在应用信息里声明 `acecode` 协议。

#### Scenario: 绿色版首次启动
- **WHEN** 用户第一次运行没有经过安装器的 Desktop
- **THEN** Desktop SHALL 在当前用户范围注册 `acecode` 协议，指向自身

#### Scenario: 安装版和绿色版同时存在
- **WHEN** 安装版已经注册了协议，用户又运行了一个绿色版 Desktop
- **THEN** 绿色版 SHALL NOT 覆盖安装版的注册

### Requirement: 通过系统协议打开链接
在 Windows 和 macOS 上，用户在浏览器地址栏粘贴并打开邀请链接，或在其它程序里点击它时，系统 SHALL 把链接交给 ACECode Desktop。如果 Desktop 已经在运行，SHALL 由已运行的实例处理，新进程交接后立即退出，不开第二个窗口。如果 Desktop 还没完成启动，链接 SHALL 等页面就绪后再处理，不丢失。

#### Scenario: Desktop 已在运行
- **WHEN** Desktop 已经打开，用户在浏览器里打开邀请链接并同意打开 ACECode
- **THEN** 已运行的 Desktop 窗口 SHALL 被拉到前台并弹出连接确认，不出现第二个 Desktop 进程

#### Scenario: Desktop 没有运行
- **WHEN** Desktop 没有运行，用户在浏览器里打开邀请链接
- **THEN** Desktop SHALL 启动，并在页面就绪后弹出连接确认

### Requirement: 剪贴板识别
Desktop SHALL 识别用户复制到剪贴板的邀请链接。Windows 上 SHALL 在剪贴板变化时检查，包括窗口在后台或最小化到托盘的情况；窗口不在前台时 SHALL 用系统通知提示，点击通知后再弹连接确认。macOS 上 SHALL 只在窗口被激活时检查。同一条链接 SHALL 只提示一次。设置里 SHALL 能关闭剪贴板识别，默认开启。

#### Scenario: 后台时复制链接
- **WHEN** Desktop 最小化在托盘，用户在聊天软件里复制了一条邀请链接
- **THEN** Desktop SHALL 弹出一条系统通知，点击后拉起窗口并显示连接确认

#### Scenario: 再次复制同一条链接
- **WHEN** 用户已经对某条链接做过选择，之后又复制了同一条链接
- **THEN** Desktop SHALL NOT 再次提示

### Requirement: 剪贴板隐私
剪贴板识别 SHALL 只判断文字里是否包含邀请链接；不包含时 SHALL 立刻丢弃读到的内容。剪贴板内容 SHALL NOT 写进日志、持久化或发给任何服务。剪贴板内容带有「不要被监视程序处理」或「不要进剪贴板历史」标记(密码管理器复制密码时通常会设置)时，系统 SHALL 不读取文字内容。

#### Scenario: 从密码管理器复制密码
- **WHEN** 用户从会设置「不要被监视」标记的密码管理器复制了一个密码
- **THEN** Desktop SHALL NOT 读取这次剪贴板的文字内容

#### Scenario: 复制普通文字
- **WHEN** 用户复制了一段不含邀请链接的文字
- **THEN** Desktop SHALL 不提示、不记录，并丢弃读到的内容

### Requirement: 粘贴到 ACECode 输入框
用户把邀请链接粘进 ACECode 的消息输入框时，系统 SHALL 弹出连接确认，而不是把链接插进输入框。用户取消后，链接 SHALL 照常作为文字粘进输入框。这条入口在 Desktop、浏览器里的 Web 界面和安卓上都可用。

#### Scenario: 在 Web 界面里粘贴
- **WHEN** 用户在浏览器打开的 ACECode Web 界面里，把邀请链接粘进输入框
- **THEN** 系统 SHALL 弹出连接确认

#### Scenario: 取消后照常粘贴
- **WHEN** 用户在连接确认上选择取消
- **THEN** 链接文字 SHALL 出现在输入框原本的光标位置

### Requirement: 连接前必须确认
不论链接从哪个入口进来，系统 SHALL 先显示确认：主人显示名、会话标题、将要连接的地址。用户确认前，系统 SHALL NOT 向链接里的任何地址发起网络请求。链接内容来自外部，确认框里的显示名和标题 SHALL 按纯文本显示。

#### Scenario: 网页自动触发链接
- **WHEN** 某个网页在用户不知情时触发了一条邀请链接
- **THEN** ACECode SHALL 只显示确认，用户不确认就不发出任何网络请求

#### Scenario: 显示名里有标签
- **WHEN** 链接里的主人显示名是一段 HTML 标签
- **THEN** 确认框 SHALL 原样把它显示为文字，不渲染
