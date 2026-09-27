# P2-03 平台件下沉到 `platform/` 与 `pty/` 验证记录

认领 Claude-p2-03,2026-09-27(`fdb91d1f`)。分支 `refactor20260927/P2-03`,基于 master `4b6ce35c`(含主代理为 P2 补的 normalize_includes 过渡目录解析),中途按主代理要求合入一次 master(`bf1ef7db`,快照工具的对象路径换算)。搬迁清单 = `scripts/refactor/src_layout_map.tsv` 中 phase 为 P2-03 的 61 行(45 个 src 文件 + `src/web/pty/` 整目录 9 个文件 + 15 条 tests 行 17 个文件),与 layout-map.md §2 / tasks.md 3.2 一致。**按主代理 2026-09-27 的决策 D24,旧路径不留转发头、`layers.tsv` 不加 exception 行**;中途加过的 20 个转发头与 20 条 exception 已在 `c69f91c6` 删除,最终树里 `scripts/` 与 `src/layers.tsv` 与 master 逐字节相同。

| 提交 | 内容 |
|---|---|
| `8c8538b0` [mechanical] | 71 个文件 `git mv`,全部 R100,0 行内容改动:`utils/{clipboard,open_url,power_inhibitor}.*` → `src/platform/`;`utils/terminal_*` 与 `upgrade/console.hpp` → `src/platform/terminal/`;`hooks/hook_runner.*` → `platform/process/process_runner.*`;`lsp/lsp_process.*` → `platform/process/piped_process.*`;`lsp/lsp_which.*` → `platform/process/which.*`;`daemon/platform{.hpp,_posix.cpp,_windows.cpp}` → `platform/process/os_process*`;`desktop/locale.*` → `src/platform/`;desktop 的 folder_picker / context_picker / open_in_explorer / notifications* / custom_toast* / strings → `platform/native_ui/`;`src/web/pty/` → `src/pty/`;14 个测试 → `tests/platform/`,`tests/web/pty/` → `tests/pty/` |
| `bb94c967` [mechanical] | `normalize_includes.py --scope src`(50 文件 76 行)与 `--scope tests`(30 文件 31 行)的输出,只改引号内路径;`--check` 两个范围 exit 0;numstat 107/107,`-U0` 补丁里 +/- 行全部是 `#include` 行 |
| `de834f51` | 内容修改:`platform::ProcessSpec`(hooks 留 `using HookCommandSpec`)、`platform::PipedProcess` / `SpawnOptions` / `which`(lsp 留别名头 `lsp/lsp_platform_aliases.hpp`,channels / environment 直接改用 platform 名)、搬走文件里 14 行指回旧目录的裸名 include、CMake 清单、文档路径;当时还加了 20 个转发头与 exception 行(已被 `c69f91c6` 撤掉)。细节见第 2 节 |
| `a92c0e4e` | 合入 origin/master(`bf1ef7db`) |
| `be02de47` | 为转发头做的两处工具修正(validate_map 识别转发头、migrate_branch 用例夹具),D24 之后无需保留,`c69f91c6` 已整体回退 |
| `c69f91c6` | D24:删除 20 个转发头与 20 条 exception 行,回退 `be02de47`;`scripts/` 与 `src/layers.tsv` 恢复与 master 相同 |

净 diff(`origin/master..HEAD`):160 个文件;37 个纯 R100 + 34 个 R070–R099(搬走后又改了 include / 命名空间 / 注释的文件,搬迁本身在 `8c8538b0` 里是 R100)+ 88 个 M + 1 个 A(`src/lsp/lsp_platform_aliases.hpp`)。

## 1. §7.2「P2(每个 PR)」闸门逐项

