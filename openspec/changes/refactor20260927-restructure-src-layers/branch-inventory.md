# P0-02 分支与 worktree 盘点

本记录来自 2026-09-27 的 Windows 本机仓库 `C:/Users/shaoh/acecode`。盘点前已 fetch origin，工具为 P0-03 的 `24fc5199`（在 `657e0f9e` 上补充 remote-only 分支与子模块脏状态支持）。全部操作只读，未删除、移动、重置或改写任何遗留分支/worktree。

- 固定基线：`e15df9eadfaf05a170fd8b83fb26939452e2d3e9`。
- 实测 85 个分支/远端跟踪 ref、5 个 worktree；所有查询成功，无 unknown 状态。
- 9 个遗留远端 ref 含独有 src 改动，每个最多 16 个 src 路径，与母设计的风险范围一致。本地 `jb` 与 `origin/jb` 指向同一提交，按一个待迁移对象处理。
- 原审计的 51 个 worktree / 72 个本地分支 / 28 个可清理旧 worktree 没有出现在这台机器的 Git 管理视图中。当前 5 个 worktree 均用于主仓或本次重构，没有可交付用户删除的闲置旧 worktree；不能据此判断其它机器上的 worktree 状态。原数字是历史审计背景，不作为本机枚举结果。
- 4 个 worktree 有改动：主仓保留既有 `scripts/__pycache__/`，另外三个是正在实现的 P0-03、P0-05、P2-01。逐项原始状态保存在同目录 JSON 中。采样时 P0-02 worktree 是干净的，报告随后写入该 worktree。

## 九个待迁移 ref

| Ref | 固定 HEAD | 独有提交 | src 路径 | tests 路径 | HEAD 摘要 |
|---|---|---:|---:|---:|---|
| `origin/chatview_optimize` | `3e5bbb440ba7be3420b0844d6c62a9eeea0a5bca` | 1 | 10 | 8 | Add tests for ChatViewport and related components; enhance Markdown rendering |
| `origin/claude/ai-image-sharing-tool-8ewihy` | `3dd26983bc04bbcf451a1c5e000fb6abcbf52991` | 1 | 6 | 0 | Add show_image tool for AI image sharing (desktop → TUI) |
| `origin/claude/debug-acecode-crash-XNWgr` | `405a71ec63c4d929f51f212e40f748046c909d00` | 6 | 7 | 3 | Merge origin/master and resolve desktop conflicts |
| `origin/claude/desktop-skill-error-handling-r3j2h8` | `241304a8f0fbbea1899360c77c4e74d1e811b001` | 1 | 11 | 1 | fix(skills): a malformed SKILL.md must not 500 the whole skills page |
| `origin/claude/fix-desktop-context-compression-tkOxK` | `b893da6d8da696df2dbb92bc4f0cdeb967791411` | 1 | 2 | 0 | fix: auto-compact context for daemon/desktop sessions |
| `origin/claude/multi-model-config-design-wWhDX` | `3f6a2fe274cff6becfc3e132ede23736c9bc369d` | 1 | 4 | 0 | feat(tui): /model 改成 FTXUI 选择器 + 内联键位 |
| `origin/codex/add-self-session-control` | `1ce71fe01bfb54a546d23cbc0ed108423659dcf4` | 1 | 12 | 2 | WIP: preserve self session control implementation |
| `origin/docs/askuserquestion-dual-entry-design` | `de8f8c986ff6ff338a04d7aedafc0028ad84bd56` | 7 | 16 | 6 | docs: 标记 ask-user-question-dual-entry OpenSpec 任务全部完成 |
| `origin/jb` | `fd895862840e6557e205fa5e37bd7aa72ead10c9` | 3 | 12 | 1 | feat: JB 模式开启时用解开的提示词整段替换系统提示 |

P2-09 使用上述固定 HEAD 演练 patch 迁移；P3-03 保留原 ref 并在独立迁移分支验证，`--check` 通过后才考虑合入。此清单不表示这些功能分支已经获准合入，也不把它们的功能顺带加入本次重构。

