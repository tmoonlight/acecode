# P2-04 跨进程协议与工作区验证记录

认领 Claude-p2-04,2026-09-27(`fdb91d1f`)。分支 `refactor20260927/P2-04`,在 master `9de03443`(含主代理同日的三条 P2 工具修正:`187bdd50` normalize_includes 解析过渡目录、`d40762c7` / `bf1ef7db` 快照 `--reverse-map` 含对象文件路径)上按 tasks.md 3.3 与 D24(2026-09-27:旧路径不留转发头、`layers.tsv` 不加 exception 行)分组提交,收尾时再合入含 P2-02 的 master `ec64edcb`:

| 提交 | 内容 |
|---|---|
| `05ab77d5` [mechanical] | 17 个文件 `git mv`(全部 R100,内容零改动):`src/daemon/{runtime_files.hpp,runtime_files.cpp,guid.hpp}`、`src/desktop/{open_request,agent_browser_runtime}.{hpp,cpp}`、`src/desktop/daemon_protocol.hpp` → `src/ipc/`;`src/desktop/workspace_registry.{hpp,cpp}`、`src/web/handlers/files_handler.{hpp,cpp}` → `src/workspace/`(D21,`namespace desktop` 暂不改);5 个测试镜像到 `tests/ipc/`、`tests/workspace/`。对应 `src_layout_map.tsv` 里 phase=P2-04 的 17 条 move 行 |
| `295041e7` [mechanical] | `normalize_includes.py --scope src`(22 文件 31 行)/ `--scope tests`(12 文件 13 行):旧模块根写法(`desktop/workspace_registry.hpp`、`daemon/runtime_files.hpp`、`web/handlers/files_handler.hpp` …)与旧目录里的同目录裸名(`runtime_files.hpp`、`guid.hpp`、`daemon_protocol.hpp`、`agent_browser_runtime.hpp`、`open_request.hpp`、`workspace_registry.hpp`、`files_handler.hpp`)统一改为 `ipc/…`、`workspace/…`,`.mm`(`agent_browser_host_mac.mm`)同样处理;`--check` 两个范围 exit 0;numstat(histogram)44/44,`-U0` 补丁的 +/- 行全部是 `#include "` 行 |
| `b2439902` | `src/ipc/runtime_files.cpp` 里工具无法解析的裸名 `platform.hpp`(留在 daemon、P2-03 要搬的文件,映射目标尚不存在,normalize 按规则不猜测)手工改为 `daemon/platform.hpp`;lint 按映射把它归到 platform(rank 2)< ipc(5),不产生新违规,P2-03 合入后重跑 normalize 会改到 `platform/process/os_process.hpp` |
| `82cae6a8` | `get_acecode_dir` / `get_run_dir` / `get_logs_dir` 从 `config/config.{hpp,cpp}`(原 485-501 行)逐字节搬到 `utils/paths.{hpp,cpp}`(`paths.cpp` 新增同目录 `#include "constants.hpp"` 取 `SUBDIR_RUN` / `SUBDIR_LOGS`);`config.hpp` 改为 `#include "utils/paths.hpp"` 透传(utils 0 < config 3,合法);20 个只为这三个函数 include `config/config.hpp` 的文件改 include `utils/paths.hpp`;`size_baseline.txt` 的 config.cpp 上限 2875 → 2857 |
| `c2b217f9` | `CMakeLists.txt` `ACECODE_NATIVE_BRIDGE_SUPPORT_SOURCES` 只改四行 + ws2_32 注释里的文件名;`tests/cpp_source_paths.json`(D4)两个值;CLAUDE.md / docs / web/README.md 路径字面量 |
| `957fb508` | 合入 origin/master `ec64edcb`(P2-02 `94938fa4`、P1 验收记录、Mermaid PR #75、P2-01 时序放宽)。两处冲突都在 include 块:`session/session_storage.cpp`(P2-02 把 `prompt/context_usage_breakdown.hpp` 换成 `llm/context_usage.hpp`,本任务把 `config/config.hpp` 换成 `utils/paths.hpp`)与 `session/thread_service.cpp`(P2-02 把 `commands/compact.hpp` 换成 `llm/message_predicates.hpp` + `llm/token_estimate.hpp`,本任务同样只删 config 行),解决办法是两边都保留:取 P2-02 的 llm/ 头、去掉 config 头;之后 `normalize_includes --check` 两个范围 exit 0。`CMakeLists.txt` 没有冲突(P2-02 未改显式清单) |
| (本记录所在提交) | 本记录 + `branch-inventory.md`「P2-04 公告」 |

开工时按简报旧口径做过「7 个转发头 + 7 条 exception 行」并为此改过 `validate_map.py` 与一个工具自测夹具;D24 生效后整组丢弃,分支在 `9de03443` 上重放(未推送过,无 rebase 已推送提交的问题),源码树与丢弃前逐文件相同(`git diff` 只剩 7 个转发头、`layers.tsv` 与 4 个工具文件的差异,均为回到 master 状态)。

## 1. §7.2「P2(每个 PR)」闸门逐项

| 闸门 | 结论 | 证据 |
|---|---|---|
| lint 违规数下降 | **未上升、也没有下降:合入前 master 161 → 本分支 161;合入 P2-02 后的 master 153 → 本分支 153。任务点名的反向边在过渡模式 lint 里本来就不计为违规**(第 5 节有解释与按物理位置核对的反向边表) | `check_layers.py --enforce-parent-includes` 前后各一份 JSON,findings 集合与对应的 master 逐条相同(仅 `session_storage.cpp` / `hook_registry.cpp` 两条既有 finding 因上方删了一行 include 而行号 -1) |
| 转发头已登记 | 按 D24 不适用:旧路径不留任何文件,`layers.tsv` 未动(`exceptions_used` 为空) | `git ls-files src/daemon/runtime_files.hpp` 等 7 个旧路径均为空;`validate_map --strict` exit 0,17 条 P2-04 行进入 `planned_or_obsolete_rows`(旧路径已不存在) |
| 按映射换算后每个文件所属的 target 不变 | 满足:59 target / 3529 元组,`--reverse-map` 后与参照逐元组相同,0 增 0 减(唯一差异是 25 条 `nlohmann_json.natvis` 元组的 vcpkg 路径前缀,环境差异,归一后 `compare_snapshots.py` `unexpected=false`) | 第 4 节 |
| 测试已随源文件移动 | 满足 | `05ab77d5` 的 5 条 tests R100;`tests/CMakeLists.txt` 按 `*_test.cpp` GLOB_RECURSE 自动发现 `tests/ipc/`、`tests/workspace/` |
| 三平台构建通过 | 本机 Windows 全新目录通过(第 2 节);Linux / macOS 留给主代理合入前的 `refactor-matrix` | `82cae6a8` 改 include 的 20 个文件里,非 Windows 的 `#else` / `__APPLE__` / `__linux__` 分支逐段核对过,只用到 `<string>` / `<optional>` / `<vector>`(`utils/paths.hpp` 自带)与各自已 include 的头;`config.hpp` 透传 include 保证只经它间接拿到这三个函数的文件(含平台专属 .cpp / .mm)照常编译 |
| 用例清单不变 | 满足:`tests` 名单 5115、ctest 注册名 5119、SKIP 集合 9 条与参照逐条相同,0 增 0 减,失败集合前后都为空 | 第 3、4 节 |

## 2. 本机构建(Windows,全新目录 `build-p2`)

Ninja + MSVC 2022,Release,`BUILD_TESTING=ON`,`ACECODE_BUILD_DESKTOP=ON`,x64-windows-static,只读复用 `N:/Users/shao/acecode/build/vcpkg_installed`,configure 前写 File API query;脚本 `N:\Users\shao\acecode-p2-04-rec\build-p2.cmd`(先 `vcvars64`)。

| 步骤 | 结果 |
|---|---|
| configure | 通过(`acecode_assert_known_roots` / `acecode_require_sources` 护栏均通过;`src/ipc/`、`src/workspace/` 被 GLOB_RECURSE 收进 `acecode_testable`,四个显式清单路径命中) |
| `cmake --build --target acecode acecode-desktop acecode_unit_tests --parallel 8` | 通过(全新目录首轮 1020 步,17:34–17:40,0 error / 0 warning-as-error) |
| `--target computer_use_native_smoke computer_use_broker_smoke agent_browser_host_smoke agent_browser_pointer_demo acecode_upgrade_restart_smoke` | 通过(`acecode_upgrade_restart_smoke` 的源 `tests/desktop/helpers/upgrade_restart_smoke.cpp` include 了搬走的 `ipc/runtime_files.hpp`、`ipc/daemon_protocol.hpp`,一并证明新 include 根生效) |
| D24 重放后的增量重建(同一目录 `build-p2`,同一脚本:re-configure + 主目标 392 步 + 冒烟目标 9 步,21:13–21:19) | 通过,exit 0,0 error |
| 合入 P2-02 之后的增量重建(同一目录、同一脚本:re-configure + 主目标 421 步 + 冒烟目标 4 步,21:27–21:33) | 通过,exit 0,0 error;第 4 节的快照与第 3 节的单测都取自这次重建产物 |

## 3. 单测

### 3.1 desktop 冒烟(启动、打开 workspace)的替代验证

开跑前 `tasklist` 显示用户正开着桌面版(`acecode-desktop.exe` PID 13680,`~/.acecode/logs/desktop-2026-09-27.log` 19:46 / 19:49 两次启动,并带着两个 `acecode.exe` daemon),且本会话的 computer-use 屏幕访问申请被用户拒绝,没有截图手段。按简报约定改为跑 `tests/desktop` + `tests/workspace` + `tests/ipc` 的全部用例(37 个测试文件、54 个 suite,过滤器 `N:\Users\shao\acecode-p2-04-rec\subset-flags.txt`,隔离 HOME `N:\acecode-p2-04-iso2`):

```text
build-p2\tests\acecode_unit_tests.exe --gtest_flagfile=subset-flags.txt --gtest_output=xml:subset-results.xml
[==========] 383 tests from 54 test suites ran. (717 ms total)
[  PASSED  ] 382 tests.
[  SKIPPED ] 1 test: NativeNotifications.OptInDeliversSelfDrawnToast(需 ACECODE_RUN_NOTIFICATION_SMOKE=1 的手动冒烟,与基线一致)
```

合入 P2-02 前后各跑一次,结果相同:0 失败(用户桌面版正在运行的情况下 `DesktopSingleInstance.*` 7 项也全部通过)。其中搬走的测试文件对应的 suite:`FilesHandler` 25 项全过(任务行点名「files_handler 相关测试通过」)、`WorkspaceRegistry` / `WorkspaceRegistryProbeCache` / `WorkspaceRegistryOrder` / `WorkspaceRegistryDefault` 23 + 4 + 6 + 6 项全过、`DaemonRuntimeFiles` 15 项全过、`AgentBrowserRuntime` 5 项全过、`DesktopOpenRequest` 5 项全过;引用搬走头的 `DaemonPool`(23)/ `PickActive`(6)/ `WorkspaceProfile`(10)同样通过。结果 XML:`N:\Users\shao\acecode-p2-04-rec\subset-results.xml`。

### 3.2 全量单测(隔离 HOME / TEMP:`N:\acecode-p2-04-iso\{home,tmp}`,仓库外)

```text
python scripts\refactor\gtest_inventory.py --binary N:\Users\shao\acecode-p2-04\build-p2\tests\acecode_unit_tests.exe --run --ctest-dir N:\Users\shao\acecode-p2-04\build-p2 --timeout 7200 --output N:\Users\shao\acecode-p2-04-rec\gtest.json
```

USERPROFILE / HOME / APPDATA / LOCALAPPDATA / TEMP / TMP 全部指到仓库外(`N:\acecode-p2-04-rec\test-run.cmd`),对合入 P2-02 之后重建的 `build-p2` 跑,21:34–21:40:

| 项目 | 参照(P2-02 之后的 master,同一台机器) | P2-04(合入后,HEAD 的 `957fb508`) |
|---|---|---|
| 列出 / 执行 / SKIP / 失败 | 5115 / 5114 / 9 / 0 | **5115 / 5114 / 9 / 0** |
| ctest 注册名 | 5119 | 5119 |
| SKIP 清单 | 9 条 | 同一 9 条:`ChannelBridge.RealBridgeStartsWithoutConnectingAnAccount`、`ChannelSetup.RealDependencyInstallationIsOptionalAndUsesTemporaryState`、`ImageGenerationNetworkSmoke.{AbortReturnsPromptlyDuringGeneration,GeneratesOneImageFromRealEndpoint}`、`NativeNotifications.OptInDeliversSelfDrawnToast`、`RssSearchBackendLive.HostedEndpointReturnsSearchableResults`、`SessionReplayRealJsonl.UserSessionDbcoding5_2026_04_26`、`SystemPromptTest.PosixPromptStaysCleanOfWindowsGuidance`、`Utf8PathTest.ExtendedLengthPathIsIdentityOnPosix` |
| 失败清单 | 空 | 空(用户桌面版全程在运行,`DesktopSingleInstance.*` / `ComputerUsePointerOverlay.*` 这次都没有抖动) |

合入 master 之前对 D24 重放树也起跑过一轮全量,跑到一半因为要合入 P2-02 并重建而被我手动中止(只杀了本 worktree 的 `acecode_unit_tests.exe`),不计入结果;合入前的树上另跑过 3.1 节的子集(相同结果)。

## 4. 目标快照与用例清单对照(参照 `acecode-p2-shared/master-windows-{targets,gtest}.json`)

参照文件在 2026-09-27 21:2x 换成了 P2-02 合入后的 master(59 target / 3572 元组;5115 用例 / 9 SKIP / 0 失败),下面的对照都取自合入 `ec64edcb` 之后重建的 `build-p2`(第 2 节最后一行)。

```text
python scripts/refactor/cmake_target_snapshot.py --build-dir build-p2 --configuration Release --map scripts/refactor/src_layout_map.tsv --reverse-map --compare N:/Users/shao/acecode-p2-shared/master-windows-targets.json --output N:/Users/shao/acecode-p2-04-rec/targets-compare-merged.json
```

| 项目 | 参照(master `94938fa4` 之后,原样物理路径) | P2-04 `build-p2` |
|---|---|---|
| target 数 / 元组数 | 59 / 3572 | 59 / 3572 |
| target 增减 | — | 0 / 0 |
| 元组增减,`--reverse-map` 直接对照参照(工具原始输出) | — | 67 removed / 67 added,分三类:(a) 25 对 `nlohmann_json.natvis` 元组只差 vcpkg 路径前缀(参照在主检出采集,`vcpkg_installed` 在其源根下记成 `build/vcpkg_installed/...`;本 worktree 只读复用同一目录、位于源根之外,记成绝对路径;与 P1-01 记录的环境差异同一条);(b) 42 对是 **P2-02 搬走的文件**(`src/llm/{llm_provider.hpp,model_family.*,tool_icons.hpp,tool_protocol_names.*}`、`src/utils/{diff_utils,diff_view_truncate,word_diff}.*`、`tests/llm/…`、`tests/utils/diff_*` 及其在 5 个 target 里的 .obj):参照是**原样**物理路径,本侧 `--reverse-map` 把它们反查回 `src/tool/…`、`src/provider/llm_provider.hpp` 旧路径,于是两侧写法不同;(c) 本任务搬走的 17 个文件反查后与参照相同,**不在差异里** |
| 元组增减,两侧都换算回旧路径(参照用工具自己的 `translate_for_comparison(reverse=True)` 换算,42 条元组变动;本侧再归一 (a) 的前缀;`compare_snapshots.py --before 参照(反查) --after 本侧(反查+归一)`) | — | **`unexpected=false`,0 增 0 减,targets_added / targets_removed / unexpected_added / unexpected_removed 全空** —— 即「按映射换算回旧路径后每个文件所属的 target 不变」 |
| 元组增减,两侧都用原样物理路径(本侧不加 `--reverse-map`,只归一 (a)) | — | 22 removed / 22 added,**恰好就是本任务搬的 17 个文件 + `files_handler.cpp.obj` 在 5 个消费 target 里的对象**,一一对应、target 相同:`acecode_unit_tests` 的 5 个测试(`tests/{daemon,desktop,web}/…` → `tests/{ipc,workspace}/…`)、`acecode_native_bridge_support` 的 4 个 .cpp、`acecode_testable` 的 `files_handler.cpp` 与 7 个 .hpp、`acecode` / `acecode_unit_tests` / `concurrent_session_writer` / `remote_web_proxy_test_child` / `state_file_claim_worker` 的 `files_handler.cpp.obj`;没有其它差异(`N:\Users\shao\acecode-p2-04-rec\compare-targets-merged-raw.json`) |

搬走文件的归属抽样(反查后):`acecode_native_bridge_support` 仍含 `src/daemon/runtime_files.cpp`、`src/desktop/{agent_browser_runtime,open_request,workspace_registry}.cpp`;`acecode_testable` 仍含 `src/web/handlers/files_handler.cpp` 与 7 个搬走的 .hpp;`acecode` / `acecode_unit_tests` 等 5 个 target 仍消费 `@build/CMakeFiles/acecode_testable.dir/src/web/handlers/files_handler.cpp.obj`(对象路径由 `bf1ef7db` 同样反查)。`utils/paths.cpp` 只增内容不增文件,本任务没有新增文件。脚本:`N:\Users\shao\acecode-p2-04-rec\normalize_vcpkg_prefix.py`(只改那 25 个值)、参照反查的几行 Python 见同目录记录。

用例清单对照(`compare_snapshots.py --before 参照(反查) --after 本侧(反查+归一) --gtest-before master-windows-gtest.json --gtest-after gtest.json`,`N:\Users\shao\acecode-p2-04-rec\compare-final.json`):targets `unexpected=false`;gtest 段 `tests_before = tests_after = 5115`、`tests_added / tests_removed` 为空、`skipped_added / skipped_removed` 为空、`failures_before / failures_after` 为空、ctest 5119 → 5119。搬走的 5 个测试文件里的用例名不变(gtest 按 suite 名注册,与文件位置无关),所以清单不增不减。

## 5. lint 变化(同一台机器同一工具,报告模式)

| 项目 | 开工前(master `4b6ce35c`) | 合入 P2-02 前的 P2-04(`c2b217f9`) | 合入后的 master(`ec64edcb`,P2-02 记录) | 合入后的 P2-04(HEAD) |
|---|---|---|---|---|
| 分层违规总数 | 161(R8 119 / R1 18 / R9 10 / R2 6 / R3 5 / R5 1 / R10 1 / R14 1) | 161(逐规则相同,findings 集合相同) | 153(P2-02 消掉 8 条:R1 -5、R2 -2、R3 -1) | 153(R8 119 / R1 13 / R9 10 / R2 4 / R3 4 / R5 1 / R10 1 / R14 1;与 161 基线的差异 = P2-02 消掉的 8 条 + `hook_registry.cpp` 行号 -1) |
| include 边数(`include_edges`) | 2654 | 2654(20 个文件把 `config/config.hpp` 边换成 `utils/paths.hpp` 边,`config.hpp` 新增对 `utils/paths.hpp` 的边,`paths.cpp` 新增对 `constants.hpp` 的边;`expert_registry.cpp` / `hook_registry.cpp` / `paths_test.cpp` 原本已有 `utils/paths.hpp`,各少一条边,合计持平) | — | 2679 |
| `validate_map --strict` | exit 0 | exit 0(0 findings) | exit 0 | exit 0(0 findings;17 条 P2-04 行进入 `planned_or_obsolete_rows`) |
| `check_file_size --strict` / `check_ownership --strict` | exit 0 | exit 0(config.cpp 上限同步下调到 2857) | exit 0 | exit 0 |
| `normalize_includes --check` src / tests | exit 0 | exit 0 | exit 0 | exit 0(含冲突解决后的两个文件) |
| `check_doc_paths` findings | 85(工具原始计数;P1-01 / P2-02 记录的 84 是按 (file,line,path) 去重后的数,`docs/help/conversation.html:46` 同一路径报了两次) | **82**(去重 81):搬迁引入的 6 处(CLAUDE.md ×3、docs/agent-browser.md、docs/desktop-shell/multi-workspace.md、web/README.md)已改回;另修掉 `docs/desktop-shell/design.md` 里本就指错目录的 `src/desktop/runtime_files.{hpp,cpp}`、`tests/desktop/runtime_files_test.cpp` 三处。`openspec/**` 按工具约定不扫描 | 84(去重) | 82(去重 81,与合入前相同) |
| 工具自测 `unittest discover scripts/refactor/tests` | 71 项通过(`4b6ce35c`) | 72 项通过(`bf1ef7db` 新增 1 项,本任务未改工具) | 72 | 72 项通过 |
| `migrate_branch.py --check --layout current` | — | exit 1(P2 期间预期,记录即可):counts layers 2350 / migration_paths 60 / documentation 82 / map 0 / include_normalization 0 / ownership 0 / seed 0(`N:\Users\shao\acecode-p2-04-rec\migrate-check.json`,采集于 D24 重放之前的树;重放只删了转发头与工具改动,不影响这些计数) | — | 未重跑 |

**为什么总数没有下降。** `check_layers` 在过渡模式下先按 `src_layout_map.tsv` 把每个文件换算到最终路径再归模块:`src/desktop/workspace_registry.hpp` 等 7 个头在开工前就有精确映射行,已经被算作 `workspace` / `ipc` 模块(rank 6 / 5),所以 `agent_loop.cpp → desktop/workspace_registry.hpp` 这类边在基线里从来不是 R1 / R2 违规。基线的 18 条 R1 与 6 条 R2 全部属于 P2-02 / P2-05 / P2-06 / P2-07 的清单(config → permissions、session → session_registry、utils → config、hooks → skills、prompt → agent 等),没有一条涉及本任务的文件,因此本任务能做到的是「不上升」而不是「下降」。任务行「lint 显示指向 desktop、web 的反向边消失」按**物理位置**核对如下(`git grep` 两个提交的 include 行):

| 反向边(includer 模块 → 被包含头的物理目录) | 开工前 `4b6ce35c` | 之后 HEAD |
|---|---|---|
| agent(`src/agent_loop.cpp:5`)→ desktop/workspace_registry.hpp | 有 | `workspace/workspace_registry.hpp` |
| session(`global_session_catalog.cpp:3`、`global_session_search.cpp:4`)→ desktop/workspace_registry.hpp | 有 | workspace/ |
| tool(`workspace_tools.cpp:3`)→ desktop/workspace_registry.hpp | 有 | workspace/ |
| tool(`agent_browser/browser_tools.cpp:5`、`agent_browser/cdp_client.cpp:4`)→ desktop/agent_browser_runtime.hpp | 有 | ipc/ |
| environment(`data_dir_migration.hpp:27`)→ daemon/runtime_files.hpp | 有 | ipc/ |
| tui(`path_reference/path_reference.cpp:3`)→ web/handlers/files_handler.hpp | 有 | workspace/ |
| tui(`settings/settings_center.cpp:12`、`commands/desktop_command.hpp:3`)→ desktop/{workspace_registry,open_request}.hpp | 有 | workspace/、ipc/ |
| headless(`headless_runner.cpp:13`)→ desktop/workspace_registry.hpp(R5 平级) | 有 | workspace/ |
| web(`server_impl.hpp:16`)→ desktop/workspace_registry.hpp(R5 平级) | 有 | workspace/ |
| daemon(`worker.cpp:8/14`)→ desktop/{daemon_protocol,workspace_registry}.hpp | 有 | ipc/、workspace/ |
| cli(`main.cpp:189`)→ desktop/workspace_registry.hpp | 有 | workspace/ |
| desktop(`daemon_pool.cpp:3/5`、`main.cpp:51`)→ daemon/{guid,runtime_files}.hpp | 有 | ipc/ |
| tests 12 处 → desktop / daemon / web 旧路径 | 有 | ipc/、workspace/ |

HEAD 上 `git grep` 旧物理路径(`desktop/workspace_registry.hpp`、`daemon/runtime_files.hpp`、`web/handlers/files_handler.hpp` 等)在 src / tests 里 0 命中。`src/ipc/runtime_files.cpp → daemon/platform.hpp` 是本任务留下的唯一一条指向 daemon 物理目录的边(见 `b2439902`)。

## 6. `get_*_dir` 拆分的判定方法(`82cae6a8`)

扫描 `config.hpp` 及其 4 个直接包含头(`desktop_close_behavior.hpp`、`saved_models.hpp`、`computer_use/pointer_appearance.hpp`、`utils/constants.hpp`)声明的全部标识符,对每个 include 了 `config/config.hpp` 的 src / tests 文件检查正文里用到哪些:除三个函数外一个都没用到的共 20 个(layout-map 估计 19 个,多出的是 `runtime_files.cpp`,它另外只用 `utils/constants.hpp` 且已直接 include):`daemon/startup_diagnostics`、`memory/memory_paths`、`channels/runtime`、`desktop/edge_app_launcher`、`desktop/single_instance_posix`、`experts/expert_registry`、`hooks/hook_config`、`hooks/hook_registry`、`loop/loop_store`、`provider/auth/github_auth`、`provider/auth/xai_auth`、`session/session_storage`、`session/thread_service`、`tool/theme_create_tool`、`upgrade/apply`、`upgrade/diagnostics`、`ipc/agent_browser_runtime`、`ipc/open_request`、`ipc/runtime_files`、`tests/utils/paths_test`(后者与 `expert_registry` / `hook_registry` 原本已有 `utils/paths.hpp`,只删 config 行)。`daemon/cli.cpp`、`daemon/service_win.cpp`、`skills/skill_init.cpp`、`hooks/hook_payload.cpp` 另外用了 `AppConfig` / `load_config` / `ModelProfile`,保留 config include。签名、namespace(`acecode`)、`get_run_dir` 的 override 语义、`resolve_data_dir` 的 redirect 缓存语义都不动,运行期行为零变化。

## 7. 触碰到的不变量(§7.3)

- §7.3.8 路径:`get_*_dir` 三个函数只是逐字节搬家;`tests/utils/paths_test.cpp` 与 daemon / desktop / ipc / workspace 相关用例随第 3 节运行。
- §7.3.9 构建边界:`acecode-desktop` 仍只链 `acecode_desktop_support` + `acecode_native_bridge_support`;搬走的四个 .cpp 仍在 `ACECODE_NATIVE_BRIDGE_SUPPORT_SOURCES`,第 4 节的目标快照证明每个文件所属 target 不变;EXCLUDE_FROM_ALL 冒烟目标都能构建(第 2 节)。
- 其余不变量(prompt cache 前缀、provider 历史出口、审批门、单写者、锁序、PA)本任务不触碰,守护测试随全量单测运行。

## 8. 遗留与需主代理决定的事项

1. **lint 总数只做到「不上升」(161 → 161)**,原因见第 5 节:任务点名的反向边在过渡模式下本来就按映射归到 ipc / workspace,基线里没有本任务能消掉的 finding。tasks.md 3.3「lint 显示指向 desktop、web 的反向边消失」以第 5 节的物理边对照表为准,请主代理确认这样验收。
2. **`src/ipc/runtime_files.cpp → daemon/platform.hpp`**(`b2439902`)是与 P2-03 的交叉点:P2-03 把 `daemon/platform*` 搬到 `platform/process/os_process*` 后,合入方重跑 `normalize_includes.py --scope src` 即可自动改到新路径(映射表已有该行);两边合入顺序不限。
3. **`CMakeLists.txt` 与 P2-03 相邻行冲突**:本任务只改 `ACECODE_NATIVE_BRIDGE_SUPPORT_SOURCES` 的 4 行(原 228 / 233 / 239 / 243 行)与 396 行的 ws2_32 注释,P2-03 改同一清单里紧邻的 `daemon/platform_*`、`desktop/custom_toast*`、`desktop/locale`、`utils/clipboard` 等行,git 会把相邻改动报成冲突,逐行保留双方即可(没有语义冲突)。
4. **Linux / macOS 未在本机构建**:`82cae6a8` 的非 Windows 分支只做了逐段人工核对(第 1 节),`agent_browser_host_mac.mm` 的 include 改写由 normalize 完成、未编译;请合入前跑 `refactor-matrix`。
5. **desktop 冒烟改为替代验证**(第 3.1 节):用户桌面版正在运行 + computer-use 屏幕访问被拒,没有做「启动 build-p2 的 acecode-desktop.exe 并截图」;若需要真机截图,请主代理在桌面版关闭后用 `build-p2\acecode-desktop.exe`(daemon 在同目录)补做。
6. **开工时按简报旧口径做过、D24 后丢弃的三件事**(备份分支 `backup/P2-04-pre-d24`,仅本机,未推送):7 个转发头 + 7 条 exception 行;`validate_map.py` 识别转发头的修正(否则转发头会被报成「destination collision」,与 D24 的判断一致);`test_migrate_branch` 终态夹具剥掉 exception 行。D24 之后三者都不再需要,分支里没有任何工具改动。
7. `check_doc_paths` 的「不多于 84」以工具原始计数为准是 85 → 82(第 5 节有口径说明);`docs/desktop-shell/design.md` 的三处修正超出「只改搬走文件的路径字面量」一点点(它们本就指错目录,但指向的正是本任务搬走的文件),若不希望在本任务里改,回退 `c2b217f9` 里该文件的两行即可,findings 会回到 85。
8. `layout-map.md` 的 P2-04 行「19 个文件」实测为 20 个(多出 `runtime_files.cpp`),表内数字未改;按顶部规则由主代理决定是否回写。
9. **参照快照的口径**:主代理 21:2x 换上的 `master-windows-targets.json` 是 P2-02 之后 master 的**原样**物理路径快照(含 `src/llm/…`),不是 `--reverse-map` 之后的;直接拿本侧 `--reverse-map` 的输出去比,P2-02 搬走的 42 条元组会被报成假差异(第 4 节 (b))。本记录改为两侧都换算回旧路径再比(用工具自己的 `translate_for_comparison(reverse=True)`),并附一份原样路径对照证明差异只剩本任务的 17 个文件。后续 P2 任务对照时建议参照也用 `--reverse-map` 采集,或统一用原样路径 + 只允许本任务搬走文件的一一对应差异。
10. 主检出 `N:/Users/shao/acecode` reflog 里 19:15 的 checkout / merge 不是本会话做的:本会话所有 git / cmake / python 命令都带 `cd /n/Users/shao/acecode-p2-04 &&` 或 `git -C N:/Users/shao/acecode-p2-04`,构建脚本内 `cd /d N:\Users\shao\acecode-p2-04`;本会话在 17:41(首轮构建结束)到 21:13(用量限制重置)之间没有执行过任何命令。

## 9. Codex 接手复核(2026-09-28,合入 P2-03 前)

已合入最新 master `0c0e37cb`,当前源码提交 `f3483845d986230f848dbe75ad5c623b5e769ff4`。[refactor-matrix 36332588494](https://github.com/tmoonlight/acecode/actions/runs/36332588494) 的四平台全新构建均通过:

| 平台 | target / 编译元组 | 列出 / 执行 / SKIP / 失败 |
|---|---|---|
| Windows x64 | 59 / 3572 | 5115 / 5108 / 10 / 3 |
| Linux x64 | 50 / 3486 | 5035 / 5026 / 16 / 0 |
| macOS arm64 | 57 / 3631 | 5040 / 5033 / 14 / 11 |
| Deepin x64 | 13 / 1372 | 不启用测试 |

相对 post-p0,目标增减和既有元组删除均为 0,新增元组只有前序 P2-02 的 43 / 19 条。本任务全部搬迁文件保持原目标与编译属性。产物与对照 JSON 在仓库外 `N:/Users/shao/AppData/Local/Temp/codex-refactor20260928/ci/p2-04/` 及 `ci-audit-p2-04-*.json`。

原始 CTest 参数化名称含运行时指针 / 对象字节,不作逐字节等值声明;本次核对 GoogleTest suite / case 集合、CTest 注册数量和 SKIP 用例集合,均无增删。快照比较在两侧统一使用 `translate_for_comparison(reverse=True)`,所有既有目标依赖和编译属性保持。

Deepin artifact 的 `source_revision` 因容器 Git 的 `dubious ownership` 报错为空。已从同一 build job 的 checkout 日志核对完整提交号,与传入的不可变 `source_ref` 一致;没有把空字段当作来源验证通过。Deepin 按原矩阵约定只构建 CLI / Desktop,不运行单测。

矩阵是基线采集工作流,其成功状态不代表所有断言通过。Windows 的失败未超出 post-p0 基线;macOS 的 10 项既有失败仍在,另有 `OpenAiProviderErrorRecovery.SseKeepaliveCommentsDoNotTriggerRetry`,这项已在 P1 验收及 P2-02 记录为既有时序抖动。本次保留其失败,没有删除用例、增加 SKIP 或修改断言。Linux 完整测试为 0 失败。

### 9.1 实际 Desktop 工作区与文件预览验证

补齐第 3.1 节当时未执行的真机链路。先在本工作区运行 `pnpm install --frozen-lockfile` / `pnpm build`,再用全新、可被共享启动器识别的 `build/windows-x64-desktop-release` 配置并构建 CLI / Desktop。MSVC x64 + Ninja Release,只读复用主检出的 vcpkg 依赖,519 步构建成功。通过 `python scripts/dev_environment.py desktop --yes` 启动,启动器确认复用本工作区产物并完成增量检查。

HOME / USERPROFILE / APPDATA / LOCALAPPDATA / TEMP / TMP 均为仓库外 `N:/ac-p204-0928/` 的独立目录,WebView2 profile 也独立。通过该实例的 WebView2 CDP 操作 DOM,未使用屏幕截图:

1. Desktop 窗口启动,页面标识 `__ACECODE_DESKTOP_SHELL__` 为 true,原生 bridge 可调用。
2. 在隔离 daemon 中注册 `N:/ac-p204-0928/workspace-one`,HTTP 201;原生 `aceDesktop_listWorkspaces` 能读到该工作区。
3. `aceDesktop_activateWorkspace` 成功,返回相同 cwd / hash,daemon 保持 running。
4. 在实际页面选择该工作区,展开文件面板,点击 `p2-04-smoke.txt`,文件预览中显示 `P2-04 workspace file handler smoke`。同时原生工作区状态为 active / available / running。
5. 通过 `aceDesktop_quitApp` 正常退出,本次 Desktop PID 48772 和 daemon PID 40576 均已结束;原有用户 Desktop PID 13680 仍在运行。

检查摘要为仓库外 `p2-04-desktop-smoke.json`;构建与启动日志为 `p2-04-native-build.log` / `p2-04-desktop-launch.log`。测试文件仅写入隔离工作区,没有修改项目源码。

P2-03 合入后仍须同步最新 master,解决 CMake 相邻改动并复验;本节不提前勾选 P2-04。

## 10. 同步 P2-03 后的集成复验(2026-09-28)

P2-03 已由 PR #79 合入 master(`b7be0dcb`)。本分支的合并提交为 `aa6872f961b6f4e73210bac0e5de1cb8db9594c9`。14 个文件的冲突均为相邻 include / CMake 路径或文档路径,保留 P2-04 的 ipc / workspace 路径及 utils/paths 分离,同时采用 P2-03 的 platform / pty 路径。P2-03 完成标记已在 tasks.md 更新。

- src / tests include 规范化、映射、分层阻断项、行数、所有权检查和 `git diff --cached --check` 全部通过;OpenSpec strict 校验及前端完整 `pnpm test` 通过。
- 全新目录 `build/p2-04-integrated-aa6872f9` 配置并构建 CLI / Desktop / GoogleTest / 五个冒烟目标,1046 步完成,退出码 0。
- File API 目标对照:59 target / 3573 元组。相对 P2-02 后的同机 master 快照,目标差异 0、既有元组删除 0,唯一新增为 P2-03 的 `lsp_platform_aliases.hpp`;P2-04 搬迁文件的归属和编译属性全部保持。
- 完整本机 GoogleTest 在独立短路径 `N:/a04i/` 完成:5115 项列出、5114 项执行、9 项跳过、0 失败、退出码 0;结果见仓库外 `p2-04-integrated-gtest.json`。
- 用官方开发启动器启动本次集成目录的 Desktop,再次验证原生桥激活工作区与 UI 文件预览。Desktop PID 56752 / daemon PID 47836 均为该目录产物,正常退出后两个进程均结束,用户原 Desktop PID 13680 保持运行;无屏幕截图。证据为仓库外 `p2-04-integrated-desktop-smoke.json`。
- [集成版本四平台 CI 36336644249](https://github.com/tmoonlight/acecode/actions/runs/36336644249) 已按完整源码 SHA 调度,结果尚待完成,本任务仍不勾选验收。
