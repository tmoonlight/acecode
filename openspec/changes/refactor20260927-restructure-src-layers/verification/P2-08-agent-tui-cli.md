# P2-08 agent 与 TUI 归位、清空根目录 验证

认领提交 `e47bc171`(master),基线为 P2-07 合入后的 master `c11e9e14`。工作分支 `refactor20260927/P2-08`,复用 `N:/Users/shao/acecode-p2-04` 工作区与 P2-07 的构建目录 `build/p2-07-session-host`(D26:构建目录跨任务复用,Ninja 增量)。按 D26 只做 Windows 本机验收,不触发 CI。

## 实现边界

- `a8136dd7` `[mechanical]`:`apply_layout.py move --phase P2-02 --phase P2-08`,140 个文件全部 R100、0 行内容改动。src 根目录清空(agent_loop.* → agent/,doom_guard → agent/guards/,main.cpp → cli/,tui_state.hpp → tui/,version.hpp.in → cmake/);commands、markdown、path_reference 整体进 tui/;compact* → agent/compaction/,configure* → cli/configure/,channels/command → cli/channels_cli,drag_scroll / text_input_ops → tui/,prompt_environment → prompt/,side_chat → agent/side_question/,session_replay / session_resume_restore → tui/resume/,message_payload / tool_event_payload → agent/event_payload/,daemon/mcp_runtime → tool/,agent_browser/pointer_overlay.cpp 改名 browser_pointer_overlay.cpp;tests 镜像 59 个文件同步归位(tests/agent_loop → tests/agent,含 P2-02 遗留的 compact_core_test → tests/agent)。搬空的 7 个目录已删除。
- `b6eb7967` `[mechanical]`:`apply_layout.py rewrite`,166 行 include 改写(30 处 `agent_loop.hpp`、7 处 `tui_state.hpp` 等)逐文件增删相等;CMakeLists.txt 的显式 markdown 清单与 version.hpp.in、`cmake/acecode_source_paths.cmake` 的 main / pointer 变量、`tests/cpp_source_paths.json` 按映射换算;87 份文档路径改写并重生成 help 站点(49 篇)。blob 保持 LF。
- 内容提交:
  - `parse_question_policy_value` 自 `cli/interactive_options` 拆入既有的 `tool/question_policy`(该模块原本就是 AskUserQuestion 应答策略的纯函数层),`daemon/cli.cpp` 改 include `tool/question_policy.hpp`,切断 daemon → cli;
  - `restore_file_tool_state_from_messages` 及其文件级辅助(FileToolUse、parse_file_tool_use、restore_file_tool_state、read footer 判定)自 `tui/resume/session_resume_restore` 拆出到新文件 `tool/file_state_restore.{hpp,cpp}`,`session_host/session_registry.cpp` 与 TUI resume 都改调它,切断 session_host → tui;resume 文件去掉不再使用的 apply_patch_format / mtime_tracker / text_file_buffer include;
  - `ACECODE_TUI_DIRS` 只留 `src/tui`(markdown 已在其下,`ACECODE_TUI_TESTABLE_SUBSETS` 的 `markdown/` 子集接管);
  - `tests/smoke_test.cpp` → `tests/utils/smoke_test.cpp`(只有 `Smoke.Passes`,归 base 层镜像目录),映射表与 layout-map.md 同步登记;
  - `src/layers.tsv` 登记 3 条到期例外(R3,`tool_preamble` 的三个消费方 agent_loop.hpp / cli/main.cpp / tui/resume/session_replay.cpp,owner split-agent-loop A-09,2026-11-30 到期):这三条在 P2-08 之前就存在(当时算在 src 根与 session 目录名下),不是搬迁引入;A-09 建 agent/progress 并改经事件 API 后删除;
  - CLAUDE.md 两处花括号形式的旧路径(`src/web/{tool_event_payload,message_payload}.{hpp,cpp}`)手工改到 `src/agent/event_payload/`;
  - `.git-blame-ignore-revs` 登记两个机械提交。