| 闸门 | 结论 | 证据 |
|---|---|---|
| lint 违规数下降 | 满足(161 → 160) | `check_layers.py --enforce-parent-includes`:R1 18 → 17,其余规则计数不变(R8 119 / R9 10 / R2 6 / R3 5 / R5 1 / R10 1 / R14 1);消失的边是 `src/hooks/hook_runner.hpp:3 → hook_config.hpp`(base → domain),由 `ProcessSpec` 下沉切断;无新增 finding;`exceptions_used` 0。只降 1 的原因见第 4 节 |
| 转发头已登记 | D24 改为不留 | 搬走的 .hpp 旧路径不留任何文件,`layers.tsv` 无 exception 行;全仓 grep 旧路径 include(`utils/clipboard.hpp`、`hooks/hook_runner.hpp`、`lsp/lsp_process.hpp`、`daemon/platform.hpp`、`desktop/strings.hpp`、`web/pty/…` 等 24 个头)0 处 |
| 按映射换算后每个文件所属的 target 不变 | 满足 | 第 3 节:`--reverse-map --compare` 对参照快照 59 target / 3529 元组,target 0 增 0 减,元组 0 减,唯一新增是 `acecode_testable` 里的新头 `src/lsp/lsp_platform_aliases.hpp`(File API 把 GLOB 到的头也列进 target 源清单);没有任何编译单元换 target |
| 测试已随源文件移动 | 满足 | 17 个测试文件按 map 的 15 条 tests 行 R100 移动(`tests/platform/` 14 个、`tests/pty/` 3 个);GLOB `*_test.cpp` 自动收进 `acecode_unit_tests`,用例清单不变(第 3 节) |
| 三平台构建通过 | Windows 本机满足;mac / Linux 待 CI | 全新目录 `build-p2`(第 3 节)。改动的 `.mm` 只是路径(`acecode_source_paths.cmake` 的 `ACECODE_NATIVE_BRIDGE_MAC_SOURCES`),`acecode_require_sources` 在 Windows configure 时也校验这两个路径存在;`os_process_posix.cpp` / `terminal_theme_detect_posix.cpp` / `pty_backend_posix.cpp` 只改了 include 行。建议主代理合入后 dispatch 一次 refactor-matrix |
| 用例清单不变 | 满足 | 第 3.1 节:`tests` 5115、`ctest_names` 5119、SKIP 9 条与参照逐条相同,0 增 0 减 |
| `migrate_branch.py --check --layout current` | 按约定记录,非零 | `success=false`(P2 未完成),输出存 `N:/Users/shao/acecode-p2-03-verify/migrate-check.json` |

## 2. 内容修改说明

### 2.1 process_runner(原 hooks/hook_runner)

- `HookCommandSpec` 的本体原来定义在 `hooks/hook_config.hpp`,`hook_runner.hpp` 为它 include 整个 hook_config —— 这正是基线里那条 `base -> domain` 的 R1。现在 `platform/process/process_runner.hpp` 自己定义 `acecode::platform::ProcessSpec`(command + args + `valid()`,聚合类型,`{cmd, {args}}` 初始化照旧),`hooks/hook_config.hpp` 改为 `using HookCommandSpec = platform::ProcessSpec;`。hooks 内部(hook_config / hook_manager / hook_registry / hook_runtime)与 `tests/hooks/*` 沿用旧名,不改。
- hooks 之外原本只经 `hook_runner.hpp` 拿到 `HookCommandSpec` 的调用点改用 `platform::ProcessSpec`(它们不再间接 include hook_config):`tool/grep_tool.cpp`(4)、`worktree/worktree_manager.cpp`、`environment/terminal_resolver.cpp`、`channels/setup.cpp`、`daemon/worker.cpp`、`remote_control/channel_plugin.{hpp,cpp}`(Runner 签名 + 3 处),以及 `tests/remote_control/channel_plugin_test.cpp`(12)、`tests/commands/remote_control_command_test.cpp`、`tests/remote_control/session_channel_binder_test.cpp`、`tests/platform/hook_runner_test.cpp`(6)。只换类型名,不改任何断言。
- `HookProcessResult` / `HookProcessOptions` / `HookEnvironment` / `run_hook_process` / `run_hook_shell_command` / `resolve_hook_command_path` 名字与 `namespace acecode` 暂不动(简报只要求 ProcessSpec),头注释说明;调用点 60+,留作后续。

### 2.2 piped_process / which(原 lsp/lsp_process、lsp/lsp_which)

