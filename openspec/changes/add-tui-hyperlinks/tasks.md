## 1. 子模块卫生（fork 远程）

- [x] 1.1 发布孤儿 commit `658c942`（**已通过 PR #1 合并解决，2026-09-01 23:35 前后确认**）：shaohaozhi286/FTXUI PR #1 真 merge（`20c99b5d`，父 = `c2e90617`+`658c942c`），`658c942` 已是 `main` 祖先，任何 clone 均可取到。`.gitmodules` 已回切 `shaohaozhi286/FTXUI.git` 并删除 `branch` 行（`97e6e351`）；子模块 `origin` 指回官方，`myfork`（LIUXIN557/FTXUI）保留作可写备用。
- [x] 1.2 在 `external/ftxui` 从 `658c942` 创建 `feat/osc8-hyperlink` 分支（分支留在与 1.1 相同的可写远程上）。**2026-09-02 完成**：`git checkout -b feat/osc8-hyperlink`（基线 `658c942c`），本地分支已建，待 hover 补丁完成后 push 到 `myfork`（LIUXIN557/FTXUI）。

## 2. FTXUI 框架核实：OSC 8 能力（2026-09-02 结论：上游已有，零代码）

- [x] 2.1 核实 `Cell` 链接字段：上游已有 `uint8_t hyperlink`（`include/ftxui/screen/cell.hpp:42-45`），0 = 无链接，索引 Screen 元数据。
- [x] 2.2 核实 `Screen` URL 表：已有 `RegisterHyperlink(std::string_view) -> uint8_t` + `Hyperlink(uint8_t)`（`screen.hpp:86-89`、`screen.cpp:582-600`），带去重与 255 上限保护。
- [x] 2.3 核实 `ToString()` 发射：`UpdateCellStyle()`（`screen.cpp:83-87`）在 hyperlink 变化时发 `\x1B]8;;URL\x1B\\`；行尾/结尾经 default_cell 复位自动关闭。
- [x] 2.4 核实装饰器：`ftxui::hyperlink()`（`elements.hpp:130-131` + `src/ftxui/dom/hyperlink.cpp`）已实现，注册 URL 后给区域格子打 id。
- [x] 2.5 核实单测：`hyperlink_test.cpp` 已有字节级断言（开/关/相邻切换），spec 的行尾关闭/相邻去重验收点已覆盖。
- [x] 2.6 补丁登记：无需——OSC 8 为上游功能（Copyright 2023 Arthur Sonzogni），非本地补丁，不入 `ACECODE_PATCHES.md`。

## 3. FTXUI 补丁：悬停移动（`?1003` opt-in）

- [x] 3.1 新增 `App::EnableMouseHoverMotion(bool)`（`include/ftxui/component/app.hpp`），镜像 `EnableKittyKeyboard`。**2026-09-02 完成**。
- [x] 3.2 `src/ftxui/component/app.cpp`：开启时 `EnableMouseTracking()` 发 `?1003h`（`kMouseAnyEvent`）替代 `?1002`，uninstall 注册对称发 `?1003l`；`RunOnce()` 将无按键 `Mouse::Moved` 分类为 passive——仍分发组件但不使 `frame_valid_` 失效（组件自行 `RequestAnimationFrame`）。**2026-09-02 完成**（uninstall 起初漏发 `?1003l`，测试抓出后修复）。
- [x] 3.3 更新 `app_test.cpp` 期望序列（新增 `MouseHoverMotionDisabledByDefault` / `MouseHoverMotionEnabledSendsAnyEventTracking` 两个测试）；写入 `ACECODE_PATCHES.md`（`hover-motion` 段，交叉引用 `idle-mouse-redraw` 的动机）。**2026-09-02 完成**。ftxui 全量单测 362/362 通过。

## 4. 应用侧：探测与 markdown 接线

