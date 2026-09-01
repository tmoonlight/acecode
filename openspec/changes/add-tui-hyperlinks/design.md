## Context（背景）

- 链接元数据在"本地文件"场景已经端到端打通：`src/markdown/markdown_formatter.cpp` 用 `reflect(region.box)` 把每个链接的屏幕矩形记入 `opts.link_regions`，`src/main.cpp` 的鼠标处理器用 `href_at(mouse.x, mouse.y)` 命中检测后调 `open_tui_chat_file_link()`（`src/tui/chat_file_link.cpp`），后者当前用 `has_url_scheme()` 拒绝一切带 scheme 的 URL。
- markdown 渲染器里的 `make_hyperlink()` 与 `terminal_supports_hyperlinks()` 只有定义、从未被调用。前者只返回显示文本，因为 FTXUI 没有透传任意转义序列的通道：每帧都经 `Screen::ToString()` 输出，逐格 diff `Cell`、在样式变化处经 `UpdateCellStyle()` 发 SGR——只有字符数据和属性。
- vendored fork（`external/ftxui`）已有成熟的 ACECode 补丁体系（conhost、drag-autoscroll、mouse-origin、kitty keyboard、`658c942` 的 DEC 2026 同步输出、`ACECODE_PATCHES.md` 记录的 `idle-mouse-redraw` 补丁），改 fork 是既定惯例。值得注意：`idle-mouse-redraw` 当初**特意**把鼠标上报从 `?1003`（any-event）降为 `?1002`（button-event），以消除老 Windows 控制台上悬停移动引发的重绘抖动——SGR 解析器和 `Mouse::Motion::Moved` 事件模型本身已支持 motion，只是终端不发送。
- 父仓库 gitlink 锁定 `658c942`，该 commit 从未 push 到 fork 远程（shaohaozhi286/FTXUI 仅有 `main` 分支）。在孤儿 commit 上继续叠补丁会让风险越滚越大。
- OSC 8 支持无法在启动时可靠查询；沿用 DEC 2026 的 env 白名单方法（`detect_synchronized_output_support`），Apple Terminal.app（无 OSC 8）是回归敏感度最高的回退终端。

## Goals / Non-Goals（目标 / 非目标）

**Goals：**

- 聊天区内任何 `http`/`https` 链接点击即用系统默认浏览器打开，两种渲染模式均生效。
- 渲染期中和"文字伪装 URL"（host 不符）的链接；不影响正常命名的链接。
- 在支持的终端上发射 OSC 8，让 Cmd/Ctrl+点击、原生悬停、右键打开/复制生效——终端用户已有的肌肉记忆。
- 悬停时在应用内浮层显示真实 URL，作为人眼兜底。
- 不支持的终端输出与现状字节级一致。
- 在叠加新补丁前先修掉孤儿 commit 隐患。

**Non-Goals：**

- 内容滚入终端历史后仍可点击（报告差距 #8，滚屏改造——本轮只打 OSC 8 地基，收益后置）。
- 完整 URL 匹配校验（只比域名；`faceb00k.com` 这类仿冒域名不归本层管，归浏览器）。
- 老式/经典 Windows conhost 的悬停支持（1003 在那里保持关闭）。
- DA1/DECRQM 查询式探测。
- web UI 或 daemon 的 OSC 8（仅 TUI）。

## Decisions（决策）

### 1. Cell 存链接 *id*，不存 URL 字符串

`Cell` 新增 `uint32_t link_id = 0`（0 = 无链接）。`Screen` 持有每帧 `std::vector<std::string>` URL 表和 `RegisterLink(std::string) -> uint32_t` 分配器。每格塞 `std::string` 会让 `dimx * dimy` 网格的每格多 24-32 字节、且在每次 diff 中被复制；id 只有 4 字节，相邻格比较也变成平凡操作。

### 2. 在 `Screen::ToString()` 里按 link-id 变化发射 OSC 8

输出循环本来就跨格跟踪样式状态（`previous_cell_ref` + `UpdateCellStyle`）。在其基础上再跟踪当前 `link_id`：变化时发 `ESC ]8;;URL ST`（开）和 `ESC ]8;;ST`（关）；相邻同 id 不重发。**每行行尾必关闭**（OSC 8 跨 `\r\n` 的行为各终端不一致，关掉重开普适安全）。空格格继承当前 link id，链接 span 内的空格也保持可点。

### 3. `ftxui::hyperlink(Element, url)` 装饰器作为公共 API

