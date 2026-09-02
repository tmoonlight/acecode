## Context（背景）

- 链接元数据在"本地文件"场景已经端到端打通：`src/markdown/markdown_formatter.cpp` 用 `reflect(region.box)` 把每个链接的屏幕矩形记入 `opts.link_regions`，`src/main.cpp` 的鼠标处理器用 `href_at(mouse.x, mouse.y)` 命中检测后调 `open_tui_chat_file_link()`（`src/tui/chat_file_link.cpp`），后者当前用 `has_url_scheme()` 拒绝一切带 scheme 的 URL。
- markdown 渲染器里的 `make_hyperlink()` 与 `terminal_supports_hyperlinks()` 只有定义、从未被调用。前者只返回显示文本，其注释称"FTXUI Elements 层面发不出 OSC 8"——**该注释基于旧版判断，已过时**：当前 fork 基线（`658c942`，2026-08 的 main）随上游自带完整 OSC 8 支持（见决策 1-3），实际缺口只是渲染器未接线 + 终端探测未调用。
- vendored fork（`external/ftxui`）已有成熟的 ACECode 补丁体系（conhost、drag-autoscroll、mouse-origin、kitty keyboard、`658c942` 的 DEC 2026 同步输出、`ACECODE_PATCHES.md` 记录的 `idle-mouse-redraw` 补丁），改 fork 是既定惯例。值得注意：`idle-mouse-redraw` 当初**特意**把鼠标上报从 `?1003`（any-event）降为 `?1002`（button-event），以消除老 Windows 控制台上悬停移动引发的重绘抖动——SGR 解析器和 `Mouse::Motion::Moved` 事件模型本身已支持 motion，只是终端不发送。
- 父仓库 gitlink 锁定 `658c942`。**孤儿 commit 隐患已解决**（2026-09-01）：shaohaozhi286/FTXUI PR #1 真 merge（`20c99b5d`）后 `658c942` 已是 `main` 祖先，任何 clone 可取；`.gitmodules` 已回切官方 URL。
- OSC 8 支持无法在启动时可靠查询；沿用 DEC 2026 的 env 白名单方法（`detect_synchronized_output_support`），Apple Terminal.app（无 OSC 8）是回归敏感度最高的回退终端。

## Goals / Non-Goals（目标 / 非目标）

**Goals：**

- 聊天区内任何 `http`/`https` 链接点击即用系统默认浏览器打开，两种渲染模式均生效。
- 渲染期中和"文字伪装 URL"（host 不符）的链接；不影响正常命名的链接。
- 在支持的终端上发射 OSC 8，让 Cmd/Ctrl+点击、原生悬停、右键打开/复制生效——终端用户已有的肌肉记忆。
- 悬停时在应用内浮层显示真实 URL，作为人眼兜底。
- 不支持的终端输出与现状字节级一致。
- 孤儿 commit 隐患已在实施前修复（PR #1 合并，见决策 7）。

**Non-Goals：**

- 内容滚入终端历史后仍可点击（报告差距 #8，滚屏改造——本轮只打 OSC 8 地基，收益后置）。
- 完整 URL 匹配校验（只比域名；`faceb00k.com` 这类仿冒域名不归本层管，归浏览器）。
- 老式/经典 Windows conhost 的悬停支持（1003 在那里保持关闭）。
- DA1/DECRQM 查询式探测。
- web UI 或 daemon 的 OSC 8（仅 TUI）。

## Decisions（决策）

### 1. OSC 8 透传采用上游现成实现，不写补丁

核实（2026-09-02）：fork 基线 `658c942` 已随上游 FTXUI 自带全套 OSC 8 支持——`Cell::hyperlink`（`uint8_t`，0 = 无链接，索引 Screen 元数据）、`Screen::RegisterHyperlink(std::string_view) -> uint8_t` + `Hyperlink(uint8_t)`（带去重与 255 上限保护）、`Screen::ToString()` 经 `UpdateCellStyle()` 在 link-id 变化处发射 `\x1B]8;;URL\x1B\\` 开/关序列（行尾/结尾经 default_cell 复位自动关闭）、`ftxui::hyperlink()` 装饰器（`elements.hpp:130-131` + `hyperlink.cpp`）。方案与原设计的"Cell 存 id + Screen URL 表 + ToString 发射 + 装饰器"完全同构（上游用 `uint8_t` 而非 `uint32_t`，上限 255 有保护），**无需任何框架改动**。本变更的框架侧只剩 hover 补丁（决策 5）。

### 2. 行尾关闭与相邻去重已由上游保证

`Screen::ToString()` 在换行处先经 `UpdateCellStyle(..., default_cell)` 复位样式（含 hyperlink），再输出 `\r\n`；结尾同样复位——行尾必关闭 ✓。`UpdateCellStyle()` 只在 `next.hyperlink != prev.hyperlink` 时发射——相邻同 id 不重发 ✓。`hyperlink_test.cpp` 已有字节级断言（开/关/相邻切换）✓。这些验收点 spec 里已有覆盖，无需新增框架测试。