- `LspProcess` → `platform::PipedProcess`,`LspSpawnOptions` → `platform::SpawnOptions`,`quote_windows_arg` / `FileExistsFn` / `which_in` / `which` 只换命名空间 `acecode::lsp` → `acecode::platform`。
- 新头 `src/lsp/lsp_platform_aliases.hpp`(真实文件,随 lsp 模块走)保留 `lsp::LspProcess` / `LspSpawnOptions` / `FileExistsFn` / `quote_windows_arg` / `which` / `which_in` 别名;`lsp_client.hpp` 与 `lsp_server_registry.hpp` 改 include 它,lsp 内部调用点(lsp_client / lsp_server_registry / lsp_service、`tests/lsp/lsp_server_registry_test.cpp`)一处不改。
- lsp 之外按 layout-map §6「P2-03 起 bridge/setup 改用 platform/process」直接改用 platform 名:`channels/bridge.{hpp,cpp}`、`channels/runtime.hpp`、`channels/setup.{hpp,cpp}`、`environment/toolchains.{hpp,cpp}`、`tests/test_support/channels/test_support.hpp`、`tests/platform/lsp_which_test.cpp`(using 声明与头注释)。grep 确认 lsp 之外不再有 `lsp::LspProcess` / `lsp::LspSpawnOptions` / `lsp::which`。

### 2.3 只搬位置的部分

- `os_process*`(原 daemon/platform*):函数与 `namespace acecode::daemon` 不变(简报允许),头注释写明约 20 个调用点(daemon / desktop / session / web / channels / tests)逐步改名。P2-04 正在搬 `daemon/runtime_files.cpp`(它调 `daemon::current_pid`),现在改命名空间会让两个分支在同一批调用点上撞车。
- `platform/terminal/console.hpp`(原 upgrade/console.hpp):`namespace acecode::upgrade` 不变,只有 `upgrade/apply.cpp`、`upgrade/upgrade.cpp` 两个使用方。
- desktop 的 native_ui 件与 locale:命名空间 `acecode::desktop` 不变。
- 搬走文件里指回旧目录的裸名 include 工具无法解析(简报里「也会被改成模块根形式」这一句实测不成立,P2-02 的工具修正只解决「指向搬走文件」的方向),14 行手工改:`platform/clipboard.cpp` 的 `base64.hpp` / `encoding.hpp` → `utils/...`,`power_inhibitor.cpp`、`terminal_capability.cpp`、`terminal_theme_detect{,_posix,_win}.cpp` 的 `logger.hpp` → `utils/logger.hpp`,`native_ui/strings.cpp` 的 `locale.hpp` → `platform/locale.hpp`,`os_process_{posix,windows}.cpp` 的 `platform.hpp` → `os_process.hpp`,`process_runner.cpp` / `piped_process.cpp` / `which.cpp` 的自包含头名,`process_runner.hpp` 删掉 `hook_config.hpp`。改完 `check_layers` 的 14 项「unresolved project include」全部消失。
- CMake:`CMakeLists.txt` 的 `ACECODE_NATIVE_BRIDGE_SUPPORT_SOURCES` 11 行(os_process_posix / os_process_windows / custom_toast / custom_toast_win / context_picker / folder_picker_win / locale / notifications / open_in_explorer / strings / clipboard)、`ACECODE_NOTIFICATION_BACKEND_SOURCES` 2 行、`target_sources(acecode_native_bridge_support …)` 的 notifications_win / notifications_stub 两处、两条注释;`cmake/acecode_source_paths.cmake` 的 `ACECODE_WINPTY_AGENT_LOCATION_SOURCE`(即任务点名的 `acecode_winpty.cmake:64` 那条引用,P0-04 已收成共用变量)与 `ACECODE_NATIVE_BRIDGE_MAC_SOURCES` 两个 `.mm`;`cmake/acecode_winpty.cmake` 一条注释。P2-04 的 runtime_files / agent_browser_runtime / open_request / workspace_registry 四行没碰(与它们交错,合入时会有文本冲突,按「合入串行」处理)。`cmake/deepin/CMakeLists.txt` 与 `tests/CMakeLists.txt` 没有引用搬走的文件。configure 时 `acecode_require_sources` 护栏全部通过。
- 文档与路径表:CLAUDE.md(Desktop Shell 关键模块列表、Mouse+Clipboard 的 `src/platform/clipboard.*`、ConsoleDock 的 `src/pty/`、Windows 弹框三层结构的归属、LSP 结构里 `platform/process/piped_process` / `which`)、AGENT.md、`tests/README.md` 镜像表、`docs/localization.md`、`docs/desktop-shell/multi-workspace.md`、`docs/plan/reviews/theme-management-001-web.md`、`docs/string-utilities-refactor.md`、`docs/help-source/group6.py`(改后跑 `build_help.py`,生成物只有 `sources.json` 一行 diff,其余 html 无变化)、`tests/cpp_source_paths.json`(`desktop/context_picker.cpp` 键的值改为新路径,键名不动;`node web/src/lib/previewWorkbenchArchitecture.test.js` 全部 pass);搬走的测试与 `open_url.hpp` 的头注释里的旧路径同步。`check_doc_paths` 85 → 85(开工前本树实测 85,不是简报里的 84;差异来自 P2-02 合入 master 之后,不是本任务引入),无新增失效路径。注意「原 `src/web/pty/`」这种带 `src/` 的旧路径写进文档会被 checker 算失效,改成不带前缀的写法。