## 两个单提交例外

- `origin/codex/add-self-session-control`：保留该独有提交，选择 P2-09/P3-03 的 patch 迁移路线，避免在结构重构准备期引入另一项功能变更。
- 历史审计的 `review-ai-image-sharing-tool` ref 在本机及 origin 当前跟踪列表均不存在；对应功能源分支 `origin/claude/ai-image-sharing-tool-8ewihy` 仍存在且只有一个独有提交，已列入上表，采用相同的 patch 迁移路线。没有把不存在的别名标记为已迁移或弃用。

## 复现与验收

在包含 P0-03 工具的 checkout 中运行：

```text
python scripts/refactor/branch_inventory.py --repo C:/Users/shaoh/acecode --base e15df9eadfaf05a170fd8b83fb26939452e2d3e9 --jobs 3 --timeout 120 --format json --output branch-inventory.json
```

JSON 保留每个 ref 的完整 `git cherry` 输出、独有 src/tests 路径和每个 worktree 的原始脏状态；下面是同一份 JSON 生成的表格。分支或 worktree 后续变化时重新盘点，不能复用此时间点的清理判断。

## 完整机器盘点

Base: `master` (`e15df9eadfaf05a170fd8b83fb26939452e2d3e9`). Read-only snapshot.

`src` / `tests` count distinct paths changed by commits marked `+` by `git cherry`.