### 3. `ftxui::hyperlink()` 装饰器现成可用

上游 `hyperlink.cpp` 的 `Hyperlink::Render()` 先 `RegisterHyperlink(link_)` 再给区域格子打 `hyperlink` id，API 为 `hyperlink(std::string_view link, Element)` 与装饰器重载 `hyperlink(std::string_view link)`。markdown 渲染器 `is_link` 分支只需在现有颜色/下划线样式之后套用装饰器（检测到 OSC 8 支持时）；`link_regions`（应用内点击用）收集逻辑不变——两条通道共享元数据但代码路径独立。

### 4. 只比域名的防骗校验

纯函数（无 env 依赖、可单测）：显示文字能解析出"URL 形状"（含点号、无空格）时提取 host；host 与链接目标 host 不符 → 降级——不进 `link_regions`、去掉链接样式、按纯文本渲染。标签文字（无 URL 形状）和 host 一致的文字（省略 scheme、截断路径）放行。畸形/非 ASCII 的"URL"按不匹配处理。

### 5. 悬停需要 opt-in 的 `?1003` 补丁加重绘抑制

新增 `App::EnableMouseHoverMotion(bool)`，镜像 `EnableKittyKeyboard` / `EnableSynchronizedOutput` 的 API 风格：开启后 `EnableMouseTracking()` 发 `?1003h`（恢复被 `idle-mouse-redraw` 移除的 any-event 上报），事件循环抑制"无按键 `Mouse::Moved`"引发的帧失效，避免悬停在会抖动的控制台上触发重绘风暴。TUI 仅在终端进入 hover 安全名单时启用；老式/经典 conhost 保持 `?1002`。

### 6. OSC 8 探测迁入 `terminal_capability`

把渲染器里的死代码 `terminal_supports_hyperlinks()` 吸收为 `detect_osc8_support_with()`，沿用既定 blacklist > whitelist > unknown-off 模式，补充 kitty（`KITTY_WINDOW_ID`、`TERM == xterm-kitty`）和 Ghostty 标记。**Apple Terminal.app 有意不进白名单**：无 OSC 8，必须验证无回归。发射本身即使被忽略也无害，白名单误判只是优雅降级。

### 7. 子模块卫生已完成

父仓库 gitlink 锁定的 `658c942` 曾因只在 `refs/pull/1/head`、不在任何命名分支上而构成孤儿 commit 隐患；且 `LIUXIN557` 对 shaohaozhi286/FTXUI 无 push 权限。**已解决（2026-09-01）**：shaohaozhi286/FTXUI PR #1 真 merge（`20c99b5d`，父 = `c2e90617` + `658c942c`），`658c942` 已是 `main` 祖先，任何机器 clone 可取；`.gitmodules` 已回切 `shaohaozhi286/FTXUI.git` 并删除 `branch` 行（`97e6e351`）；子模块 `origin` 指回官方、`myfork`（LIUXIN557/FTXUI）保留作可写备用。`feat/osc8-hyperlink` 分支已从 `658c942` 创建（任务 1.2）。hover 补丁完成后：push 该分支到可写远程 → 更新父仓库 gitlink 并提升 `ports/ftxui` port-version。顺序有讲究：**绝不把 gitlink 前移到未 push 的 commit**。

### 8. A 与 B 单次交付

按既定范围，A 和 B 合为一次交付（一个 openspec 条目、一个应用侧 PR）。tasks.md 内部把 A 侧任务排在前面，这样即使 B 侧 fork 工作受阻，后续拆分交付只是设检查点，不是返工。

## Risks / Trade-offs（风险 / 取舍）

- **A 的时间线被 B 绑定**（单次交付）：设计时已知情接受；在验证前检查点仍可抽出 A 单独上线。
- **OSC 8 与鼠标跟踪的交互**：个别终端在鼠标上报开启时可能把修饰键点击交给应用而非终端。iTerm2、kitty、WezTerm、Windows Terminal 对 OSC 8 span 的 Cmd/Ctrl+点击均正常放行；无论何种情况，应用内点击（A）都是兜底。
- **`?1003` 在老控制台上的回归**：用门控缓解（老式/经典 conhost 关闭）加重绘抑制；`idle-mouse-redraw` 原始动机有文档记录，不是被悄悄回退。
- **行尾关闭**每条换行链接多几个字节，相对现有每格 SGR 流量可忽略。
- **只比域名的校验**拦不住仿冒域名；悬停气泡是人眼兜底，浏览器是最终防线。
- **fork 面积扩大**：仅新增悬停补丁一个，分叉面比原方案（OSC 8 + 悬停两个补丁）小一半；OSC 8 是上游功能零分叉。悬停补丁为 opt-in、记录在 `ACECODE_PATCHES.md`。
