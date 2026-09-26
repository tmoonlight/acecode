# 旧路径 → 新路径映射表(refactor20260927)

本表是 P0-03 生成 `src/layers.tsv` 与 `scripts/refactor/src_layout_map.tsv` 的**初版依据**,分组与 rank 的定义见 [design.md](design.md) D2。

阅读规则:
- 以目录级映射为主,冻结时(P3)整目录 R100 改名。
- 标了 P2-xx 的行,在冻结**前**由对应 PR 先移到 `src/<模块>/` 形式的新位置,冻结时再随模块挂到分组下。
- 新文件或拆出物由 P6 / B / O 任务产生。
- 路径中的 `{a,b}` 表示同名 `.hpp/.cpp` 或列举;`*` 表示同名的全部文件。
- 行号基于 `7942011b`。
- 执行中若发现本表与实际依赖不符,以 lint 实测为准修正 `layers.tsv`,并回写本表。

## 1. 根目录散文件

| 旧路径 | 新路径 | 何时搬 | 说明 |
|---|---|---|---|
| `src/agent_loop.{hpp,cpp}` | `src/engine/agent/agent_loop.{hpp,cpp}` | P2-08 先 R100 移到 `src/agent/`;P3 挂到 engine/ | 之后由 split-agent-loop 拆分 |
| `src/agent_loop_doom_guard.{hpp,cpp}` | `src/engine/agent/guards/doom_guard.{hpp,cpp}` | P2-08 | |
| `src/agent_loop_shell_guard.hpp` | `src/domain/permissions/shell_write_guard.hpp` | P2-05 | 与 `sandbox/command_classifier.cpp:919` 分词器的合并放二期 |
| `src/permissions.hpp` | `src/domain/permissions/permissions.hpp` + `src/base/config/vocab/permission_mode.hpp` | P2-05 | 28-54 行的枚举与 304-350 行的模式名规范化拆到 vocab;`config.cpp:3`、`settings_mutations.cpp:2`、`exec_decision.hpp:12` 改用 vocab;tests 中 38 处裸 include 改为 `permissions/permissions.hpp` |
| `src/tui_state.hpp` | `src/apps/tui/tui_state.hpp` | P2-08 | tests 中 7 处裸 include 同步改 |
| `src/main.cpp` | `src/apps/cli/main.cpp` | P2-08 先 R100 移到 `src/cli/main.cpp`;P3 挂到 apps/ | 之后由 split-tui-main 拆分 |
| `src/version.hpp.in` | `cmake/version.hpp.in` | P2-08 | 生成名 `generated/version.hpp` 不变;`upgrade/version` 同时改名为 `utils/semver`,消除同名遮蔽 |

## 2. base/(L0)

