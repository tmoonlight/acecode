# Tasks: refactor20260927-restructure-src-layers

> 执行前必读 design.md 的 §6「提交与协作约定」。关键规则:
> - 开工前在任务行末尾追加 `〔认领: <代理名> <日期>〕`,单独提交到 master;前置任务没勾选的不开工。
> - 提交信息前缀写 `refactor20260927(layers/<任务编号>): …`。
> - 热点文件规则见 design.md §6 第 5 条:
>   - agent_loop.* 与 main.cpp 同一时刻只允许一个任务修改;
>   - CMakeLists.txt、tests/CMakeLists.txt 可以并行开发,但合入 master 要串行,后合入的一方先 rebase,再重跑 cmake_target_snapshot 比对。
> - 各任务的执行波次与可并行关系见 design.md §8.3。
>
> 其它 change 的任务:
> - P0-09、P0-12 在 split-tui-main;
> - P0-10、P0-11 在 split-agent-loop;
> - P2-01 在 adopt-ownership-conventions。
>
> 标记含义:【主】判断密集或改热点文件,由负责本 change 的主代理串行完成;【子】可以交给子代理或 Codex 独立完成;【并】可以和同阶段其它任务并行。

## 1. Phase 0:决策、护栏、清理(不冻结)

- [ ] 1.1 【P0-01】【主】立项定稿。
  - 核对 design.md §9 决策登记,D1/D2/D15/D21/D22 已定;
  - 在 AGENTS.md 增加「所有权与生命周期」一节,约定摘自 adopt-ownership-conventions design.md,每条附一个仓库中的真实反例;
  - CLAUDE.md 顶部加一行,指向本系列 4 个 change,并写明「src 分层重构进行中,新文件放置规则见 restructure-src-layers/design.md」。
  - 验证:4 个 change 都通过 `openspec validate --strict`;AGENTS.md / CLAUDE.md 的 diff 只含上述段落。
- [ ] 1.2 【P0-02】【子】【并】分支盘点。
  - 用 `scripts/refactor/branch_inventory.sh` 输出每个 worktree / 分支的领先提交数、`git cherry` 结果、src 与 tests 改动数、是否有脏文件,存档到本 change 目录的 `branch-inventory.md`;
  - 28 个补丁等价的旧 worktree 列出清单,**交给用户确认后由用户自行删除**,本任务不删除任何东西;
  - `codex/add-self-session-control` 与 `review-ai-image-sharing-tool` 两个单提交例外,记录处理建议:先合入,或走 patch 迁移。
  - 验证:盘点表已存档,9 个带独有 src 改动的 ref 逐一列出。
- [ ] 1.3 【P0-03】【子】【并】迁移与 lint 工具集,全部以报告模式运行。在 `scripts/layers/` 与 `scripts/refactor/` 下新增:
  - `repo_files.py`:文件清单只取 `git ls-files`;按字节逐行读写,保留 CRLF/LF;前缀替换带尾部斜杠并锚定边界;
  - `check_layers.py`:实现 R1–R14;
  - `normalize_includes.py`:幂等,支持 `--check`,平台 `#if` 块里的 include 也按文本处理;
  - `check_file_size.py`:37 个超限文件入基线,web 与 stb 标注豁免;
  - `check_ownership.py`:统计 R15 的五类指标;
  - `check_line_coverage.py`:按「原行号 → 新文件」映射检查原文件每一行都有去处;
  - `gtest_inventory.py`:用例清单与 SKIP 清单;
  - `cmake_target_snapshot.py`:基于 CMake File API,包含 EXCLUDE_FROM_ALL 冒烟目标;
  - `check_doc_paths.py`;
  - `validate_map.py`;
  - `branch_inventory.sh`;
  - 按 layout-map.md 生成 `src/layers.tsv` 初版与 `scripts/refactor/src_layout_map.tsv`。
  - 验证:
    - 在当前 master 上跑出全部基线;
    - `normalize_includes --check` 统计出约 1124 行 `../`、366 个文件,与调研数字相符;
    - **用正式 `layers.tsv` 重跑分层检查,把实测违规数写回 design.md「D2」段**;
    - 在一个混排行尾的样本文件上确认行尾不变;
    - 脚本在含嵌套 worktree 的仓库根运行时,不触碰 `.claude/worktrees`、`.worktrees`、`.acecode/worktrees`。