| Ref | Ahead | Behind | Cherry + | Cherry - | src | tests | Error |
|---|---:|---:|---:|---:|---:|---:|---|
| refs/heads/jb | 3 | 65 | 3 | 0 | 12 | 1 |  |
| refs/heads/master | 0 | 0 | 0 | 0 | 0 | 0 |  |
| refs/heads/refactor20260927/P0-01 | 0 | 6 | 0 | 0 | 0 | 0 |  |
| refs/heads/refactor20260927/P0-02 | 0 | 0 | 0 | 0 | 0 | 0 |  |
| refs/heads/refactor20260927/P0-03 | 2 | 1 | 2 | 0 | 0 | 0 |  |
| refs/heads/refactor20260927/P0-05 | 0 | 1 | 0 | 0 | 0 | 0 |  |
| refs/heads/refactor20260927/P0-06 | 1 | 1 | 1 | 0 | 0 | 1 |  |
| refs/heads/refactor20260927/P2-01 | 0 | 1 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/add-dev-web-prototypes | 0 | 217 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/archon/thread-60a47498 | 4 | 1118 | 0 | 4 | 0 | 0 |  |
| refs/remotes/origin/askuserquestion-v2 | 0 | 196 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/chatview_optimize | 1 | 1012 | 1 | 0 | 10 | 8 |  |
| refs/remotes/origin/claude/acecode-cache-hit-optimization-lx9hxc | 0 | 661 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/claude/acecode-session-rename-bug-n1ndkn | 0 | 452 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/claude/ai-image-sharing-tool-8ewihy | 1 | 929 | 1 | 0 | 6 | 0 |  |
| refs/remotes/origin/claude/browser-improvement-ideas-ibzwcr | 1 | 177 | 1 | 0 | 0 | 0 |  |
| refs/remotes/origin/claude/debug-acecode-crash-XNWgr | 7 | 1074 | 6 | 0 | 7 | 3 |  |
| refs/remotes/origin/claude/desktop-skill-error-handling-r3j2h8 | 1 | 330 | 1 | 0 | 11 | 1 |  |
| refs/remotes/origin/claude/desktop-webview-design-GttVa | 0 | 1136 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/claude/feedback-daemon-logs-8nzu5p | 0 | 641 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/claude/fix-desktop-context-compression-tkOxK | 1 | 1064 | 1 | 0 | 2 | 0 |  |
| refs/remotes/origin/claude/github-version-tag-release-l2h68z | 0 | 80 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/claude/multi-model-config-design-wWhDX | 1 | 1119 | 1 | 0 | 4 | 0 |  |
| refs/remotes/origin/claude/remove-copilot-claude-attribution-r8fhym | 0 | 411 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/claude/ui-tars-computer-use-integration-nwl4fo | 1 | 422 | 1 | 0 | 0 | 0 |  |
| refs/remotes/origin/claude/wintoast-compatibility-issue-5wzara | 0 | 688 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/codex/add-self-session-control | 1 | 551 | 1 | 0 | 12 | 2 |  |
| refs/remotes/origin/codex/desktop-package-size-test | 3 | 893 | 3 | 0 | 0 | 1 |  |
| refs/remotes/origin/codex/fix-chat-workspace-switch-delay | 0 | 316 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/codex/mcp-core | 0 | 177 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/codex/package-size-regression-fix | 0 | 867 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/codex/release-v0.9.11 | 0 | 323 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/codex/release-v0.9.11-version | 0 | 321 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/codex/release-v0.9.27 | 0 | 11 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/codex/speed-up-release-packaging | 0 | 700 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/codex/whatsapp-channels | 0 | 311 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/copilot/add-github-actions-for-multi-platform | 0 | 1192 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/copilot/assess-frontend-maintainability | 0 | 1031 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/copilot/fix-submodule-pull-issues | 0 | 1189 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/copilot/gitaction-pro-vs-non-pro | 0 | 424 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/copilot/refactor-code-for-better-readability | 0 | 1119 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/copilot/stabilize-github-actions-workflow | 0 | 1183 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/docs/askuserquestion-dual-entry-design | 7 | 317 | 7 | 0 | 16 | 6 |  |
| refs/remotes/origin/feat/acemodel-turn-usage-api | 0 | 130 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/feat/ask-configurable-option-limit | 0 | 214 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/feat/brainstorming-grilling-handoff | 0 | 351 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/feat/development-environment-launcher | 0 | 155 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/feat/fork-user-prompt-to-composer | 0 | 294 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/feat/herdr-custom-agent | 0 | 606 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/feat/macos-custom-installer-updates | 0 | 171 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/feat/middle-click-close-preview-tab | 3 | 351 | 2 | 0 | 0 | 0 |  |
| refs/remotes/origin/feat/rss-search-service | 0 | 603 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/feat/session-context-menu-copy | 0 | 351 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/feat/streaming-incremental-layout | 0 | 403 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/feat/tui-hyperlinks | 3 | 333 | 1 | 1 | 0 | 1 |  |
| refs/remotes/origin/feat/verify-package-skill | 0 | 340 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/feat/web-ask-user-question-reland | 0 | 227 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/fix-askuserquestion-i18n | 0 | 208 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/fix-pnpm-patch-format | 0 | 217 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/fix/centralize-tui-logs | 0 | 177 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/fix/dev-launcher-runtime-identity | 0 | 129 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/fix/ftxui-vcpkg-bump-openspec-wrapup | 0 | 328 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/fix/question-picker-answer-reset | 0 | 17 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/fix/settings-modal-stuck-and-dev-script | 0 | 338 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/fix/sidebar-alignment-rhythm | 0 | 84 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/fix/sidebar-icon-alignment | 0 | 176 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/fix/sidebar-session-load-race | 0 | 156 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/fix/skill-loader-utf8-truncation | 0 | 347 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/fix/verify-package-desktop-probe | 0 | 331 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/fix/web-ask-feedback-persistence | 0 | 249 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/improve/build-dry-run | 0 | 255 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/jb | 3 | 65 | 3 | 0 | 12 | 1 |  |
| refs/remotes/origin/master | 0 | 1 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/refactor20260927/P0-06 | 1 | 1 | 1 | 0 | 0 | 1 |  |
| refs/remotes/origin/restore-askuserquestion-ui | 0 | 212 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/session_profile | 0 | 635 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/shz_vide/confirmation-window-ui-1d5588 | 1 | 831 | 1 | 0 | 0 | 0 |  |
| refs/remotes/origin/shz_vide/interface-design-fixes-28c216 | 0 | 828 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/shz_vide/permission-denial-queue-bug-13d424 | 1 | 831 | 0 | 1 | 0 | 0 |  |
| refs/remotes/origin/sidebar-optimization | 0 | 128 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/support/agents-md | 0 | 420 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/task/redesign-model-settings-with-presets | 0 | 554 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/webui-worktree-badge-and-ui-polish | 0 | 827 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/worktree-ask-question-policy | 0 | 862 | 0 | 0 | 0 | 0 |  |
| refs/remotes/origin/worktree-dialog-status | 0 | 216 | 0 | 0 | 0 | 0 |  |