| 旧路径 | 新路径 | 何时搬 | 说明 |
|---|---|---|---|
| `src/utils/` | `src/base/utils/` | P3 | 例外见下面几行 |
| `src/utils/{file_operations,tool_errors,tool_args_parser}.hpp` | `src/adapters/tool/` | P2-05 | 切断 utils→tool(`file_operations.hpp:4-5`、`tool_errors.hpp:3`) |
| `src/utils/text_file_buffer.{hpp,cpp}` | `base/utils/text_file_buffer.*`(只留纯编解码)+ `adapters/tool/safe_text_write.*` | P2-05 | `safe_write_text_file`、MCP 配置拦截(cpp:847-852)、带工具名的文案(631/684/707/779)上移;`web/routes/routes_files.cpp:325` 只改调用点(D21) |
| `src/utils/token_tracker.{hpp,cpp}` | `src/domain/session/` | P2-05 | |
| `src/utils/{models_dev_catalog.*,model_capabilities.cpp}` | `src/base/config/` + `src/adapters/provider/models_dev_catalog_cache.*` | P2-05 | cpp 212-277、343 行依赖 registry 的部分上移,断开 config→provider→utils→provider→config 的环 |
| `src/utils/path_validator.hpp` | `src/domain/permissions/path_validator.hpp` | P2-05 | |
| `src/utils/{clipboard,open_url,power_inhibitor}.*` | `src/base/platform/` | P2-03 | |
| `src/utils/{terminal_capability.*,terminal_theme_detect*,terminal_title.*,terminal_input.hpp}` | `src/base/platform/terminal/` | P2-03 | `terminal_title` 的 `sanitize_title` 在 P2-07 拆到 `domain/session/session_title_text` |
| `src/utils/{drag_scroll,text_input_ops}.*` | `src/apps/tui/` | P2-08 | 登记进 `ACECODE_TUI_TESTABLE_SUBSETS` |
| `src/utils/state_file.*` | `src/base/utils/`(只留通用部分) | P2-05 | 5 组专用函数搬回各自使用方,切断 `state_file.hpp:3 → config/saved_models` |
| (新增)`joining_thread`、`lifetime_token`、`scope_exit`、`abandonable_call`、`abort_signal` | `src/base/utils/` | P2-01(属 adopt-ownership-conventions) | 必须在 P3 之前合入 |
| `src/tool/{diff_utils,word_diff,diff_view_truncate}.*` | `src/base/utils/` | P2-02 | |
| `src/skills/frontmatter.*` | `src/base/utils/` | P2-06 | memory→skills 边随之消失 |
| `src/upgrade/version.*` | `src/base/utils/semver.*` | P2-05 | |
| `src/image/` | `src/base/image/` | P3 | |
| `src/image/stb/*.h` | `external/stb/` | P3(M1 搬,M2 改 include) | SYSTEM include 只挂在 base 上 |
| `src/hooks/hook_runner.*` | `src/base/platform/process/process_runner.*` | P2-03 | `HookCommandSpec` 改为 `platform::ProcessSpec`,hooks 保留 using 别名 |
| `src/lsp/lsp_process.*` | `src/base/platform/process/piped_process.*` | P2-03 | lsp 保留别名 |
| `src/lsp/lsp_which.*` | `src/base/platform/process/which.*` | P2-03 | |
| `src/daemon/{platform.hpp,platform_posix.cpp,platform_windows.cpp}` | `src/base/platform/process/os_process{.hpp,_posix.cpp,_windows.cpp}` | P2-03 | NATIVE_BRIDGE 清单同步 |
| `src/desktop/locale.*` | `src/base/platform/` | P2-03 | |
| `src/desktop/{folder_picker.hpp,folder_picker_win.cpp,folder_picker_mac.mm,context_picker.*,open_in_explorer.*,notifications.*,notifications_backend.hpp,notifications_win.cpp,notifications_mac.mm,notifications_stub.cpp,custom_toast.*,custom_toast_win.cpp,strings.*}` | `src/base/platform/native_ui/` | P2-03 | `.mm` 路径与 NOTIFICATION_BACKEND 清单同步 |
| `src/upgrade/console.hpp` | `src/base/platform/terminal/` | P2-03 | 与 `terminal_input` 功能重叠,合并放二期 |
| (新增)`unique_handle`、`file_lock` | `src/base/platform/process/` | O-09 | |
| (新增)`utf8_command_line` | `src/base/platform/` | B-02 | 自 main.cpp:2925-2958 |
| `src/config/` | `src/base/config/` | P3 | 新增 `vocab/` |
| `src/computer_use/pointer_appearance.hpp` | `src/base/config/vocab/` | P2-05 | `config.hpp:5` 改指 vocab |
| `src/themes/theme_id.hpp` | `src/base/config/vocab/` | P2-05 | |
| `src/provider/builtin_model_catalog.*` | `src/base/config/` | P2-05 | 已在 NATIVE_BRIDGE 清单里 |
| `config.cpp:485-501` 的 `get_acecode_dir/get_run_dir/get_logs_dir` | `src/base/utils/paths.*` | P2-04 | 19 个文件只为这三个函数 include 了 config.hpp |
| `src/network/` | `src/base/network/` | P3 | |
| `src/upgrade/http.*` | `src/base/network/http.*` | P2-05 | themes→upgrade 边随之消失 |
| `src/daemon/{runtime_files.*,guid.hpp}` | `src/base/ipc/` | P2-04 | |
| `src/desktop/{open_request.*,daemon_protocol.hpp,agent_browser_runtime.*}` | `src/base/ipc/` | P2-04 | `agent_browser_runtime.cpp` 依赖 config(rank 3 < ipc 5,合法) |
| `src/desktop/workspace_registry.*` | `src/base/workspace/` | P2-04 | namespace `desktop` 暂不改 |
| `src/web/handlers/files_handler.*` | `src/base/workspace/` | P2-04 | D21;只依赖 `utils/encoding` |
| `src/web/pty/` | `src/base/pty/` | P2-03 | 同步改 `cmake/acecode_winpty.cmake:64`;`routes_pty.cpp` 留在 web |
| `src/environment/` | `src/base/environment/` | P3 | |
| `src/environment/prompt_environment.*` | `src/engine/prompt/` | P2-08 | 切断 environment→prompt |