风格对齐 `color()` / `underlined()`。渲染时向 screen 注册 URL，并给自己区域的每个格子打上 `link_id`。markdown 渲染器的 `is_link` 分支在现有颜色/下划线样式之后套用；`link_regions`（应用内点击用）收集逻辑不变——两条通道共享元数据但代码路径独立。

### 4. 只比域名的防骗校验

纯函数（无 env 依赖、可单测）：显示文字能解析出"URL 形状"（含点号、无空格）时提取 host；host 与链接目标 host 不符 → 降级——不进 `link_regions`、去掉链接样式、按纯文本渲染。标签文字（无 URL 形状）和 host 一致的文字（省略 scheme、截断路径）放行。畸形/非 ASCII 的"URL"按不匹配处理。

### 5. 悬停需要 opt-in 的 `?1003` 补丁加重绘抑制

新增 `App::EnableMouseHoverMotion(bool)`，镜像 `EnableKittyKeyboard` / `EnableSynchronizedOutput` 的 API 风格：开启后 `EnableMouseTracking()` 发 `?1003h`（恢复被 `idle-mouse-redraw` 移除的 any-event 上报），事件循环抑制"无按键 `Mouse::Moved`"引发的帧失效，避免悬停在会抖动的控制台上触发重绘风暴。TUI 仅在终端进入 hover 安全名单时启用；老式/经典 conhost 保持 `?1002`。

### 6. OSC 8 探测迁入 `terminal_capability`

把渲染器里的死代码 `terminal_supports_hyperlinks()` 吸收为 `detect_osc8_support_with()`，沿用既定 blacklist > whitelist > unknown-off 模式，补充 kitty（`KITTY_WINDOW_ID`、`TERM == xterm-kitty`）和 Ghostty 标记。**Apple Terminal.app 有意不进白名单**：无 OSC 8，必须验证无回归。发射本身即使被忽略也无害，白名单误判只是优雅降级。

### 7. 打补丁前先做子模块卫生

父仓库 gitlink 锁定的 `658c942` 只存在于 `refs/pull/1/head`（PR #1 的 head），不在任何命名分支上；`git submodule update` 默认只 fetch `refs/heads/*`，故全新 clone / CI 取不到它。此外已实测：`LIUXIN557` 身份对 fork 远程（shaohaozhi286/FTXUI）**无 push 权限**。因此发布该 commit 的路径按优先级为：① fork owner 合并 PR #1 或授予 push 权限后，推 `feat/synchronized-output`；② 改用 LIUXIN557 自己的 fork 作可写远程（同步 `.gitmodules` 的 URL）；③ 最后手段：gitlink 回退 `main`（c2e90617）、放弃 DEC 2026 补丁。确定可写远程后：（1）发布 `658c942`；（2）从它开 `feat/osc8-hyperlink`；（3）OSC 8 与悬停两个补丁以独立 commit 落在分支上；（4）push 该分支；（5）更新父仓库 gitlink 并提升 `ports/ftxui` port-version。顺序有讲究：**绝不把 gitlink 前移到未 push 的 commit**。

### 8. A 与 B 单次交付

按既定范围，A 和 B 合为一次交付（一个 openspec 条目、一个应用侧 PR）。tasks.md 内部把 A 侧任务排在前面，这样即使 B 侧 fork 工作受阻，后续拆分交付只是设检查点，不是返工。

## Risks / Trade-offs（风险 / 取舍）

- **A 的时间线被 B 绑定**（单次交付）：设计时已知情接受；在验证前检查点仍可抽出 A 单独上线。
- **OSC 8 与鼠标跟踪的交互**：个别终端在鼠标上报开启时可能把修饰键点击交给应用而非终端。iTerm2、kitty、WezTerm、Windows Terminal 对 OSC 8 span 的 Cmd/Ctrl+点击均正常放行；无论何种情况，应用内点击（A）都是兜底。
- **`?1003` 在老控制台上的回归**：用门控缓解（老式/经典 conhost 关闭）加重绘抑制；`idle-mouse-redraw` 原始动机有文档记录，不是被悄悄回退。
- **行尾关闭**每条换行链接多几个字节，相对现有每格 SGR 流量可忽略。
- **只比域名的校验**拦不住仿冒域名；悬停气泡是人眼兜底，浏览器是最终防线。
- **fork 面积扩大**：两个新补丁加大与上游 FTXUI 的分叉。缓解：都是 opt-in、记录在 `ACECODE_PATCHES.md`、且有上游化潜力（上游 FTXUI 无 OSC 8 支持，我们的补丁是合理的贡献候选）。
