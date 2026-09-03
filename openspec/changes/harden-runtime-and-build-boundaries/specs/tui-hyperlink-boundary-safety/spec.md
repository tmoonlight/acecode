## ADDED Requirements

### Requirement: 链接 host 必须来自真实 authority
系统 MUST 在处理 userinfo 前切分 HTTP(S) authority，并且路径、反斜杠、query 或 fragment 中的 `@` MUST NOT 改变用于安全比较的 host。

#### Scenario: 路径中的 at 符号不覆盖 host
- **WHEN** href 为 `https://evil.example/@trusted.example`
- **THEN** 安全比较使用的 host 为 `evil.example`

#### Scenario: query 中的 at 符号不覆盖 host
- **WHEN** href 为 `https://evil.example/?next=@trusted.example`
- **THEN** 安全比较使用的 host 为 `evil.example`

#### Scenario: authority 中的 userinfo 仍受支持
- **WHEN** href 为 `https://user:pass@example.com/path`
- **THEN** 安全比较使用的 host 为 `example.com`

### Requirement: 无法验证的远端 URL 形链接必须降级
当显示文字呈 URL 形状，而远端 HTTP(S) href 无法解析出可靠 host 时，系统 SHALL 将其降级为不可点击纯文本；普通命名标签不受影响。

#### Scenario: 畸形远端目标默认不可点击
- **WHEN** URL 形显示文字对应的 HTTP(S) href authority 为空或畸形
- **THEN** 链接不进入可点击区域且不带链接样式

#### Scenario: 普通标签保持既有行为
- **WHEN** 显示文字不是 URL 形状
- **THEN** 合法 HTTP(S) href 仍可点击

### Requirement: 悬停气泡必须适配终端 cell 宽度
链接 tooltip SHALL 按终端 cell 宽度截断并保证浮层不宽于当前终端；无法容纳边框的极窄终端 SHALL 不绘制气泡。

#### Scenario: 长 Unicode URL 出现在窄终端
- **WHEN** tooltip URL 的 cell 宽度超过可用终端宽度
- **THEN** 文本在有效 UTF-8 边界截断，浮层宽度不超过终端

### Requirement: POSIX URL 启动子进程必须被回收
系统 SHALL 在 POSIX 平台回收用于执行系统 URL opener 的直接子进程，且不得改变主 TUI 的全局 SIGCHLD 策略。

#### Scenario: opener 进程退出
- **WHEN** `xdg-open` 或 `open` 子进程结束
- **THEN** 后台 waiter 调用 `waitpid` 回收该子进程，不留下 zombie