## 3. domain/(L1)

| 旧路径 | 新路径 | 何时搬 | 说明 |
|---|---|---|---|
| `src/provider/llm_provider.hpp` | `src/domain/llm/llm_provider.hpp` | P2-02 | 删掉第 3 行 `#include "retry_policy.hpp"`;需要重试函数的实现文件自行 include `provider/retry_policy.hpp`,遗漏的地方编译会报出来;83 处 include 改前缀,旧路径留转发头 |
| `src/tool/{tool_protocol_names.*,model_family.*,tool_icons.hpp}` | `src/domain/llm/` | P2-02 | |
| `tool/tool_executor.hpp` 中的 `ToolResult` / `ToolSummary` | `src/domain/llm/tool_result.hpp` | P2-02 | 改用方:`tool_metadata_codec.hpp:23`、`tool_result_storage.hpp:4`、`web/tool_event_payload.hpp:12` |
| `commands/compact.hpp` 的 token 估算(`approx_token_count`、`truncate_text_to_token_budget`、`estimate_message_tokens`) | `src/domain/llm/token_estimate.*` | P2-02 | `session/thread_repair.cpp`、`prompt/system_prompt.cpp`、`prompt/context_usage_breakdown.cpp` 改为只依赖 llm |
| `commands/compact.hpp` 的 `is_real_user_message`、`is_compact_summary_message` | `src/domain/llm/message_predicates.*` | P2-02 | |
| `commands/compact.hpp` 的上下文阈值与告警 | `src/domain/llm/context_thresholds.*` | P2-02 | |
| `prompt/context_usage_breakdown` 的 JSON codec | `src/domain/llm/context_usage.*` | P2-02 | 切断 `session_storage.cpp:7 → prompt` |
| `tool_preamble/tool_preamble` 的 `strip_text_preamble_tags` / `TextPreambleScanner` | `src/domain/llm/text_preamble_tags.*` | P2-02 | 如果有非纯依赖,就放 utils |
| `src/headless/headless_mode.*` | `src/domain/permissions/interaction_mode.*` | P2-05 | 零依赖,消费方都在更高层 |
| main.cpp:3272-3294 的 7 条 TUI 专属 Deny 规则 | `src/domain/permissions/default_rules.*` | B-03 | 函数名要带 tui,只作用于 TUI 主会话 |
| `src/security/` | `src/domain/security/` | P3 | |
| `src/sandbox/` | `src/domain/sandbox/` | P3 | |
| `src/experts/` | `src/domain/experts/` | P3 | |
| `src/project_instructions/` | `src/domain/project_instructions/` | P3 | |
| `src/memory/` | `src/domain/memory/` | P3 | |
| `src/history/` | `src/domain/history/` | P3 | 接收 main.cpp:7203-7218 的 `input_history_recorder`(B-04) |
| `src/connectors/` | `src/domain/connectors/` | P3 | |
| `src/hooks/` | `src/domain/hooks/` | P3 | `hook_runner` 已在 P2-03 移出;`hook_payload` 中含 provider 类型的三个构造器在 P2-06 移到 `engine/agent/hook_bridge/hook_events`;P2-06 新增 `hook_seeds`(拆自 default_skill_seeder);`hook_payload.cpp:5` 的死 include 在 P0-08 删除 |
| `src/skills/` | `src/domain/skills/` | P3 | frontmatter 在 P2-06 移到 utils;`skill_commands` 在 P2-06 移到 `apps/tui/commands` |
| `src/commands/opencode_command.*` | `src/domain/skills/` | P2-06 | `opencode_command_registry` 留在 commands |
| `src/web/handlers/skill_command_expander.*` | `src/domain/skills/` | P2-06 | D21 |
| main.cpp:1176-1208 `reconcile_default_skills_on_startup` | `src/domain/skills/default_skill_startup.*` | B-02 | 与 `daemon/cli.cpp:66-74` 的同名包装二期再合并 |
| `src/worktree/` | `src/domain/worktree/` | P3 | 只保留纯 git 操作;启动期 worktree 引导放在 `apps/tui/app/startup_worktree`(B-03) |
| `src/gitinfo/` | `src/domain/gitinfo/` | P3 | gitinfo(22) → worktree(21) 合法 |
| `src/session/` | `src/domain/session/` | P3 | 例外见下面几行;留下持久化、会话 API 契约、附件、各类 store、`tool_result_storage`、`thread_repair` |
| `src/session/{session_registry,local_session_client,thread_service,task_suggestion_service,session_auto_title,session_title_generator}.*` | `src/host/session_host/` | P2-07 | `session_title_generator` 的 sanitize / is_error 纯函数留在 `domain/session/session_title_text`(`session_manager.cpp:6`、`session_storage.cpp:4` 使用) |
| `src/session/side_chat.*` | `src/engine/agent/side_question/side_chat.*` | P2-08 | domain 不调模型 |
| `src/session/{session_replay,session_resume_restore}.*` | `src/apps/tui/resume/` | P2-08 | `restore_file_tool_state_from_messages` 拆到 `adapters/tool/file_state_restore`,`session_registry.cpp:1165` 改调它 |
| (新增)`composer_attachments` | `src/domain/session/` | B-04 | 自 main.cpp:1708-1757 |

