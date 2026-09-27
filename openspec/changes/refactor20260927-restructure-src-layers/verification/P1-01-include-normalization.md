# P1-01 include 规范化验证记录

认领 Claude-phase1,2026-09-27(`305e74bc`)。分支 `refactor20260927/P1-01`,按 tasks.md 2.1 分三个提交:

| 提交 | 内容 |
|---|---|
| `15184732` | 7 个测试 helper 头 `git mv` 到 `tests/test_support/<area>/`(全部 R100;映射 = layout-map.md §8 与 `src_layout_map.tsv` 的 7 条 P1-01 行);`computer_use_native_smoke` 补 `${CMAKE_SOURCE_DIR}/tests` 根(它的两个冒烟源是 computer_use 三个 helper 头的唯一使用方,`acecode_unit_tests` 早已带该根) |
| `5524a352` [mechanical] | `normalize_includes.py --scope src`:369 文件、1150 行(1122 行 `../` + 28 行子目录相对写法)改为模块根形式,同目录裸名保留 |
| `7a90db6d` [mechanical] | `normalize_includes.py --scope tests`:48 文件、54 行(16 行 `../`、1 行 `agent_loop/stub_provider.hpp`、37 行同目录裸名)改为 `test_support/<area>/` 完整前缀 |
| `8a4524af` | 回归修复:`tests/test_support/channels/test_support.hpp` 原以 `__FILE__` 同目录定位夹具 `tests/channels/fake_bridge.mjs`,搬家后指向不存在的脚本,全新目录首轮全量单测 10 个 Channel* 用例全部报「WhatsApp bridge exited」;改为 P0-05 的 `find_repo_root(__FILE__) / "tests/channels/fake_bridge.mjs"`。其余 6 个搬走的 helper 头没有 `__FILE__` 相对定位 |

工具只改引号内的路径,其它字节(CRLF/LF 混排、末尾换行、平台条件分支)不动;头文件不在任何 target 的显式源清单里,目标快照不受 `git mv` 影响。

## 1. §7.2「P1」闸门逐项

| 闸门 | 结论 | 证据 |
|---|---|---|
| numstat 满足每个文件「增加 = 删除 = 改动的 include 行数」 | 满足 | `git diff --numstat --diff-algorithm=histogram`:src 369 文件 1150/1150,tests 48 文件 54/54,逐文件与工具报告的改动行数相等;`-U0` 补丁里 2300 + 108 行 +/- 全部匹配 `#include "`。默认 myers 在 `src/daemon/worker.cpp`(67 vs 66)与 `src/web/routes/routes_environment.cpp`(9 vs 8)上各多报 1 行,是相邻 include 行前后缀相同导致的非最小编辑脚本,补丁正文与 histogram / patience 均为 66 与 8 |
| 第二次运行 0 diff | 满足 | `normalize_includes.py --check --scope src` 与 `--scope tests` 均 exit 0、changed_lines 0、errors 0 |
| src 下不再有 `../` | 满足 | `grep -rE '^\s*#\s*include\s*"\.\./' src` 为 0(tests 同样为 0);`check_layers.py --enforce-parent-includes` exit 0,parent-relative 0 项 |
| Win/Linux/mac/arm/Deepin 全新目录构建通过 | 满足 | 本机 Windows 全新目录 `build-p1`(第 2 节);`package.yml` run 36304753531 全部 11 个平台作业成功(含 windows-arm64、linux-arm64 / armv7、macos-arm64、三个 Deepin 包);`refactor-matrix` run 36305642345 三平台 + run 36304751667 的 Deepin configure + build(第 3 节) |
| 用例清单与 SKIP 清单等于 G0(D23:以 post-p0 为对照) | 满足 | 四平台 `compare_snapshots.py`:`tests` 名单、ctest 注册名、SKIP 集合与目标快照全部逐条相同(第 3 节表);本机同样相同(第 2 节)。失败集合的差异只有两条已知抖动 |
| 公告 9 个遗留分支 | 满足 | `branch-inventory.md`「P1-01 公告」一节;AGENTS.md「Coding Style」、CLAUDE.md 顶部提示与 `tests/README.md` Conventions 同步写明 include 写法与 helper 头位置 |

## 2. 本机验证(Windows,全新目录 `build-p1`)

全新目录 `build-p1`(Ninja + MSVC 2022,Release,`BUILD_TESTING=ON`,`ACECODE_BUILD_DESKTOP=ON`,x64-windows-static,只读复用 `build/vcpkg_installed`),configure 前写 File API query:

| 步骤 | 结果 |
|---|---|
| configure | 通过(`acecode_assert_known_roots` / `acecode_require_sources` 护栏均通过) |
| `cmake --build --target acecode acecode-desktop acecode_unit_tests computer_use_native_smoke computer_use_broker_smoke` | 通过(1029 步;两个 computer_use 冒烟目标是三个搬走的 helper 头的唯一使用方,一并构建证明新 include 根生效) |
| 目标快照 vs `baseline/g0/post-p0/windows-x64/targets.json` | 59 target / 3529 元组,`compare_snapshots.py` `unexpected=false`,0 增 0 减。唯一的环境差异是本机 vcpkg_installed 位于源根下的 `build/`,快照记成 `build/vcpkg_installed/...`,CI 的在构建目录内记成 `@build/...`;把这一个前缀归一后逐元组相同(25 条 `nlohmann_json.natvis` 元组) |
| 全量单测(`gtest_inventory.py --run`,隔离 HOME / TEMP 在仓库外 `N:cecode-p1-iso`) | 首轮:5115 列出 / 5114 执行 / 9 SKIP / **10 失败**(全部 Channel*,根因是 helper 头搬家后 `__FILE__` 相对定位失效,见上表 `8a4524af`);修复后增量重建再跑全量:5115 / 5114 / 9 SKIP / **0 失败**。`tests` 与 `ctest_names`(5119)与 post-p0 逐条相同;SKIP 比 CI 基线少 1 条 `Encoding.AutoDecoderRetainsLegacyDiagnosticsUnderUtf8Console`(本机控制台代码页满足其前置条件,CI runner 上 SKIP,环境差异,与 P0 本机记录一致) |