- [ ] 1.4 【P0-04】【主】【并】CMake 源文件护栏。
  - `CMakeLists.txt:176/183` 的正则拆成 `ACECODE_TUI_DIRS` 与 `ACECODE_TUI_TESTABLE_SUBSETS`(预先写入 commands/、resume/、path_reference/、markdown/,以及 drag_scroll、text_input_ops、skill_commands),TUI 源集合为空时 `FATAL_ERROR`;
  - 新增 `cmake/acecode_source_guards.cmake`(`acecode_require_sources`、`acecode_set_source_define`、`acecode_assert_known_roots`),覆盖:
    - 全部显式清单(:185-314);
    - 11 个 `.mm`;
    - REMOVE_ITEM;
    - `:18` 的 `ACECODE_DEEPIN`;
    - `:416` 的 `ACECODE_CHANNEL_ASSET_DIR`;
    - `:369-377` 的 OBJCXX。
  - computer_use 的路径统一写成 `${CMAKE_SOURCE_DIR}`;
  - `acecode_winpty.cmake:64`、`cmake/deepin/CMakeLists.txt:15`、`acecode_desktop.cmake:74-88` 改为共用变量;
  - `tests/CMakeLists.txt` 中 EXCLUDE_FROM_ALL 冒烟目标的源路径,改为引用根 CMake 导出的变量。
  - 前置:1.3。
  - 验证:
    - cmake_target_snapshot 与 G0 逐元组相同;
    - 故意改坏一个显式清单路径、一个 set_property 路径,configure 都必须 FATAL;
    - TUI 源集合非空。
- [ ] 1.5 【P0-05】【子】【并】测试路径健壮化。
  - 新增 `tests/test_support/repo_root.hpp::find_repo_root()`,向上查找同时含 CMakeLists.txt 与 .git 的目录;用它替换 7 个文件中 17 处固定层数的 `parent_path()` 链:
    - `hooks/hook_registry_test.cpp:46`;
    - `skills/default_skill_seeder_test.cpp` 9 处;
    - `skills/ai_theme_seed_test.cpp:31`;
    - `channels/bridge_test.cpp:44`;
    - `web/models_handler_test.cpp:308`;
    - `web/model_catalog_handler_test.cpp:80`;
    - `web/web_server_smoke_test.cpp:10128`。
  - `channel_boundary_guard_test`:`:19` 改用 `find_repo_root()`,`:68-72` 断言扫描根存在且非空;
  - `bridge_test`:区分「路径错误」(FAIL)与「没装 node_modules」(SKIP);
  - 按规范写中文注释,说明回归时的症状是静默 SKIP 或空转通过。
  - 验证:
    - 用例清单不变;
    - 把测试源复制到加深一层的目录后,仍能找到仓库根;
    - `bridge_test` 在缺 node_modules 时 SKIP,路径错误时 FAIL。
- [ ] 1.6 【P0-06】【子】【并】前端架构测试路径表与脚本(D4)。
  - 9 个 `*Architecture*.test.js` 读取的 C++ 路径收敛到 `tests/cpp_source_paths.json`;`desktopCloseDialogArchitecture.test.js:82` 的分段拼接要人工核对;
  - `scripts/code_quality_check.{sh,bat}` 去掉写死的 `src/tool/*.cpp`。
  - 不改任何 React 代码。
  - 验证:`pnpm test` 通过;把表中任一路径改坏,对应测试必须明确失败。
- [ ] 1.7 【P0-07】【子】【并】CI 与基线 G0。
  - `.github/workflows/test.yml` 新增 layer-lint job(Linux,报告模式,排在 C++ 构建之前);
  - 新增只能手动触发的 `refactor-matrix` job(windows-2022 / macos-15,构建并运行 `acecode_unit_tests`);
  - 采集 G0:四个平台的 target 快照、gtest 清单与 SKIP 清单、四类 lint 基线,存档到本 change 目录的 `baseline/`。
  - 前置:1.3。
  - 验证:手动 dispatch 一次 refactor-matrix,Windows 与 macOS 跑完 ctest。原本就失败的用例只记入基线,不作为阻断条件。