### 2.4 转发头的实测结论(供 D24 备案)

D24 之前按简报留过 20 + 4 个 2 行转发头,两条实测结论值得留档:

- `src/web/pty/` 的 4 个转发头会让构建失败:新模块名 `pty` 与旧子目录同名,`src/web/server_impl.hpp:68` 的规范写法 `#include "pty/pty_session_registry.hpp"` 被 MSVC 先按包含者目录命中转发头,转发头里同名的 include 又被按**父包含者**目录解析回它自己,`#pragma once` 之后什么也不包含 —— `routes_environment.cpp` 实测 `C2027 未定义类型 acecode::PtySessionRegistry`;lint 同时报 R8「ambiguous include」。这就是 design.md D1 说的「MSVC 按父包含者目录查找造成的平台差异」。
- 其它 20 个转发头能编译,但 `validate_map --strict` 报 20 项 destination collision(与 D24 的理由一致);lint 把转发头按精确行映射到目的模块,转发头指向真身是模块内边,不产生 R1/R2,exception 行只是登记簿。

现在两类都不存在,问题随之消失。

## 3. 本机验证(Windows 11,全新目录 `build-p2`)

Ninja + MSVC 2022 Release,`BUILD_TESTING=ON`,`ACECODE_BUILD_DESKTOP=ON`,x64-windows-static,只读复用 `N:/Users/shao/acecode/build/vcpkg_installed`;configure 前写 File API query。删转发头之后重新 configure + build 一次(GLOB 重扫),快照取自最终树。

