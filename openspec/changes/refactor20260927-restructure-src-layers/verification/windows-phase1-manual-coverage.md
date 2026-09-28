# Windows 人工清单覆盖审查

日期:2026-09-29。对照 [TUI 手工回归清单](../../refactor20260927-split-tui-main/manual-test-checklist.md)。本表区分实窗/真实进程执行与组件测试;未执行人工步骤保持未验收,不把全量单测等同于手工清单全部通过。

| 原清单小节 | 当前证据 | 未完整执行的人工范围 |
| --- | --- | --- |
| 1 启动 | CLI 输出/-c、ConPTY/WinPTY、Windows Terminal/conhost 实窗启动与 alt-screen;启动装配和快照相关用例包含于全量 | 全部启动参数组合、GitHub issue worktree、真实 Copilot 设备登录流程逐项重演 |
| 2 对话 | 四类终端对话、流式响应;ChatViewport、ActivityNarrator、TuiFrameRenderer、todo/goal 用例通过 | 真实终端逐项长会话滚动、心跳与故障重试观感 |
| 3 工具行 | TUI 实际 spawn_subagent 工具行;ToolRowFormatTest、diff、工具摘要与展开相关用例通过 | 实窗多文件 diff、长 MCP JSON、展开折叠组合逐项重演 |
| 4 浮层 | 实窗 model picker;89 条名称含 AskQuestion 的用例、确认框及六状态按键矩阵包含于全量 | 多题自定义输入、拖选复制、全部 picker 与子代理权限排队的实窗组合 |
| 5 输入 | Windows Terminal/conhost 的 Unicode 中文字符输入;ComposerInput、附件、粘贴和焦点组件用例通过 | 真实系统剪贴板大文本/图片、SSH OSC52、完整 Shell 与排队交互 |
| 6 鼠标 | 原生输入 direct/helper 自有窗口冒烟通过;聊天滚动条、选区补偿与 hover 组件用例通过 | TUI 实窗拖选越界滚动、链接点击/气泡等完整手工步骤 |
| 7 中断 | 两个 PTY 后端各自的空闲双击 Ctrl+C;首次 Ctrl+C 保持进程;事件路由中断/取消矩阵通过 | 全部 Esc 取消优先级与 Plan 模式实窗组合 |
| 8 全屏界面 | TuiEventRouter.FullScreenSurfaceOwnsInputUntilChatIsSelected 及设置相关组件用例通过 | 设置页打开期间子代理权限请求、关闭设置后再弹框的真实操作 |
| 9 其它 | 真实 TUI 后台子会话;自动标题与模型 picker;RemoteControlHub、通知生命周期等用例通过 | 真实 IM 账号入站/出站、系统通知点击回到会话 |
| 10 退出 | 两个 PTY 后端各自 /exit、空闲双击 Ctrl+C,子会话调用在途;实窗退出与 resume 提示;Desktop 慢 MCP 退出 | 关闭窗口/Ctrl+Break、完整 worktree 清理操作、微软拼音候选窗位置。POSIX SIGTERM 随跨端补验 |

详细结果与限制见 [一期 Windows 验证记录](windows-phase1-validation.md)。微软拼音候选窗是 P0-08 已保留的人工项;真实账号专项尚无本次执行证据。其余本机组件、终端与关停验证已尽量通过隔离工作区和本地模型桩完成,不使用真实账号。

交付范围决定(用户 2026-09-29 确认):人工专项后补,本次按 Windows 自动化及已完成实测交付。本表未完整执行的人工步骤保留为后续事项,不阻塞本次主线交付;manual-test-checklist.md 的未执行条目仍不勾选。