- [ ] 1.8 【P0-08】【主】删死代码,约 −1950 行。以下每项删除前都要再 grep 一次,确认没有外部引用:
  - `src/tui/{cli_dispatch,tui_init,tui_context,agent_callbacks_builder,terminal_utils,clipboard_helpers,ime_windows}.{hpp,cpp}`、`src/tui/input_event_handler.hpp`、`src/tui/message_render_cache.cpp`(1 行的空 .cpp,头文件在用,保留);
  - `src/main.cpp:1395-1617` 的 IME 死代码(`update_ime_composition_window` 从未被调用),以及 `:35-37` 的 `<imm.h>` 与 `#pragma comment(lib,"Imm32.lib")`;`CMakeLists.txt:538-542` 的 imm32 链接与 `:539` 的注释;
  - `src/web/handlers/pinned_sessions_handler.cpp`(1 行空壳,.hpp 保留);
  - `src/provider/auth/acecode.code-workspace`;
  - `src/daemon/supervisor.{hpp,cpp}`(注意与 `desktop/daemon_supervisor.*` 区分,后者在用);
  - `src/hooks/hook_payload.cpp:5` 的无用 include;
  - `docs/help-source` 里对 `cli_dispatch.cpp` 的引用(改 group*.py 后重新生成),以及 `tests/README.md:34`。
  - 前置:1.2。
  - 验证:
    - 四平台全新目录构建通过;
    - 单测全绿;
    - 手工确认 Windows 上微软拼音候选窗的位置与删除前一致;
    - 用例清单与 G0 相同。

## 2. Phase 1:include 规范化(1 个脚本 PR,不冻结)

- [ ] 2.1 【P1-01】【子】include 改为模块根形式。分三个提交:
  - (a) 7 个测试 helper 头 `git mv` 到 `tests/test_support/<area>/`,纯改名 R100(映射见 layout-map.md §8),同时在 `tests/CMakeLists.txt` 把 `${CMAKE_SOURCE_DIR}/tests` 加为 include 根;
  - (b) 脚本把 src 下 366 个文件、1124 行 `../` 与子目录相对写法改成 `"<模块>/…"`,同目录裸名保留;
  - (c) tests 中指向 helper 的 16 行 `../` 与 37 行同目录 include,改为 `"test_support/<area>/…"`。
  - 前置:P0-01、1.3、1.4、1.5、1.7。
  - 验证:
    - numstat 满足每个文件「增加 = 删除 = 改动的 include 行数」;
    - 第二次运行 0 diff;
    - src 下不再有 `../`;
    - Windows 本地全新目录构建;手动 dispatch package.yml,覆盖 mac/arm/Deepin;
    - 用例清单与 SKIP 清单等于 G0;
    - 公告 9 个遗留分支:先在自己的分支上跑同一个脚本,再 rebase。
- [ ] 2.2 【P1-02】【子】lint 阻断 `../`,并把 2.1 的机械提交写进 `.git-blame-ignore-revs`。
  - 验证:在 CI 上故意新增一行 `../` include,layer-lint job 失败。

## 3. Phase 2:冻结前的模块重定位与拆头(约 12–15 个小 PR,不冻结)

> **每个 PR 的通用要求**:
> - lint 违规数单调下降;
> - 旧路径留 2 行转发头,登记进 `layers.tsv` exceptions,冻结当天到期;
> - 新模块以 `src/<模块>/` 的形式建立,include 写法与冻结后一致;
> - 对应测试一起 `git mv` 到 `tests/<新模块>/`;
> - 凡是移进 `src/tui/` 的文件,同一个 PR 更新 `ACECODE_TUI_TESTABLE_SUBSETS`;
> - cmake_target_snapshot 按映射换算回旧路径后,每个文件所属的 target 不变;
> - 跑一次 `migrate_branch.py --check`,并通知 9 个遗留分支。
>
> **执行顺序**:3.1、3.2、3.3 可并行;3.4 依赖 3.1;3.5 依赖 3.1、3.4;3.6 依赖 3.5;3.7 放最后,且要求 P0-09、P0-10、P0-11 已先合入;3.8 在 1.3 之后全程并行。P2-01(RAII 原语)在 adopt-ownership-conventions,可与本组并行,但必须在 Phase 3 之前合入。

