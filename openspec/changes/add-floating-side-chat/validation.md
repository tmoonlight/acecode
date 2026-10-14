# Validation

Validated in the current Windows checkout on 2026-09-15. No release or installed daemon replacement was performed.

## Web

- `pnpm i18n:catalog`: regenerated and reviewed English overrides for the new copy.
- `pnpm test`: full Web suite passed, including conversation lifecycle, private stream transport, geometry, command routing and source-catalog checks.
- `pnpm build`: production single-file build passed; compatibility scan found no unsupported lookbehind expressions.
- Browser test mounted the actual `ChatView`, `SideChatWindow`, theme provider and modal, with deterministic HTTP/WebSocket fixtures. It verified `/side` with no arguments, a body portal and one header action, waiting/streaming input lock, reset on retry, main-draft isolation, follow-up history, stop/continue, close/reopen with draft and history, header dragging, all eight resize handles, placement above a body modal, Escape isolation, and resetting on session changes. No browser page errors occurred.
- Inspected screenshots at 1440×1000 and 390×780 in light and dark themes. The initial visual pass corrected the surface/background token hierarchy; the final pass passed all interactions.
- Browser fixtures did not contact a live model or modify user sessions. Native WebView occlusion uses the existing overlap coordinator; a packaged desktop/native-browser smoke test was not run.

## C++

The focused side-chat provider/protocol test binary was built with MSVC using existing local vcpkg headers/libraries: **36/36 tests passed** (16 side-chat tests and 20 existing authentication tests). Tests use fake providers and cover context isolation, actual deltas, retry/reset, partial cancellation, independent main retry delays, history validation, tool rejection, unsupported native-agent providers, per-connection request ownership, disconnect and late callback suppression. Reproduction: `& C:/Users/shao/AppData/Local/Temp/acecode-side-chat-20260915/build-side-chat.ps1`, or run `side_chat_tests.exe` there. Build/test logs and GTest XML are beside the executable.

This checkout has no configured CMake build and its submodules are not initialized. `agent_loop.cpp`, `session_registry.cpp`, `routes_ws.cpp`, `server.cpp`, `codex_provider.cpp`, and `copilot_provider.cpp` passed MSVC `/Zs` checks. A temporary harness of the two production Copilot token methods, with a mock token exchange, also passed concurrent-refresh, stale-401, snapshot-stability and account-change checks. A complete daemon/desktop build, real socket integration, live-provider request, and the full C++ suite are not claimed.

- `openspec validate add-floating-side-chat --strict`: passed.
- `git diff --check`: passed, including new source/test/spec files.

## Compatibility

- Existing synchronous HTTP and TUI side questions retain their old single-turn behavior.
- The native Codex app-server provider cannot guarantee tool-free calls and is rejected by the new streaming path before execution, with a translated message asking the user to choose another model.
- Side transcripts remain temporary for the selected view; refresh or switching sessions clears them. Closing/reopening within the same session preserves them.

## Artifacts

Local browser results, screenshots, fixture script and Web logs are under `C:/Users/shao/AppData/Local/Temp/acecode-side-chat-qa/`. The fixture HTML/JSX was removed from the project after verification. The C++ test binary is under `C:/Users/shao/AppData/Local/Temp/acecode-side-chat-20260915/`.

## Session toolbar, clear action and compact composer (2026-09-15)

- Added the session toolbar's SVG bubble before the trajectory action. The floating header now has trash/clear and close buttons, superseding the initial close-only layout above.
- Controller tests passed **20/20**, including clearing completed history/draft, clearing during loading/streaming/stop acknowledgement, synchronous cancellation callbacks, and ignoring every old event while a fresh request runs.
- `pnpm i18n:catalog`, `pnpm test`, `pnpm build`, `pnpm i18n:audit`, `openspec validate add-floating-side-chat --strict`, and `git diff --check` passed. The Impeccable detector reported no findings on the changed UI targets.
- The browser fixture mounted the production `ChatView` and `SideChatWindow`. It verified the bubble SVG loads and opens the shared window; clear precedes close; close/reopen retains draft/history; clear cancels loading, streaming and stopping requests, keeps the window open, focuses the editable textarea and starts a fresh request with empty side history. The main draft remained unchanged, clearing preserved geometry, and header dragging still worked.
- Measured the bottom region at **60px** in empty, multiline, light/dark and 1440px/390px viewport cases: a 44px border-box input plus 8px top and bottom spacing. Shift+Enter retains multiline drafts and scrolls inside the textarea without increasing the region's height. Screenshots for all four theme/viewport combinations were visually reviewed. No browser page errors occurred.
- Artifacts: `C:/Users/shao/AppData/Local/Temp/acecode-side-chat-refinement-20260915/` contains the browser script, fixture sources, results JSON, screenshots and Web logs. Temporary repository fixture files were removed after verification.
- This refinement changes only frontend behavior; daemon/provider/C++ code is unchanged. No installed desktop rebuild or live-model verification was performed for this refinement.

## Minimize label refinement (2026-09-16)

- Replaced the close icon and label with a horizontal minimize action while retaining the clear button and existing hide/reopen lifecycle.
- The v0.9.19 reasoning-depth browser check verified minimize/reopen preserves the side draft. Full frontend tests, i18n catalog generation, production build and strict OpenSpec validation passed.

## 侧边聊天存续时间（2026-10-15）

- 设置 → 常规新增「侧边聊天存续时间」，单位分钟，默认 30，仅接受正整数；使用现有偏好 hook 持久化，并接入设置搜索及英文文案。
- 新增 15 个定向场景，覆盖从请求结束而非开始计时、精确到期边界、续聊更新计时、生成中不失效、切换及草稿不续期、输入校验失败不续期、成功/失败/停止、最新配置、会话身份隔离、重挂载、清空和迟到回调。现有控制器用例保持通过。
- `pnpm test`、`pnpm build`（含旧 WebView 正则兼容检查）、`pnpm i18n:catalog`、`pnpm i18n:audit`、`openspec validate add-floating-side-chat --strict` 与本次改动范围的 `git diff --check` 通过；Impeccable 检测返回无问题。
- 可复现浏览器脚本：`web/scripts/test-side-chat-retention.mjs`，依赖外部 Playwright，可用 `ACE_PLAYWRIGHT_MODULE` 指定已有模块路径。脚本挂载真实 `SettingsPage`、`ChatView`、全局会话菜单与侧聊浮窗，使用 HTTP/WebSocket 夹具和可控时钟。验证分钟配置持久化/输入回退、跨会话内容与追问隔离、草稿及窗口状态恢复、StrictMode/重新挂载、40 分钟长回答、到期仅切回清空、中断部分回答、最新配置及英文标签。浏览器无页面错误。
- 浅色 1440px、深色 720px 截图已检查，配置行无横向溢出。截图位于 `C:/Users/shao/AppData/Local/Temp/ace-side-chat-retention-7Ypoos/`。
- 仅修改 Web 前端及设计/验证文档；没有桌面二进制重建、已安装桌面运行证明或真实模型请求验证。