## 4. adapters/(L2)

| 旧路径 | 新路径 | 何时搬 | 说明 |
|---|---|---|---|
| `src/upgrade/` | `src/adapters/upgrade/` | P3 | `manifest.cpp` 的 `ACECODE_DEEPIN` 源属性随路径更新 |
| `src/themes/` | `src/adapters/themes/` | P3 | |
| `src/feedback/` | `src/adapters/feedback/` | P3 | |
| `src/computer_use/` | `src/adapters/computer_use/` | P3 | helper 可执行文件源受 R6 约束;A-05 新增 `session_lease.hpp` |
| `src/lsp/` | `src/adapters/lsp/` | P3 | P2-07 接收 `lsp_status_text`(自 `commands/lsp_command` 的 format / dispatch) |
| `src/pa/`(含 README.md) | `src/adapters/pa/` | P3 | P2-02 把 `pa_quirks.hpp:9` 改指 `llm/llm_provider.hpp`;A-11 接收 `pa_rescue_driver` |
| `src/provider/` | `src/adapters/provider/` | P3 | provider→session 的 12 条边在全序下合法 |
| `src/provider/apply_model_to_session.*` | `src/host/session_host/` | P2-07 | 切断 provider→agent_loop 这条唯一的反向边 |
| `src/provider/auth/acecode.code-workspace` | 删除 | P0-08 | 误入仓库的个人配置,含本机路径 |
| `src/tool/` | `src/adapters/tool/` | P3 | `workspace_tools` 留在 tool |
| `src/tool/{spawn_subagent_tool,thread_tools,task_suggestion_tools}.*` | `src/host/session_host/tools/` | P2-07 | 依赖 SessionRegistry / ThreadService,只在组合根注册 |
| `src/tool/agent_browser/pointer_overlay.cpp` | `src/adapters/tool/agent_browser/browser_pointer_overlay.cpp` | P2-08 | 与 `computer_use/pointer_overlay.cpp` 重名,冻结前改名 |
| `src/daemon/mcp_runtime.*` | `src/adapters/tool/` | P2-08 | 消除 headless→daemon 反向依赖 |
| `cli/interactive_options` 的 `parse_question_policy_value` | `src/adapters/tool/question_policy.*` | P2-08 | 切断 `daemon/cli.cpp:8 → cli`;A-02 同步删掉 `session_registry.cpp:1137-1144` 的重复实现 |