| Worktree | Ref / HEAD | Dirty | Status error |
|---|---|---|---|
| C:/Users/shaoh/acecode | refs/heads/master | yes |  |
| C:/Users/shaoh/.codex/worktrees/refactor-p0-01/acecode | refs/heads/refactor20260927/P0-02 | no |  |
| C:/Users/shaoh/.codex/worktrees/refactor-raii/acecode | refs/heads/refactor20260927/P2-01 | yes |  |
| C:/Users/shaoh/.codex/worktrees/refactor-testpaths/acecode | refs/heads/refactor20260927/P0-05 | yes |  |
| C:/Users/shaoh/.codex/worktrees/refactor-tools/acecode | refs/heads/refactor20260927/P0-03 | yes |  |

## git cherry results and unique paths

### refs/heads/jb

```text
+ 6f49c00ef274b7cbbd1f0da50cb9950e0ee3512b
+ 90f1ebd5fec4c9937de5f6b90d5a67e4f126074f
+ fd895862840e6557e205fa5e37bd7aa72ead10c9
```

- `src/agent_loop.cpp`
- `src/agent_loop.hpp`
- `src/config/config.cpp`
- `src/config/config.hpp`
- `src/prompt/jb_slot.cpp`
- `src/prompt/jb_slot.hpp`
- `src/prompt/jb_slot_table.inc`
- `src/prompt/system_prompt.cpp`
- `src/prompt/system_prompt.hpp`
- `src/session/session_registry.cpp`
- `src/session/session_registry.hpp`
- `src/web/routes/routes_misc.cpp`
- `tests/prompt/system_prompt_jb_mode_test.cpp`

### refs/heads/refactor20260927/P0-03

```text
+ 657e0f9e8b379bf8cfbe77671cece86edfb28a51
+ 24fc5199e9653894544e8bc051757517bdab6e39
```


### refs/heads/refactor20260927/P0-06

```text
+ b4db043d1e40f5a4ad3ee634ee5362e8887c7abe
```

- `tests/cpp_source_paths.json`

### refs/remotes/origin/archon/thread-60a47498

```text
- 7d7bd8364199e3ae365b87d57f5e23784cb95496
- ef4333983e24e70772ffb9dfc7bc3c5ee8768db3
- 27e6cacd5b9f4b2132696c9f66f1e6ae6a4f9f11
- b2392fc98c5dfc37402011fad58d7fc64833a48c
```


### refs/remotes/origin/chatview_optimize

```text
+ 3e5bbb440ba7be3420b0844d6c62a9eeea0a5bca
```

- `src/agent_loop.cpp`
- `src/commands/builtin_commands.cpp`
- `src/tool/ask_overlay_input.hpp`
- `src/tui/chat_viewport.cpp`
- `src/tui/chat_viewport.hpp`
- `src/tui/chat_viewport_cache.cpp`
- `src/tui/chat_viewport_cache.hpp`
- `src/tui/chat_viewport_model.hpp`
- `src/tui/diff_view.hpp`
- `src/tui_state.hpp`
- `tests/CMakeLists.txt`
- `tests/scripts/__init__.py`
- `tests/scripts/__pycache__/__init__.cpython-314.pyc`
- `tests/scripts/__pycache__/tui_chat_viewport_scripts_test.cpython-314.pyc`
- `tests/scripts/tui_chat_viewport_scripts_test.py`
- `tests/tui/chat_viewport_cache_test.cpp`
- `tests/tui/chat_viewport_model_test.cpp`
- `tests/tui/chat_viewport_test.cpp`

