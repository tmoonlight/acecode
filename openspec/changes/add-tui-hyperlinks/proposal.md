## Why

TUI 对比报告（`docs/tui-comparison/report.md`，差距 #3）要求"真正可点击的链接"。对照 `origin/master`（v0.9.9）核实，该差距实际只完成约 1/3：本地文件路径可以点击并在系统文件管理器中打开（经 `link_regions` + `open_tui_chat_file_link`），但网页链接（`http`/`https`）被 `has_url_scheme()` 判掉、点击无反应；终端也从未收到 OSC 8 序列——`src/markdown/markdown_formatter.cpp` 里的 `make_hyperlink()` 是死代码，只返回显示文本并丢弃 URL。用户只能手动选中、复制、切到浏览器粘贴；终端原生的 Cmd/Ctrl+点击、悬停、右键打开/复制能力全部缺失。

## What Changes

- **A. 应用内网页链接可点击。** `open_tui_chat_file_link()` 增加 `http`/`https` 分支，用系统默认浏览器打开（macOS `open` / Linux `xdg-open` / Windows `start`），与本地文件链接共用现有 `link_regions` 命中检测路径。
- **A. 防骗校验。** markdown 渲染时，显示文字"长得像 URL"但 host 与真实 URL 不符的链接降级为纯文本（无链接色、无下划线、不进 `link_regions`）；正常命名的链接（"README"、"安装指南"）和 host 一致的 URL 形文字保持可点击。**只比域名，不比完整路径。**
- **B. OSC 8 原生超链接（上游已有，直接采用）。** 核实发现 vendored fork（`external/ftxui`，基线 `658c942`）已随上游 FTXUI 自带完整 OSC 8 支持：`Cell::hyperlink` 字段、`Screen::RegisterHyperlink()`/`Hyperlink()` URL 表、`Screen::ToString()` 在 link-id 变化处发射 OSC 8 开/关序列（行尾自动关闭）、`ftxui::hyperlink()` 装饰器，且已有字节级单测（`hyperlink_test.cpp`）。**无需框架补丁**——markdown 渲染器在检测到终端支持 OSC 8 时用现成装饰器包裹链接 span；删除 `make_hyperlink()` 死代码（其"FTXUI 无法透传"的注释基于旧版判断，已过时）。
- **B. 悬停气泡（`?1003` opt-in，唯一需要的框架补丁）。** fork 新增 `App::EnableMouseHoverMotion(bool)`，恢复 any-event 鼠标上报（`idle-mouse-redraw` 补丁当前强制 `?1002`），并抑制无按键悬停事件引发的无效重绘。指针在链接区域停留约 300 ms 后，TUI 在鼠标附近浮层显示真实 URL。在不安全的终端（老式/经典 conhost）上保持关闭。
- **终端探测。** 把 `terminal_supports_hyperlinks()` 的死代码逻辑迁入 `src/utils/terminal_capability`，做成可注入 env 的纯函数（沿用 `detect_synchronized_output_support` 的 blacklist > whitelist > unknown-off 模式），补充 kitty 标记。不支持 OSC 8 的终端（notably Apple Terminal.app）行为与现状完全一致。
- **子模块卫生（已完成）。** 孤儿 commit `658c942` 已通过 shaohaozhi286/FTXUI PR #1 合并（`20c99b5d` 真 merge）进入 `main` 历史；`.gitmodules` 已回切官方 URL 并删除 `branch` 行（`97e6e351`）；`feat/osc8-hyperlink` 分支已从 `658c942` 创建，补丁落地后 push 到可写远程并更新父仓库 gitlink。

## Capabilities

### New Capabilities

- `tui-hyperlinks`：聊天区网页链接可点击（应用内点击开系统浏览器）、伪装 URL 的显示文字在渲染期被中和、支持终端发射 OSC 8 原生超链接、悬停显示真实 URL 气泡。

### Modified Capabilities

无。终端能力探测（`terminal_capability`）新增 helper，现有结构体、签名、行为不变。

## Impact

- `external/ftxui`（分支 `feat/osc8-hyperlink`，**一个**补丁 commit）：`include/ftxui/component/app.hpp` + `src/ftxui/component/app.cpp`（`EnableMouseHoverMotion`、`?1003` 重开 + 重绘抑制）、`src/ftxui/component/app_test.cpp`、`ACECODE_PATCHES.md`。OSC 8 部分零改动（上游已有）。
- `ports/ftxui/vcpkg.json`：port-version 提升以强制重编（若 hover 补丁落地）。
- `src/utils/terminal_capability.hpp/.cpp`：`detect_osc8_support_with()` 纯函数 + 真实 env 包装（吸收并替换 `terminal_supports_hyperlinks()` 死代码）。
- `src/markdown/markdown_formatter.cpp` 及相关头文件：防骗校验 helper、`ftxui::hyperlink` 接线、死代码删除。
- `src/tui/chat_file_link.cpp`（或新建 `src/utils/open_url.cpp`）：http/https 打开分支。
- `src/main.cpp`：悬停气泡元素 + 现有 `link_regions` 命中检测循环中的鼠标移动事件处理。
- `tests/markdown/`、`tests/tui/`、`tests/utils/`：校验、OSC 8 发射、探测的单元覆盖。
- 不涉及协议、daemon、web 改动。
