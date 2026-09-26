# TUI 手工回归清单(refactor20260927-split-tui-main)

每个 B 任务合入前,至少跑对应小节;P6B 完成时跑全部小节。环境:Windows Terminal、conhost(经典控制台)、macOS 或 Linux 终端,各跑一轮。结果写进提交说明,格式为「小节号 + 环境 + 通过 / 发现」。

## 1. 启动

- [ ] 普通启动;`--resume`、`-c`、`-r`(打开 resume picker);`--worktree foo`、`--worktree #123`;`--alt-screen`;`--dangerous`;`--question-policy`。
- [ ] 横幅、legacy 终端提示(只提示一次)、↑ 调出输入历史。
- [ ] hook 顺序:`startup.before_model_load` → `startup.models_loaded` → `session_start` → `title_changed`(resume 时)。
- [ ] Copilot 未登录时的设备码流程:系统行出现在 conversation 里的位置与改造前一致。
- [ ] MCP 侧栏的 loading 与连接状态;配置了 MCP 时,首回合看到的工具表与改造前一致。
- [ ] 启动快照:普通启动、`--resume`、Copilot 未登录、配置了 MCP 四种场景下,`state.conversation` 的前 N 条与 P0-12 记录的快照逐条一致。

## 2. 对话

- [ ] 流式输出跟随到底部;上滚之后不会被拉回;短对话底部锚定。
- [ ] 推理指示行右侧的心跳 `[Ns · ↓ X tokens]`。
- [ ] `● Done for Ns` 只在正常完成、未被用户中断、耗时至少 1 秒时出现。
- [ ] todo 更新;`/goal` 状态 chip;模型重试提示。

## 3. 工具行

- [ ] 三态灯:执行中为灰、成功为绿、失败为红;abort 后孤立的调用行一直保持灰色。
- [ ] Ctrl+O 全局展开或折叠;Ctrl+E 逐行展开;失败时显示前 3 行。
- [ ] apply_patch 多文件时插入文件标题行;diff 最多 3 块、每块 20 行。
- [ ] 超长的单行 MCP JSON 按可视行折叠成 3 行。

## 4. 浮层

- [ ] 确认框:数字键、a、Shift+Tab、Esc;多个子代理的权限请求依次排队弹出,并标注来源。
- [ ] AskUserQuestion:多道题、自定义答案、滚动条拖动、文本拖选、右键复制;超时倒计时。
- [ ] `/rewind`、`/fork`、`/resume`、`/model`、`/mode` 各个 picker;`@` 路径补全;`/` 命令补全。
- [ ] 吞键矩阵抽查:picker 打开时 Ctrl+E 仍然切换聚焦的 tool_result;Ctrl+A、Alt+↑/↓、←/→、可打印字符被吞。

## 5. 输入

- [ ] Ctrl+V 粘贴大段文本并折叠成块;Alt+V 粘贴图片附件;右键复制,SSH 下走 OSC 52;Alt+A。
- [ ] 中文字形与粘贴块作为整体移动光标;删除选区。
- [ ] `!` 进入 Shell 模式;忙时提交的消息进入排队队列。
- [ ] 任何输入变化都会取消 Ctrl+C 的退出武装。

## 6. 鼠标

- [ ] 滚轮滚动;拖动聊天区滚动条与侧栏滚动条;越界拖选时自动滚动。
- [ ] 链接悬停出现气泡、点击打开;按下鼠标时气泡隐藏。

## 7. 中断

- [ ] 忙时 Ctrl+C 中断当前回合,Done 行不追加;空闲时连按两次 Ctrl+C 退出,只按一次不退出。
- [ ] Esc 的取消顺序:picker → Shell 模式 → 附件 → 中断回合。
- [ ] Shift+Tab 切换权限模式,包括进入与退出 Plan 模式。

## 8. 全屏界面

- [ ] 打开设置中心与管理中心;修改 MCP 配置后生效。
- [ ] **设置页打开期间,子代理发出权限请求;关闭设置页后,确认框才弹出**(事件旁路的现状,必须保持)。
- [ ] 关闭全屏界面后回到对话,输入框获得焦点。

## 9. 其它

- [ ] 子代理:运行中的任务显示在侧栏;`/tasks list|abort|clear`。
- [ ] IM 远程控制的入站提交与出站转发;Windows 通知点击后回到会话。
- [ ] 自动标题生成;`/model` 切换后下一回合生效。

## 10. 退出

- [ ] `/exit`、连按两次 Ctrl+C、关闭窗口、Ctrl+Break、SIGTERM:都打印 resume 提示,且不残留鼠标追踪。
- [ ] `--worktree` 启动后退出:无变更时删除,有变更时保留并提示;会话中途 `EnterWorktree` 进入、无变更时,退出也会删除。
- [ ] 断网状态下退出不卡住。
- [ ] Windows 上微软拼音候选窗的位置与删除 IME 代码之前一致(对应 restructure P0-08)。