- [ ] 3.1 【P2-02】【主】共同协议根 `llm/`,对应 layout-map.md §3 前 7 行。
  - `provider/llm_provider.hpp` 整头移到 `src/llm/`,删掉第 3 行的 retry_policy include;
  - `tool_protocol_names`、`model_family`、`tool_icons` 移到 llm/;
  - 拆出 `llm/tool_result.hpp`;
  - `commands/compact.hpp` 按使用方拆出 `llm/token_estimate`、`llm/message_predicates`、`llm/context_thresholds`,剩余部分留在 compact;
  - context_usage codec 移到 `llm/context_usage`;
  - `strip_text_preamble_tags` / `TextPreambleScanner` 移到 `llm/text_preamble_tags`;
  - `tool/diff_*` 移到 utils/;
  - `pa_quirks.hpp:9` 改指 `llm/llm_provider.hpp`。
  - 验证:
    - 三平台构建通过;
    - lint 显示 provider↔session、provider↔tool、provider↔pa 三个环消失;
    - `system_prompt.cpp` 对 compact.hpp 的无用 include 已删除;
    - byte-stable 用例与 tool_preamble 测试通过。
- [ ] 3.2 【P2-03】【子】【并】平台件下沉到 `platform/` 与 `pty/`,对应 layout-map.md §2 中标 P2-03 的行。
  - `hooks/hook_runner` → `platform/process/process_runner`(ProcessSpec,hooks 保留别名);
  - `lsp_process` → `piped_process`,`lsp_which` → `which`;
  - `daemon/platform*` → `os_process*`;
  - `web/pty` → `src/pty/`,同步改 `acecode_winpty.cmake:64`;
  - desktop 的 locale 与 native_ui 件、utils 的 clipboard/open_url/power_inhibitor/terminal_*、`upgrade/console` 移到 platform。
  - 验证:
    - 构建通过,lint 违规数下降;
    - Windows 上 ConPTY 与 winpty 控制台都能打开(Web 控制台停靠区冒烟);
    - desktop 构建通过,且不链接 acecode_testable。
- [ ] 3.3 【P2-04】【子】【并】跨进程协议与工作区。
  - runtime_files、guid、open_request、daemon_protocol、agent_browser_runtime → `src/ipc/`;
  - workspace_registry、`web/handlers/files_handler` → `src/workspace/`;
  - `config.cpp:485-501` 的 `get_*_dir` → `utils/paths`。
  - 验证:
    - 构建通过;
    - desktop 冒烟:启动、打开 workspace;
    - lint 显示指向 desktop、web 的反向边消失;
    - files_handler 相关测试通过。
- [ ] 3.4 【P2-05】【主】config 与 utils 的反向边,对应 layout-map.md 中标 P2-05 的行。
  - `permissions.hpp` 拆出 `config/vocab/permission_mode.hpp`;theme_id、pointer_appearance 移到 vocab;
  - builtin_model_catalog 与 models_dev_catalog 的纯部分移到 config,依赖 registry 的部分移到 `provider/models_dev_catalog_cache`;
  - state_file 的专用函数搬回各自使用方;
  - file_operations、tool_errors、tool_args_parser → tool/;text_file_buffer 拆出 `tool/safe_text_write`(`routes_files.cpp:325` 只改调用点);
  - token_tracker → session;
  - path_validator、shell_guard、permissions.hpp → permissions/;headless_mode → `permissions/interaction_mode`;
  - upgrade/http → network;upgrade/version → `utils/semver`;
  - tests 中 38 处裸 `permissions.hpp` include 改掉;`tests/permissions_test.cpp` 移到 `tests/permissions/`。
  - 前置:3.1。
  - 验证:
    - 构建通过;
    - permissions_test 与 MCP 配置拦截测试通过;
    - lint 显示 config 环与 themes→upgrade 边消失。
- [ ] 3.5 【P2-06】【子】hooks 与 skills 的横向边。
  - hook_payload 的三个 provider 构造器 → `src/agent/hook_bridge/hook_events`;
  - hook 种子拆出 `hooks/hook_seeds`;
  - `skill_commands` → `src/tui/commands/`,同步 testable 子集;
  - frontmatter → utils;
  - `web/handlers/skill_command_expander` 与 `commands/opencode_command` → `skills/`;
  - `tests/skill_registry_test.cpp` 移到 `tests/skills/`。
  - 前置:3.1、3.4。
  - 验证:hooks、skills、seeder 相关测试全部通过;`skill_commands_reload_test` 能链接并运行;lint 显示 hooks→provider、hooks→skills、memory→skills、skills↔commands 边消失。