- [x] 4.1 在 `src/utils/terminal_capability.hpp/.cpp` 增加 `detect_osc8_support_with()`（env 可注入纯函数）+ `detect_osc8_support()` 包装，吸收 `terminal_supports_hyperlinks()` 并补 kitty/Ghostty 标记；Apple Terminal.app 不进白名单。**2026-09-02 完成**：决策表 blacklist > whitelist > unknown-off；收紧死代码的 `TERM 含 "xterm"` 宽匹配为只认 `xterm-kitty`（裸 xterm-256color 被大量终端伪装，保守关）；`detect_osc8_support()` 先跑完整 `detect_terminal_capabilities()` 使 Windows conhost 黑名单在真实环境生效。
- [x] 4.2 增加 hover 安全探测（同模式；老式/经典 conhost 关闭），用于门控 `EnableMouseHoverMotion`。**2026-09-02 完成**：`detect_hover_motion_support_with()`/`detect_hover_motion_support()`，白名单与 OSC 8 一致，conhost 家族强制关（?1003 重绘抖动，idle-mouse-redraw 动机），语义独立便于未来单独演化。
- [x] 4.3 `tests/utils/terminal_capability_test.cpp` 覆盖每个白名单/黑名单/未知项的单测。**2026-09-02 完成**：Osc8Support 13 个 + HoverMotionSupport 7 个，46/46 通过（含原有 TerminalCapability/SynchronizedOutputSupport）。
- [x] 4.4 实现只比域名的防骗校验 helper（`src/markdown/link_safety.hpp/.cpp`，纯函数、可单测），在 `src/markdown/markdown_formatter.cpp` 接入：不匹配的链接不进 `link_regions`、去链接样式。**2026-09-02 完成**：`flatten_inline` Link 分支先拼完整显示文字（`link_display_text` 递归 children），`is_safe_link_label` 校验失败则不加 `is_link`/`href`。防骗只针对含 `://` 的远程 href；`[foo.md](docs/foo.md)` 这类本地相对路径放行（文件名带点号不误降级）。首次实现把 `docs/evil.md` 误当远程 URL（无 `://` 前缀的相对路径被解析出 host），单测抓出后修正。
- [x] 4.5 检测到 OSC 8 时用 `ftxui::hyperlink(e, style.href)` 包裹 `is_link` span；删除 `make_hyperlink()` 和渲染器内的 `terminal_supports_hyperlinks()` 死代码。**2026-09-02 完成**：`FormatOptions` 新增 `osc8_hyperlinks`（默认 false，无回归），`apply_style` is_link 分支在 link_regions 收集后套装饰器；两条通道独立。main.cpp 按探测结果置 flag 属 5.2。
- [x] 4.6 校验 helper 单测（伪装 host 降级、标签文字放行、host 一致的截断文字放行、畸形 URL 降级）。**2026-09-02 完成**：`tests/markdown/link_safety_test.cpp` 12 个用例（extract 8 + 决策 4 组），12/12 通过。修正了 3 个脱离现实的断言（`mailto:` 宽松解析、`"https://"` 无点号不算 URL 形状、全角句号非 ASCII 点）。

## 5. 应用侧：点击打开与悬停气泡

- [x] 5.1 在聊天链接打开器加 http/https 分支：新建 `src/utils/open_url.{hpp,cpp}`（`is_openable_http_url` + `open_url_in_browser`，launcher 可注入），`src/tui/chat_file_link.cpp` 在 resolve 前接 URL 分支。**2026-09-02 完成**：仅 http/https、拦截控制字符（含 ESC 0x1B 终端转义注入）、裸 `http://` 拒绝；默认 launcher 不经 shell（POSIX fork+execlp 调 `open`/`xdg-open`，Windows `ShellExecuteW`），URL 原样作参数无注入面；失败经状态栏提示不崩溃。
- [x] 5.2 `src/main.cpp` 按探测结果接线：screen 初始化处 `EnableMouseHoverMotion(detect_hover_motion_support())`（hover 补丁新增 API），`render_message_markdown` 的 FormatOptions 置 `osc8_hyperlinks = detect_osc8_support()`（static 缓存，避免 Windows 每帧重复 console probe）。**2026-09-02 完成**：探测结果缓存为 `hover_supported` 变量，同时传给渲染层 gate 气泡（只探测一次）。
- [x] 5.3 `src/main.cpp` 悬停气泡：无按键 `Mouse::Moved` 命中检测 `link_regions`（拖动中不显示）；`anim_thread` 时间门 300ms 到期置 visible 并强制渲染；气泡用 `dbox` 叠加整屏（共享区域，不参与布局、DOM 元素不捕获输入），指针右上方优先、空间不足翻侧，坐标 clamp 防撑大 dbox 需求；移出/Esc/任意按键隐藏；`hover_supported=false`（conhost 等）恒不渲染。**2026-09-02 完成**：状态字段 5 个（`hover_link_href/since/visible/x/y`，mu 保护）入 `TuiState`；延迟常量 `kLinkHoverTooltipDelayMs=300` 入 `src/tui/thinking_animation.hpp`；`TuiRendererContext` 增 `hover_supported`。
- [x] 5.4 URL 打开命令构造单测。**2026-09-02 完成**：`tests/utils/open_url_test.cpp` 6 个用例（http/https 放行、ftp/file/javascript/mailto 拒绝、空/空白/ESC/裸 scheme 拒绝、mock launcher 收到 URL、非法 URL 不触碰 launcher、launcher 失败传播错误），6/6 通过。

## 6. 集成与验收

- [ ] 6.1 更新父仓库 gitlink 指向已 push 的 `feat/osc8-hyperlink` 头；提升 `ports/ftxui/vcpkg.json` 的 port-version。
- [ ] 6.2 重编 ftxui + `acecode` + `acecode_unit_tests`；跑全量单测 + 新增聚焦测试。
- [ ] 6.3 `openspec validate add-tui-hyperlinks --strict`。
- [ ] 6.4 人工矩阵（用户验证）：iTerm2（Cmd+点击可开、原生悬停、右键打开/复制）、Apple Terminal.app（字节级回退：无 OSC 8、应用内点击照常、无悬停回归）、Windows Terminal（Ctrl+点击可开、悬停无闪屏）。
- [ ] 6.5 在验证备注中记录顺带核实的终端（kitty、WezTerm、老式 conhost）。

## Verification