lint(同一台机器):`check_layers.py --enforce-parent-includes` exit 0;`normalize_includes.py --check` src / tests 均 exit 0;`check_file_size` / `check_ownership` / `validate_map` `--strict` 均 exit 0;`unittest discover scripts/refactor/tests` 65 项通过。

## 3. CI

两条手动触发的工作流,源码都指向分支头(`--ref master` 取工具,`source_ref` 取源码):

| 工作流 / run | 源码 | 结果 |
|---|---|---|
| `package.yml` run 36304753531 | `7a90db6d`(`refactor20260927/P1-01`) | 全部 11 个打包作业成功:windows-x64 / windows-arm64、linux-x64 / linux-arm64 / linux-armv7、macos-x64 / macos-arm64、linux-deepin-x64 / linux-deepin-arm64 / linux-deepin-armv7、web-dist;release / publish-npm 按设计跳过(非 tag)。生产源码与最终树相同(`8a4524af` 只改测试 helper 头) |
| `refactor-matrix.yml` run 36304751667(`p1-01`) | `7a90db6d` | 四平台构建全部成功;Windows / Linux / macOS 的用例清单里各多出同一批 10 个 Channel* 失败(Linux 0 → 10,Windows 4 → 13,macOS 10 → 21 含 1 条已知抖动),与本机首轮一致,即 helper 头搬家引起的回归;Deepin 目标快照 13 target / 1353 元组与 post-p0 逐元组相同 |
| `refactor-matrix.yml` run 36305642345(`p1-01-v2`) | `8a4524af`(修复后) | 见下表,正式采用 |

`p1-01-v2` 与 `baseline/g0/post-p0/<platform>` 的对照(`compare_snapshots.py`,无 `--allowed-addition`):

| 平台 | 目标 target / 元组 | 用例:post-p0 → P1-01 | 差异 |
|---|---|---|---|
| windows-x64 | 59 / 3529 → 59 / 3529,逐元组相同 | 5115 列出 / 5108 执行 / 10 SKIP / 4 失败 → 5115 / 5108 / 10 SKIP / 5 失败 | 用例集合与 SKIP 集合相同;失败集合 = 基线 4 条(`HookRunner.ShellCommandReceivesExactEnvironmentOverride`、`McpManagerAsync.ProjectOverridesHaveIsolatedToolDiscoveryAndDispatch`(P0 记录的抖动)、`dev_desktop_script_unit`、`verify_package_python_unit`)+ `AgentLoopTurnSteering.EveryAcceptedFinalBoundaryRaceInputIsCommitted` 1 条:时序竞争用例,同一份代码在本机两轮全量、第一轮 CI(`7a90db6d`)以及 Linux / macOS 上都通过,P1-01 不改任何函数体,记为 Windows CI 抖动 |
| macos-arm64 | 57 / 3588 → 57 / 3588,逐元组相同 | 5040 / 5033 / 14 SKIP / 10 失败 → 5040 / 5033 / 14 SKIP / 11 失败 | 用例与 SKIP 相同;失败 = 基线 10 条 + `OpenAiProviderErrorRecovery.SseKeepaliveCommentsDoNotTriggerRetry`(P0-acceptance 已记录的 macOS CI 抖动) |
| linux-x64 | 50 / 3443 → 50 / 3443,逐元组相同 | 5035 / 5026 / 16 SKIP / 0 失败 → 5035 / 5026 / 16 SKIP / 0 失败 | 完全相同 |
| linux-deepin-x64(取自 run 36304751667,源码 `7a90db6d`,BUILD_TESTING=OFF) | 13 / 1353 → 13 / 1353,逐元组相同 | — | Deepin 不编译测试,`8a4524af` 对它无影响 |

Linux 的四类 lint 报告随 artifact 归档(`refactor-matrix-p1-01-v2-linux-x64/lint/`),分层违规 161 项,与本机一致。

## 4. lint 变化(报告模式,同一台机器同一工具)

| 项目 | P1-01 之前(post-p0 基线) | P1-01 之后 |
|---|---|---|
| 分层违规总数 | 1308 | 161 |
| R8 | 1259(其中 parent-relative 1138) | 119(parent-relative 0;116 项是 tests 里指向 `src/` 根层散文件的裸名 include,如 `agent_loop.hpp` / `permissions.hpp`,P2-08 搬迁根目录时消失;3 项是 `upgrade/version.hpp` 与生成的 `version.hpp` 重名,P2-05 改名为 `utils/semver` 后消失) |
| R14 | 8 | 1(`tests/smoke_test.cpp` 不镜像任何模块,P2-08 归位) |
| doc-paths | 84 | 84(无新增失效路径) |
| 行数 / 所有权棘轮、映射校验、工具自测 | 通过 | 通过(`check_file_size --strict`、`check_ownership --strict`、`validate_map --strict` exit 0;`unittest discover scripts/refactor/tests` 65 项通过) |

## 5. 触碰到的不变量(§7.3)

只改 include 路径与 helper 头位置,不改任何函数体、宏、目标归属;§7.3 各条守护测试随全量单测运行(见第 2 节)。§7.3.9 的 target 归属由第 2 节的目标快照对照证明。