## 5. engine/(L3)

| 旧路径 | 新路径 | 何时搬 | 说明 |
|---|---|---|---|
| `src/tool_preamble/` | `src/engine/tool_preamble/` | P3 | 标签剥离器已在 P2-02 下沉 |
| `src/prompt/` | `src/engine/prompt/` | P3 | P2-08 接收 `prompt_environment`,P2-07 接收 `init_prompt` |
| `commands/init_command` 的 `build_*` | `src/engine/prompt/init_prompt.*` | P2-07 | |
| (新模块)`agent/` | `src/engine/agent/` | P2-06(`hook_bridge/`)/ P2-08 | |
| `src/web/{message_payload,tool_event_payload}.*` | `src/engine/agent/event_payload/` | P2-08 | D21;切断 `agent_loop.cpp:42-43`、`task_suggestion_service.cpp:14` → web |
| `src/commands/{compact,compact_prompt}.*`(P2-02 拆剩的部分) | `src/engine/agent/compaction/` | P2-08 | 必须在 commands 整体移入 tui 之前完成 |
| `hooks/hook_payload` 中含 provider 类型的三个构造器 | `src/engine/agent/hook_bridge/hook_events.*` | P2-06 | 切断 hooks→provider |

## 6. host/(L4)

| 旧路径 | 新路径 | 何时搬 | 说明 |
|---|---|---|---|
| (新模块)`session_host/` | `src/host/session_host/` | P2-07 | 见第 3 节 |
| `src/loop/` | `src/host/loop/` | P3 | |
| `src/remote_control/` | `src/host/remote_control/` | P3 | P2-03 起 `channel_plugin` 改用 `platform/process`;允许 Crow(R7) |
| `src/channels/` | `src/host/channels/` | P3 | P2-03 起 bridge/setup 改用 `platform/process`;`CMakeLists.txt:416` 的 `ACECODE_CHANNEL_ASSET_DIR` 源属性随路径更新;允许 Crow(R7) |
| `src/channels/command.*` | `src/apps/cli/channels_cli.*` | P2-08 | 切断 channels→tui |
| `host/app_runtime/` | — | 二期预留 | 本期不创建 |

## 7. apps/(L5)