### refs/remotes/origin/claude/ai-image-sharing-tool-8ewihy

```text
+ 3dd26983bc04bbcf451a1c5e000fb6abcbf52991
```

- `src/tool/builtin_tool_registry.hpp`
- `src/tool/show_image_tool.cpp`
- `src/tool/show_image_tool.hpp`
- `src/tool/tool_icons.hpp`
- `src/tui_state.hpp`
- `src/utils/open_file.hpp`

### refs/remotes/origin/claude/browser-improvement-ideas-ibzwcr

```text
+ dbb2216752eced82138418e2fe07374d1189cc51
```


### refs/remotes/origin/claude/debug-acecode-crash-XNWgr

```text
+ 2f971d12c7cac24e91c941e068f050c246bf8b49
+ 732c4b9c4f1936bf538d58f9b832cf7ea42b5bc9
+ 22bb7a0300f5e4da3f9201053c2cfe02f0657f0c
+ b826950186ae3f1fe5650a94ec897b4c0104ba5d
+ 3252e02c0bb10b8b516e69cf7154d0285df1d321
+ 73a8d258d206dd67b30a3a70c4efe9de41166851
```

- `src/desktop/chromium_app_launcher.cpp`
- `src/desktop/chromium_app_launcher.hpp`
- `src/desktop/main.cpp`
- `src/desktop/web_host.cpp`
- `src/desktop/webview2_runtime_probe.cpp`
- `src/desktop/webview2_runtime_probe.hpp`
- `src/web/server.cpp`
- `tests/desktop/chromium_app_launcher_test.cpp`
- `tests/desktop/webview2_runtime_probe_test.cpp`
- `tests/web/web_server_smoke_test.cpp`

### refs/remotes/origin/claude/desktop-skill-error-handling-r3j2h8

```text
+ 241304a8f0fbbea1899360c77c4e74d1e811b001
```

- `src/skills/frontmatter.cpp`
- `src/skills/frontmatter.hpp`
- `src/skills/skill_loader.cpp`
- `src/skills/skill_loader.hpp`
- `src/skills/skill_metadata.hpp`
- `src/skills/skill_registry.cpp`
- `src/skills/skill_registry.hpp`
- `src/web/handlers/skills_handler.cpp`
- `src/web/json_dump.hpp`
- `src/web/routes/routes_experts.cpp`
- `src/web/routes/routes_files.cpp`
- `tests/skills/skill_load_resilience_test.cpp`

### refs/remotes/origin/claude/fix-desktop-context-compression-tkOxK

```text
+ b893da6d8da696df2dbb92bc4f0cdeb967791411
```

- `src/agent_loop.cpp`
- `src/agent_loop.hpp`

### refs/remotes/origin/claude/multi-model-config-design-wWhDX

```text
+ 3f6a2fe274cff6becfc3e132ede23736c9bc369d
```

- `src/commands/builtin_commands.cpp`
- `src/tui/picker_scroll.hpp`
- `src/tui/slash_dropdown.cpp`
- `src/tui_state.hpp`

### refs/remotes/origin/claude/ui-tars-computer-use-integration-nwl4fo

```text
+ 86657a5aabf87223135f0f5a654dfb9701dca6e1
```


### refs/remotes/origin/codex/add-self-session-control

```text
+ 1ce71fe01bfb54a546d23cbc0ed108423659dcf4
```

- `src/agent_loop.cpp`
- `src/agent_loop.hpp`
- `src/session/session_control_service.cpp`
- `src/session/session_control_service.hpp`
- `src/session/session_pin_store.cpp`
- `src/session/session_pin_store.hpp`
- `src/tool/session_control_tool.cpp`
- `src/tool/session_control_tool.hpp`
- `src/tool/tool_executor.cpp`
- `src/tool/tool_executor.hpp`
- `src/web/handlers/pinned_sessions_handler.cpp`
- `src/web/handlers/pinned_sessions_handler.hpp`
- `tests/agent_loop/agent_loop_deferred_tools_test.cpp`
- `tests/tool/tool_capability_policy_test.cpp`