- [ ] 3.6 【P2-07】【主】编排层上移到 `session_host/`。
  - 先拆出 `prompt/init_prompt`(自 `commands/init_command` 的 `build_*`)与 `lsp/lsp_status_text`(自 `commands/lsp_command`);
  - 再把 session_registry、local_session_client、thread_service、task_suggestion_service、session_auto_title、session_title_generator 移过去,其中纯函数留在 `session/session_title_text`;
  - apply_model_to_session 也移过去;
  - spawn_subagent、thread、task_suggestion 三类工具移到 `session_host/tools/`。
  - 前置:3.5。
  - 验证:session_registry、spawn_subagent、web_server_smoke 测试通过;lint 显示 `session_registry.cpp:12-13 → commands` 与 provider→agent_loop 两条边消失。
- [ ] 3.7 【P2-08】【主】agent 与 TUI 归位,清空根目录。
  - `web/{message_payload,tool_event_payload}` → `src/agent/event_payload/`;
  - `session/side_chat` → `src/agent/side_question/side_chat`;
  - `commands/{compact,compact_prompt}` 剩余部分 → `src/agent/compaction/`,必须先于 commands 整体移动;
  - prompt_environment → prompt;
  - `daemon/mcp_runtime` → tool;
  - `parse_question_policy_value` → `tool/question_policy`;
  - session_replay、session_resume_restore → `src/tui/resume/`;restore_file_tool_state → `tool/file_state_restore`;
  - markdown、commands、path_reference、`tui_state.hpp`、`utils/{drag_scroll,text_input_ops}` → `src/tui/`;
  - `commands/configure*` → `src/cli/configure/`,`channels/command` → `src/cli/channels_cli`;
  - `agent_loop.*` R100 → `src/agent/`,doom_guard → `src/agent/guards/`;
  - `main.cpp` R100 → `src/cli/main.cpp`,`version.hpp.in` → `cmake/`;
  - `tool/agent_browser/pointer_overlay.cpp` 改名为 `browser_pointer_overlay.cpp`;
  - 测试镜像:`tests/agent_loop` → `tests/agent`,30 处 `agent_loop.hpp` 与 7 处 `tui_state.hpp` 改掉;commands、markdown、path_reference、session_replay*、drag_scroll、text_input_ops 的测试随源文件归位;`smoke_test.cpp` 按被测对象归位。
  - 前置:3.6;以下三项已合入,因为它们修改或新增的文件会被本任务移动:
    - split-tui-main 的 P0-09(改 `src/main.cpp`);
    - split-agent-loop 的 P0-10(改 `src/agent_loop.*`);
    - split-agent-loop 的 P0-11(在 `tests/agent_loop/` 下新增测试)。
  - 验证:
    - lint 在转发头之外 0 违规;
    - 用例清单与 G0 相同;
    - `CMakeLists.txt:173/507/548` 已同步;
    - 按平台用 `cmake --build --target` 构建全部 EXCLUDE_FROM_ALL 冒烟目标。
- [ ] 3.8 【P2-09】【子】【并】分支迁移工具 `scripts/refactor/migrate_branch.py`,提供 rebase / patch / `--apply-map` / `--docs` / `--check` 五种模式。
  - 前置:1.3(需要映射表);可与 Phase 1、Phase 2 全程并行。
  - 验证:对 9 个遗留 ref 逐一演练,patch 模式下 `git apply -3` 成功,或在记录里说明为什么不能。

## 4. Phase 3:冻结窗口(半天,外加约 1 天验证)

- [ ] 4.1 【P3-01】【主】演练。
  - 在临时 worktree 里用 `apply_layout.py` 从固定 base 生成 M1 / M2 / M2b / M3,跑完 design.md §7.2「P3」一行的全部闸门,记录耗时,用来估算冻结窗口;
  - 演练分支不推到 master。
  - 前置:Phase 2 全部完成;adopt-ownership-conventions 的 P2-01 已合入。
  - 验证:全部闸门通过,耗时已记录。