- 不在旧路径留转发头(D24)。

## 静态检查(分支内容提交后)

- `normalize_includes --check` src / tests:0 改动、0 错误。
- `check_layers --enforce-parent-includes`:**0 项**(P2-07 合入后为 79,全部是 src 根文件与 tui_state / agent_loop 头的 R8 / R9 / R3);3 条 R3 由上述到期例外压制。
- `validate_map --strict`、`check_file_size`、`check_ownership`:0。
- `check_doc_paths`:109 项(P2-07 为 102)。新增的 7 项:seed SKILL.md 的 `src/main.cpp`、`src/commands/builtin_commands.cpp`(种子路径留 P3 M2b 的 seed 事务统一改)、`docs/superpowers/plans/2026-08-27-*` 里本来就不存在的计划文件名被按前缀换算、help 搜索索引里的截断片段;CLAUDE.md 的 4 处已手工修正。

## 本机原生验证(Windows,D26 B 档)

构建:MSVC 2022 x64 / Ninja / Release,复用 `build/p2-07-session-host` 增量构建(D26 §6.6),CMake 因清单变化自动重新 configure;`acecode`、`acecode-desktop`、`acecode_unit_tests` 共 142 步(搬迁与 include 改写涉及的编译单元重编)全部成功;五个 EXCLUDE_FROM_ALL 冒烟目标(computer_use_native_smoke、computer_use_broker_smoke、agent_browser_host_smoke、agent_browser_pointer_demo、acecode_upgrade_restart_smoke)只重编改名的 browser_pointer_overlay.cpp 并重新链接,成功。

单测:`run_fast_tests.py --profile fast`(6 分片、隔离 HOME/TEMP,对照 P2-07 的快速档记录):清单 5117 条与 P2-07 相同(smoke_test 只改目录不改用例名),执行 4515 条,9 SKIP 与基线一致,0 失败,601 条慢套件用例未运行(如实记入 JSON),22 秒。首轮曾有 1 条失败 `ChannelBoundaryGuard.ProductSpecificIdentifierDoesNotEnterCoreSurfaces`:它硬编码扫描 `src/commands/remote_control_command.*` 与 `tests/commands/remote_control_command_test.cpp`,搬迁后路径不存在;已改成新路径(同批把 12 个测试文件头部注释与 main.cpp 注释里的旧路径改到新位置),重建后通过。定向补跑(`--filter` 覆盖 AgentLoop*、Compact*、SessionReplay*、SessionResumeRestore*、SideChat*、MessagePayload*、ToolEventPayload*、DragScroll*、TextInputOps*、McpRuntime*、Configure*、Markdown*、PathReference*、BuiltinCommands*、CommandRegistry*、QuestionPolicy*、InteractiveOptions*、Smoke* 等被搬迁 / 拆分模块):执行 471 条,0 失败。

target 快照:对 P2-07 的本机快照与本分支的快照都按映射表反查换算后对照,59 个目标无增删;新增 7 个元组全部是新文件 `tool/file_state_restore.{hpp,cpp}` 及其 `.obj` 在 5 个链接目标里的引用,移除 0;140 个搬迁文件(含 tests/utils/smoke_test.cpp)换算后归属与编译参数不变。

## 不变量与合入条件

- §7.3 第 1 条守护测试 `AgentLoopTermination.RequestPrefixIsByteStableAcrossIterationsInATurn` 与 `SystemPromptTest.*ByteStable*` 在快速档里强制补跑通过。
- 两处拆分只搬函数体,不改签名与行为;`parse_question_policy_value` 与 `restore_file_tool_state_from_messages` 的既有覆盖(`tests/cli/interactive_options_test.cpp`、`tests/tui/resume/session_resume_restore_test.cpp`、`tests/tool/question_policy_test.cpp`)随镜像目录一起运行。
- 合入前已把 master 合进本分支;多平台验证按 D26 留 tasks.md 5.4。