### refs/remotes/origin/codex/desktop-package-size-test

```text
+ d24c6b7cc679dcee0e16fcd23d0f92f8e9fa07a1
+ d6567ba81dfc48e3498ebf1e348aaa5b27695726
+ c40e36361f555bbb3543f6ad082b72e3db30712c
```

- `tests/CMakeLists.txt`

### refs/remotes/origin/docs/askuserquestion-dual-entry-design

```text
+ 5ca5cceee956f8046f19223bd8c7b3477f0c71d7
+ 93654dabd7892abefdd211b3022f8880f318beab
+ 4eb3bbe2f8f9a038092d7d785877f015b147f62a
+ 85550d69703fe97e68fb54ac2530c12d1296f452
+ 1a3e45a3d80318d7e8a52ace74c56bf75d88452d
+ 5b1a3597a0f8529419ab822b8244aa2b3f2f76ea
+ de8f8c986ff6ff338a04d7aedafc0028ad84bd56
```

- `src/agent_loop.cpp`
- `src/main.cpp`
- `src/remote_control/channel_question_bridge.cpp`
- `src/session/ask_user_question_prompter.hpp`
- `src/tool/ask_overlay_input.hpp`
- `src/tool/ask_user_question_tool.cpp`
- `src/tool/ask_user_question_tool.hpp`
- `src/tool/tool_executor.hpp`
- `src/tui/ask_question_overlay.cpp`
- `src/tui/ask_question_overlay.hpp`
- `src/tui/tui_ask_channel.cpp`
- `src/tui/tui_ask_channel.hpp`
- `src/tui/tui_helpers.cpp`
- `src/tui/tui_init.cpp`
- `src/tui_state.hpp`
- `src/web/routes/routes_ws.cpp`
- `tests/remote_control/channel_question_bridge_test.cpp`
- `tests/remote_control/session_channel_binder_test.cpp`
- `tests/tool/ask_overlay_input_test.cpp`
- `tests/tool/ask_user_question_tool_test.cpp`
- `tests/tui/ask_question_overlay_test.cpp`
- `tests/tui/tui_input_wrapping_test.cpp`

### refs/remotes/origin/feat/middle-click-close-preview-tab

```text
+ 52c259324dbe7acbfe0c4962659b0cbfafb8b715
+ 97e6e351111c0a3ccea51f9fcda3d6cde0e08455
```


### refs/remotes/origin/feat/tui-hyperlinks

```text
- ae67486c9fce68ec834120b34b6c36fcf60db0de
+ 3b01afffc4593ea24851a7d72af9e017fc6c4a0a
```

- `tests/scripts/verify_package_test.sh`

### refs/remotes/origin/jb

```text
+ 6f49c00ef274b7cbbd1f0da50cb9950e0ee3512b
+ 90f1ebd5fec4c9937de5f6b90d5a67e4f126074f
+ fd895862840e6557e205fa5e37bd7aa72ead10c9
```

- `src/agent_loop.cpp`
- `src/agent_loop.hpp`
- `src/config/config.cpp`
- `src/config/config.hpp`
- `src/prompt/jb_slot.cpp`
- `src/prompt/jb_slot.hpp`
- `src/prompt/jb_slot_table.inc`
- `src/prompt/system_prompt.cpp`
- `src/prompt/system_prompt.hpp`
- `src/session/session_registry.cpp`
- `src/session/session_registry.hpp`
- `src/web/routes/routes_misc.cpp`
- `tests/prompt/system_prompt_jb_mode_test.cpp`

### refs/remotes/origin/refactor20260927/P0-06

```text
+ b4db043d1e40f5a4ad3ee634ee5362e8887c7abe
```

- `tests/cpp_source_paths.json`

### refs/remotes/origin/shz_vide/confirmation-window-ui-1d5588

```text
+ eed155ed6e4b352a9a9dfbcd7ac69cd8a3f810f8
```


### refs/remotes/origin/shz_vide/permission-denial-queue-bug-13d424

```text
- 5028f277e315af16bc9ccd612038834693acdab3
```