- [ ] 4.2 【P3-02】【主】正式搬迁。
  - 提前 1–2 天在 AGENTS.md / CLAUDE.md 公告窗口;打 tag `pre-src-layout`;在**最新 master 上重新生成**以下提交,不 rebase 演练结果:
    - **M1** `[no-build]`:约 44 个模块目录(含 tests 镜像)`git mv` 到 6 个分组下,stb 移到 `external/stb`,全部 R100,include 改动 0 行;
    - **M2** `[mechanical]`:
      - 新增 `acecode_include_roots`(6 根 + generated;stb 以 SYSTEM 只挂 base);
      - 删除各处的 `${CMAKE_SOURCE_DIR}/src` 根:`CMakeLists.txt:321-323/360-362/409-414/472/493`、`acecode_desktop.cmake:105-108`、`deepin/CMakeLists.txt:19`、tests 约 11 行;
      - TUI 目录变量改为 `apps/tui`,并带上空集断言;
      - 显式清单按映射做带边界的前缀替换,更新按源设置的属性与 `.mm` 路径;
      - `tests/CMakeLists.txt` 约 27 行;
      - 删除全部转发头;
      - 文档路径:CLAUDE.md 约 97 处、ARCHITECTURE.md 约 61 处、AGENT(S).md 约 41 处、docs 约 578 处(先改 group*.py 再跑 build_help.py);`tests/cpp_source_paths.json`;scripts;
    - **M2b**:seed SKILL.md 的 6 处路径,并 bump `seed.version`、MANIFEST 的 `bundle_version` 与 `skill_md_sha256`、测试中硬编码的 bundle 版本;
    - **M3**:把 M1/M2 写进 `.git-blame-ignore-revs`。
  - 以 `--no-ff` 或 fast-forward 合入,禁止 squash;打 tag `post-src-layout`;解除冻结。
  - 前置:4.1、1.2。
  - 验证:
    - `git diff -M100% --name-status pre-src-layout M1` 全部为 R100;
    - design.md §7.2「P3」一行全部通过(target 快照逐元组、三个 lint 为 0、用例与 SKIP 清单、package.yml 全平台且 Deepin 的 `current_target()` 为 linux-deepin、`pnpm test`、Windows verify-package、冒烟五项)。
    - 风险处理:窗口期间 master 被推进时,丢弃已生成的提交,在新 master 上重新生成。
- [ ] 4.3 【P3-03】【子】解冻后的分支迁移支持。
  - 9 个遗留 ref 用 `migrate_branch.py` 的 patch 模式迁移,或在盘点表里标记弃用;
  - AGENTS.md / CLAUDE.md 写明映射表版本。
  - 验证:每个迁移后的 ref,`migrate_branch.py --check` 0 违规才允许合入。

## 5. Phase 4:搬迁收尾

- [ ] 5.1 【P4-01】【子】lint 切到 `--strict`,exceptions 必须为空;注册 `ctest layer_lint`;pre-push 钩子可选,只检查改动文件。
  - 验证:本地 `ctest -R layer_lint` 通过;故意新增一条向上依赖,lint 失败。
- [ ] 5.2 【P4-02】【子】叙述性文档。
  - 新增 `docs/architecture/src-layout.md`:层定义、依赖规则、「新文件放哪」决策表、指向 `src/layers.tsv` 的链接;
  - 重写 ARCHITECTURE.md 的结构章节,改为引用它;更正 AGENTS.md / AGENT.md 里过时的「根目录 main.cpp」;
  - CLAUDE.md 顶部改为正式的分层说明,替换 P0-01 的「进行中」提示;
  - 重写 `tests/README.md` 的镜像表;
  - 进行中的 openspec change 各加一行映射说明;
  - 提醒用户手工更新自动记忆中约 16 处路径。
  - 验证:`check_doc_paths.py` 为 0;help 站点重新生成后,diff 只涉及路径。
- [ ] 5.3 【P4-03】【子】行数、分层、所有权三个棘轮在 CI 中转为阻断。
  - 验证:CI 上故意新增一个超过 1000 行的文件、一条向上依赖、一处 `.detach()`,三个 lint 分别失败。