| 步骤 | 结果 |
|---|---|
| configure | 通过(`acecode_assert_known_roots` / `acecode_require_sources` 全部通过) |
| `cmake --build --target acecode acecode-desktop acecode_unit_tests` | 通过(1020 步;2 条告警都是既有的:`xutility` C4244、`tests/sandbox/exec_rules_test.cpp:153` C4129)。第一次带 pty 转发头的构建失败于 `routes_environment.cpp` C2027(2.4) |
| 冒烟目标 `computer_use_native_smoke computer_use_broker_smoke agent_browser_host_smoke agent_browser_pointer_demo acecode_upgrade_restart_smoke` | 通过(`smoke build exit=0`) |
| desktop 不链 acecode_testable | `build-p2/build.ninja` 里 `build acecode-desktop.exe:` 的 LINK_LIBRARIES 只有 `acecode_desktop_support.lib acecode_native_bridge_support.lib` + vcpkg 的 cpr / libcurl / zlib + 系统库,`acecode_testable` 出现 0 次 |
| 目标快照 vs `N:/Users/shao/acecode-p2-shared/master-windows-targets.json`(`--map … --reverse-map --compare`,master `bf1ef7db` 的工具) | 59 target / 3530 元组 vs 参照 59 / 3529:target 0 增 0 减;元组 25 减 26 增。25 减 + 25 增全部是 `nlohmann_json.natvis` 的环境差异(参照来自主检出,vcpkg_installed 在其源根之下记成 `build/vcpkg_installed/...`,worktree 里是源根之外的绝对路径 `N:/Users/shao/acecode/build/...`;把这一个前缀归一后 25 对逐条相同,与 P1-01 记录同类);natvis 之外 **removed 0,added 1** = `acecode_testable` 的新头 `src/lsp/lsp_platform_aliases.hpp`。14 个搬走的 `.cpp` 在 acecode / acecode_unit_tests / concurrent_session_writer / remote_web_proxy_test_child / state_file_claim_worker 五个消费目标里的对象路径经 `bf1ef7db` 的换算逐条对上。`compare_snapshots.py --allowed-addition src/lsp/lsp_platform_aliases.hpp`:targets 0 增 0 减,`authorized_added` = 该头 1 条,`unexpected_removed` / `unexpected_added` 各 25 条全是 natvis(报告存 `N:/Users/shao/acecode-p2-03-verify/compare-snapshots.json`) |
| 全量单测(隔离 HOME / TEMP 于 `N:\acecode-p2-03-iso`) | **5115 列出 / 5114 执行 / 9 SKIP / 0 失败**,`ctest_names` 5119(`gtest_inventory.py --run`,exit 0);与简报给的参照(5115 / 5114 / 9 / 0)相同,`ComputerUsePointerOverlay.*` / `DesktopSingleInstance.*` 本次都没抖 |
| 用例清单(`compare_snapshots.py --gtest-before master-windows-gtest.json --gtest-after gtest.json`) | `tests_added` 0 / `tests_removed` 0;`ctest` 5119 = 5119;SKIP 集合 9 条逐条相同(ChannelBridge.RealBridge… / ChannelSetup.RealDependency… / ImageGenerationNetworkSmoke ×2 / NativeNotifications.OptInDeliversSelfDrawnToast / RssSearchBackendLive… / SessionReplayRealJsonl.UserSessionDbcoding5_2026_04_26 / SystemPromptTest.PosixPromptStaysCleanOfWindowsGuidance / Utf8PathTest.ExtendedLengthPathIsIdentityOnPosix);失败集合两边都为空 |
| `tests/pty/` 的三种后端 | `PtyBackendSpawnTest.ConPtyEchoRoundTripAndExit` OK(268 ms,未触发其 GTEST_SKIP)、`PipeFallbackEchoRoundTripAndExit` OK、`KillRunningSessionIsIdempotent` OK、`WinptySpikeTest.SpawnCmdEchoRoundTripAndResize` OK(真实 spawn cmd + 回显 + resize,139 ms)、`MissingAgentOverrideFailsCleanly` OK、`PtySessionRegistryTest.*` 10 条 OK、`PtyBackendDetectTest.ModernWindowsPrefersConPty` OK(摘自 `gtest-run.log`) |
| Web 控制台停靠区冒烟 | `build-p2/acecode.exe daemon --foreground --port=54676 --run-dir=N:\acecode-p2-03-iso\run`(隔离 HOME,无模型配置),日志 `console backend=conpty shell="C:\Program Files\PowerShell\7\pwsh.exe"`;内置浏览器打开 `http://127.0.0.1:54676/`,点顶栏「打开控制台 (Ctrl+`)」,停靠区开出 pwsh 标签(日志 `[pty] created session pty-1 … backend=conpty`),输入 `echo p2-03-conpty-smoke` 回显 `p2-03-conpty-smoke` 并出新提示符。winpty:本机 Win11 `detect_pty_backend()` 恒选 ConPTY,没有运行期开关,winpty 路径以上面两条 `WinptySpikeTest` 覆盖 |

### 3.1 搬走模块相关用例

按用例名前缀在清单里核对,117 条全部执行、0 失败、无 SKIP:TerminalCapability 14、ClipboardTest 11、NativeNotifications 11(其中 `OptInDeliversSelfDrawnToast` 是既有的环境 SKIP)、HookRunner 10、PtySessionRegistryTest 10、ConsoleShellCatalogTest 9、LspWhich 6、OpenUrl 6、CustomToast* 23、DesktopLocale 4、DesktopStrings 4、PtyBackendSpawnTest 3、PtyShellResolveTest 2、WinptySpikeTest 2、PtyBackendDetectTest 1、PtyBackendKindNameTest 1。

## 4. lint 与工具

| 项目 | 开工前(本树 `4b6ce35c`) | 完成后 |
|---|---|---|
| 分层违规总数(`check_layers.py --enforce-parent-includes`) | 161 | **160**(R1 18 → 17,其它不变;`include_edges` 2654 → 2655,多的 1 条是 `lsp_platform_aliases.hpp`) |
| `normalize_includes --check` src / tests | exit 0 | exit 0 |
| `check_doc_paths` findings | 85 | 85 |
| `check_file_size --strict` / `check_ownership --strict` | exit 0 | exit 0(两个基线都按最终规范路径记账,搬迁后 `custom_toast_win.cpp` 1230 行等条目照常命中) |
| `validate_map --strict` | exit 0 | exit 0(findings 0;P2-03 的 61 行不再出现在 `planned_or_obsolete_rows`) |
| `unittest discover scripts/refactor/tests` | 71 通过 | 72 通过(+ master `bf1ef7db` 的对象路径用例;本分支对 `scripts/` 无改动) |

新模块的出边(按 `IncludeIndex` 实测):`platform → platform` 19 个头、`platform → utils` 4 个头(`base64` / `encoding` / `logger` / `utf8_path`)、`pty → pty` 4 个头、`pty → utils` 4 个头(`encoding` / `logger` / `paths` / `utf8_path`),没有任何指向 platform / pty 之外更高层的边。任务行点名的「utils → desktop / utils → daemon 反向边」在基线里本来就被映射表提前归入 platform 而不计违规(lint 按映射表给文件定模块),所以报告里看不到它们「消失」;真正消失的是 hooks 的 `hook_runner.hpp → hook_config.hpp`。

## 5. 触碰到的不变量(§7.3)

- 9「构建边界」:acecode-desktop 不链 acecode_testable(第 3 节 link 行);`ACECODE_DEEPIN` / `ACECODE_CHANNEL_ASSET_DIR` 所在文件没动;`.mm` 的 OBJCXX 属性与 REMOVE_ITEM 只改路径;每个文件所属 target 不变(快照);冒烟目标可构建。
- 其它条目(prompt cache、provider 出口、工具名映射、审批门、单写者、锁序、PA、路径)本任务不触碰对应代码;守护测试随全量单测运行(0 失败)。

## 6. 遗留与需要主代理决定的事项

1. `os_process.hpp` 仍是 `namespace acecode::daemon`,`console.hpp` 仍是 `acecode::upgrade`,`run_hook_*` 函数名未改 —— 简报允许,建议在 P2-04 合入后再统一改名,避免与它搬的 `runtime_files.cpp` 撞车。
2. 目标快照对照里唯一的「增」是新头 `src/lsp/lsp_platform_aliases.hpp`(`acecode_testable`,File API 把 GLOB 到的头也列进源清单);`compare_snapshots.py` 用 `--allowed-addition src/lsp/lsp_platform_aliases.hpp` 即可归为授权新增,是否登记进 D23 名单由主代理定。
3. `CMakeLists.txt` 的 `ACECODE_NATIVE_BRIDGE_SUPPORT_SOURCES` 与 P2-04 的四行交错,合入第二个分支时会有文本冲突,按行合并即可(两边都只改自己的行)。
4. mac / Linux 构建未在本机验证(只有路径改动 + 两个 `.mm` 路径经 `acecode_require_sources` 校验),建议合入后 dispatch refactor-matrix。
5. 简报「搬走文件内部指回旧目录的裸名 include 也会被 normalize 改成模块根形式」实测不成立(2.3),后续 P2 任务要预留手工改这类 include 的步骤,或者给 `IncludeIndex.resolve` 补上「按映射表反查包含者的旧目录」这一步。