| 旧路径 | 新路径 | 何时搬 | 说明 |
|---|---|---|---|
| `src/tui/` | `src/apps/tui/` | P3 | P0-08 先删死代码:`cli_dispatch.*`、`tui_init.*`、`tui_context.*`、`agent_callbacks_builder.*`、`terminal_utils.*`、`clipboard_helpers.*`、`ime_windows.*`、`input_event_handler.hpp`、`message_render_cache.cpp`(头文件在用,只删 1 行的 .cpp) |
| `src/tui/settings/` | `src/apps/tui/settings/` | P3 | |
| `src/markdown/` | `src/apps/tui/markdown/` | P2-08 | TESTABLE_TUI 清单同步 |
| `src/commands/` | `src/apps/tui/commands/` | P2-08 | 先移出:`compact` / `compact_prompt`(→ agent/compaction)、`configure*`(→ cli/configure)、`opencode_command`(→ skills,P2-06)、`init_command` 的 `build_*`(→ prompt/init_prompt,P2-07)、`lsp_command` 的 format / dispatch(→ lsp/lsp_status_text,P2-07) |
| `src/skills/skill_commands.*` | `src/apps/tui/commands/` | P2-06 | 同步 testable 清单;`skill_commands_reload_test` 要能链接 |
| `src/path_reference/` | `src/apps/tui/path_reference/` | P2-08 | 改 include `workspace/files_handler.hpp`,切断 TUI→web |
| `src/web/` | `src/apps/web/` | P3 | 内部不动,只改 include 前缀 |
| `src/web/handlers/pinned_sessions_handler.cpp` | 删除 | P0-08 | 1 行空壳;`.hpp` 在用,保留 |
| `src/headless/` | `src/apps/headless/` | P3 | |
| `src/desktop/` | `src/apps/desktop/` | P3 | 只留壳:main、web_host、tray*、splash、agent_browser_host*、daemon_pool、daemon_supervisor、single_instance*、taskbar_badge*、window_*、deepin_window_effects、linux_desktop、context_items、startup_progress、agent_browser_navigation_state、agent_browser_page_directory、url_builder、external_url、application_icon、edge_app_launcher、user_install_policy、instance_startup、pick_active、desktop_about、desktop_restart、dpi_win、webview2_runtime_probe、linux_webview_scale_policy 等(已核对:这些只被 desktop 与 tests/desktop 使用) |
| `src/daemon/` | `src/apps/daemon/` | P3 | 留下 cli、worker、service_win、heartbeat、startup_diagnostics |
| `src/daemon/supervisor.*` | 删除 | P0-08 | 已核对全仓只有自身引用;注意与 `desktop/daemon_supervisor.*` 区分,后者在用 |
| `src/cli/` | `src/apps/cli/` | P3 | |
| `src/commands/{configure,configure_catalog,configure_picker}.*` | `src/apps/cli/configure/` | P2-08 | 只被入口使用 |

## 8. tests/

| 旧路径 | 新路径 | 何时搬 | 说明 |
|---|---|---|---|
| `tests/<old_module>/` | `tests/<module>/` | 随对应源文件(P2 各 PR / P3 M1) | 按模块名镜像,不加分组层 |
| `tests/agent_loop/` | `tests/agent/` | P2-08 | 30 处 `#include "agent_loop.hpp"` 同步改 |
| `tests/permissions_test.cpp` | `tests/permissions/permissions_test.cpp` | P2-05 | |
| `tests/skill_registry_test.cpp` | `tests/skills/skill_registry_test.cpp` | P2-06 | |
| `tests/smoke_test.cpp` | 按被测对象归入模块目录 | P2-08 | 届时确认 |
| `tests/agent_loop/stub_provider.hpp` | `tests/test_support/agent/stub_provider.hpp` | P1-01 | 29 个使用方 |
| `tests/channels/test_support.hpp` | `tests/test_support/channels/test_support.hpp` | P1-01 | 16 个使用方 |
| `tests/sandbox/test_support.hpp` | `tests/test_support/sandbox/test_support.hpp` | P1-01 | 16 个使用方 |
| `tests/themes/theme_test_resources.hpp` | `tests/test_support/themes/theme_test_resources.hpp` | P1-01 | |
| `tests/computer_use/helpers/{native_control_checks,native_smoke_backend,ole_drag_fixture}.hpp` | `tests/test_support/computer_use/` | P1-01 | 冒烟目标使用,同步改 `tests/CMakeLists.txt` |

## 9. 已知需要人工确认的重名

- `runtime.hpp/.cpp` 3 份:`channels/runtime`、`computer_use/runtime`、`tool/web_search/runtime`。模块不同,冻结后不会落到同一目录,只需 lint 确认唯一解析。
- `main.cpp` 2 份:`src/main.cpp` 与 `desktop/main.cpp`,分别落在 `apps/cli/` 与 `apps/desktop/`。
- `pointer_overlay.cpp` 2 份:agent_browser 那份改名为 `browser_pointer_overlay.cpp`(P2-08)。
- `version.hpp`:`upgrade/version` 改名为 `utils/semver`,生成的 `version.hpp` 保持唯一(R9)。
