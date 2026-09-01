## 1. 子模块卫生（fork 远程）

- [x] 1.1 发布孤儿 commit `658c942`（**已通过 PR #1 合并解决，2026-09-01 23:35 前后确认**）：shaohaozhi286/FTXUI PR #1 真 merge（`20c99b5d`，父 = `c2e90617`+`658c942c`），`658c942` 已是 `main` 祖先，任何 clone 均可取到。`.gitmodules` 已回切 `shaohaozhi286/FTXUI.git` 并删除 `branch` 行（`97e6e351`）；子模块 `origin` 指回官方，`myfork`（LIUXIN557/FTXUI）保留作可写备用。
- [ ] 1.2 在 `external/ftxui` 从 `658c942` 创建 `feat/osc8-hyperlink` 分支（分支留在与 1.1 相同的可写远程上）。

## 2. FTXUI 补丁：OSC 8 透传

- [ ] 2.1 给 `Cell` 加 `uint32_t link_id`（`include/ftxui/screen/cell.hpp`），默认 0，`ACECODE-PATCH(osc8-hyperlink)` 标记。
- [ ] 2.2 增加 `Screen` 级链接 URL 表 + `RegisterLink(std::string) -> uint32_t`（`include/ftxui/screen/screen.hpp`、`src/ftxui/screen/screen.cpp`）。
- [ ] 2.3 在 `Screen::ToString(std::string&)` 中按 link-id 变化发射 OSC 8 开/关：与样式状态并行跟踪当前 id、每行行尾关闭、相邻同 id 不重发、空格继承当前 id。
- [ ] 2.4 新增 `ftxui::hyperlink(Element, std::string url)` 装饰器（`include/ftxui/dom/elements.hpp` + dom 源文件）：注册 URL、给区域格子打 `link_id`。
- [ ] 2.5 `src/ftxui/screen/screen_test.cpp` 单测：开/关序列的精确字节断言、行尾关闭、相邻同 id 去重、混合样式、无链接时输出与旧版一致。
- [ ] 2.6 在 `external/ftxui/ACECODE_PATCHES.md` 登记该补丁。

## 3. FTXUI 补丁：悬停移动（`?1003` opt-in）

- [ ] 3.1 新增 `App::EnableMouseHoverMotion(bool)`（`include/ftxui/component/app.hpp`），镜像 `EnableKittyKeyboard`。
- [ ] 3.2 `src/ftxui/component/app.cpp`：开启时 `EnableMouseTracking()`/退出清理发 `?1003h`/`?1003l` 替代 `?1002`；抑制无按键 `Mouse::Moved` 事件的帧失效。
- [ ] 3.3 更新 `app_test.cpp` 期望序列；写入 `ACECODE_PATCHES.md`（交叉引用 `idle-mouse-redraw` 的动机）。

## 4. 应用侧：探测与 markdown 接线

- [ ] 4.1 在 `src/utils/terminal_capability.hpp/.cpp` 增加 `detect_osc8_support_with()`（env 可注入纯函数）+ `detect_osc8_support()` 包装，吸收 `terminal_supports_hyperlinks()` 并补 kitty/Ghostty 标记；Apple Terminal.app 不进白名单。
- [ ] 4.2 增加 hover 安全探测（同模式；老式/经典 conhost 关闭），用于门控 `EnableMouseHoverMotion`。
- [ ] 4.3 `tests/utils/terminal_capability_test.cpp` 覆盖每个白名单/黑名单/未知项的单测。
- [ ] 4.4 实现只比域名的防骗校验 helper（markdown 或 tui utils；纯函数、可单测），在 `src/markdown/markdown_formatter.cpp` 接入：不匹配的链接不进 `link_regions`、去链接样式。
- [ ] 4.5 检测到 OSC 8 时用 `ftxui::hyperlink(e, style.href)` 包裹 `is_link` span；删除 `make_hyperlink()` 和渲染器内的 `terminal_supports_hyperlinks()` 死代码。
- [ ] 4.6 校验 helper 单测（伪装 host 降级、标签文字放行、host 一致的截断文字放行、畸形 URL 降级）。

## 5. 应用侧：点击打开与悬停气泡

- [ ] 5.1 在聊天链接打开器（`src/tui/chat_file_link.cpp` 或新建 `src/utils/open_url.cpp`）加 http/https 分支：macOS `open`、Linux `xdg-open`、Windows `start`；失败经状态栏提示，绝不崩溃。
- [ ] 5.2 `src/main.cpp`：`App::Loop()` 前按探测结果启用 `EnableMouseHoverMotion` 与 OSC 8。
- [ ] 5.3 `src/main.cpp`：无按键 `Mouse::Moved` 时命中检测 `link_regions`；停留约 300 ms 后在指针附近渲染浮层显示真实 URL；移开/Esc 隐藏。气泡不得抢焦点、不得挤压布局。
- [ ] 5.4 `tests/tui/` 单测 URL 打开命令构造（可 mock 的 runner）。

## 6. 集成与验收

- [ ] 6.1 更新父仓库 gitlink 指向已 push 的 `feat/osc8-hyperlink` 头；提升 `ports/ftxui/vcpkg.json` 的 port-version。
- [ ] 6.2 重编 ftxui + `acecode` + `acecode_unit_tests`；跑全量单测 + 新增聚焦测试。
- [ ] 6.3 `openspec validate add-tui-hyperlinks --strict`。
- [ ] 6.4 人工矩阵（用户验证）：iTerm2（Cmd+点击可开、原生悬停、右键打开/复制）、Apple Terminal.app（字节级回退：无 OSC 8、应用内点击照常、无悬停回归）、Windows Terminal（Ctrl+点击可开、悬停无闪屏）。
- [ ] 6.5 在验证备注中记录顺带核实的终端（kitty、WezTerm、老式 conhost）。

## Verification

