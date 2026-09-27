# Phase 0 验收记录(design.md §7.2「P0」行)

验收人 Claude-phase0,2026-09-27。Phase 0 的全部任务(restructure P0-01–P0-08、
split-agent-loop P0-10 / P0-11、split-tui-main P0-09 / P0-12、ownership P2-01,以及提前
完成的 P2-09)已合入 master;代码完成提交为 `5596fc8e`,四平台 post-p0 基线以它采集。
逐任务的实现与验证细节见各自的 `verification/*.md`,本记录只对照 §7.2 的 P0 闸门逐项
给结论。

## 闸门逐项

| §7.2 P0 条目 | 结论 | 证据 |
|---|---|---|
| D1 / D2 / D15 / D21 / D22 已写进 `layers.tsv` 初版 | 满足 | `src/layers.tsv`(P0-03,`baseline/p0-tools/README.md`);D2 的 adapters 命名、D15 的 tests 镜像规则均以 module / exempt 行登记 |
| 正式规则下的违规数已记录 | 满足 | 原始 1326 项(`baseline/p0-tools/layers.json`,R8 1267);post-p0 1308 项(`baseline/g0/post-p0/linux-x64/lint/layers.json`,R8 1259,P0-08 / P0-10 删代码后下降,P1 之前不要求归零)。doc-paths 80 → 85:新增 5 处是 `docs/superpowers/plans/2026-08-27-streaming-incremental-layout.md` 与 `docs/产品*_2026-08.md` 两份历史文档引用了 P0-08 删除的 `message_render_cache.cpp` / `cli_dispatch.cpp` / `clipboard_helpers.cpp`,报告模式,留给 P4 收口 |
| test.yml 全绿 | 满足 | 合入后每次推送均通过:runs 36292603030(合并树)、36292991045(工作流)、36294463909 及之后;`layer-lint` 作业首跑即通过 |
| refactor-matrix 基线已采集 | 满足 | `baseline/g0/{original,post-p0}/<platform>/`,四平台各一份目标快照;Windows / macOS / Linux 另有 gtest 清单与实际 SKIP / 失败,Linux 另有四类 lint 报告(`P0-07-ci-and-g0.md`) |
| cmake_target_snapshot(含冒烟目标)等于 G0 | 按 D23 满足 | `baseline/g0/comparison/<platform>.json`:target 集合无增减;移除的元组全部属于 P0-08 的 20 个 delete 行(及其生成对象),新增的元组全部属于 P2-01 的 6 个原语文件与 `acecode_unit_tests` 下的新测试源;`unexpected=false`。原始 G0 的 Windows 数字(59 target / 3535 元组、5033 用例)与另一台机器 P0-04 / P0-08 记录里的数字逐字相同,证明采集可复现 |
| 故意改坏路径时 configure 报 FATAL | 满足 | 本机对合并树注入两次:显式清单里的 `tui_helpers.cpp` 改成不存在路径 → `acecode_source_guards.cmake:15` FATAL;`ACECODE_TUI_DIRS` 清空 → `CMakeLists.txt:198` "TUI source set is empty" FATAL;恢复后 configure 成功且快照不变。P0-04 记录另有四种注入 |
| TUI 源集合非空 | 满足 | 同上;快照里 `acecode` 保留 TUI-only 源,`acecode_testable` 保留可测 TUI / markdown 源 |
| gtest 清单与 SKIP 清单已归档 | 满足 | `baseline/g0/*/<platform>/gtest.json`(`tests` = `--gtest_list_tests` 全量,`actual_results.skipped` / `failures`,`ctest_names`) |
| 四类 lint 基线已接入 PR | 满足 | `test.yml` 的 `layer-lint` 作业在 `pull_request` 与 `push: master` 上运行:分层 / doc-paths 报告模式,行数与所有权棘轮、映射校验、工具自测阻断 |

## 逐任务状态

| 任务 | 结论 | 备注 |
|---|---|---|
| P0-01 立项定稿 | 完成 | `f06f2598` |
| P0-02 分支盘点 | 完成 | `branch-inventory.md` / `.json`;28 个旧 worktree 的清理仍由用户自行决定 |
| P0-03 迁移与 lint 工具集 | 完成 | 工具自测 46 项(含本次新增 9 项)通过 |
| P0-04 CMake 源文件护栏 | 完成 | 四平台 G0 对照 + 本机故障注入(见上表) |
| P0-05 测试路径健壮化 | 完成 | 原始 G0 本身就采自含 P0-05 的 `3ddb7d43` |
| P0-06 前端架构测试路径表 | 完成 | `tests/cpp_source_paths.json`,`pnpm test` 走 test.yml |
| P0-07 CI 与基线 G0 | 完成 | `P0-07-ci-and-g0.md` |
| P0-08 删死代码 | 完成(留一手工项) | 四平台全新目录构建通过,目标差异全部为授权删除;「微软拼音候选窗位置」需真人在 Windows TUI 里确认 —— 删除的是从未被调用的 `update_ime_composition_window`,按构造不影响候选窗 |
| P0-09 main.cpp 孪生 helper | 完成(留一手工项) | `split-tui-main/verification/P0-09-twin-helpers.md`;底栏 chip 逐个比对与手工清单第 2、3 小节需真人操作,渲染代码逐函数比对逐字相同 |
| P0-10 agent_loop 死代码 | 完成 | 另一台机器报的时序失败 `AgentLoopTurnSteering.InterruptStartsStructuredTurnBeforeOrdinaryQueue` 在本机两次全量与三平台 CI 上均通过 |
| P0-11 agent_loop 表征测试 | 完成 | 11 组全部落地(`P0-11-characterization.md` 收尾段) |
| P0-12 TUI 启动时序与快照 | 完成 | `startup-order.md` + 四场景快照;行号锚定 `3ddb7d43`,不受 P0-08 / P0-09 影响 |
| P2-01 RAII 原语 | 完成 | 24 条用例三平台通过;macOS CI 上有界等待用例的 100ms 上限放宽到 1s(`5596fc8e`) |
| P2-09 分支迁移工具 | 完成 | 九分支演练证据在 `baseline/p2-09/` |

## 各平台的基线失败与 SKIP

具体名单以 `baseline/g0/README.md` 的采集结果表为准。要点:

- **Windows(CI)**:原始与 post-p0 各 3 个失败:`HookRunner.ShellCommandReceivesExactEnvironmentOverride`
  (runner 的控制台编码把中文覆盖值打成 `??`),以及只在 ctest 模式下才执行的两个 Python 脚本套件
  `dev_desktop_script_unit` / `verify_package_python_unit`(另一台机器的 P2-01 记录里 `verify_package_python_unit`
  同样失败);post-p0-v4 另有 `McpManagerAsync.ProjectOverridesHaveIsolatedToolDiscoveryAndDispatch` 一次失败(0.6s,同一份 C++ 在
  post-p0-v3 与原始树上通过),记为 Windows CI 抖动;10 个 SKIP(网络 / 平台 / 需显式开启的冒烟)。
- **macOS(CI)**:原始树 10 个失败(`BuiltinToolRegistry.*` 2、`ManagedRemoteWebProxy.*` 2、
  `RemoteWebTcpProxy.*`、`SandboxPolicy.ResolveAccessPrefersDeepestEntryThenDenyWriteRead`、
  `SettingsCenterRender.NarrowTopRailFollowsDeepLinkedTab`、`StateFileTest.ClaimFlagIsGrantedExactlyOnceAcrossProcesses`、
  `WebServerHttp.*` 2),post-p0 同一批;`OpenAiProviderErrorRecovery.SseKeepaliveCommentsDoNotTriggerRetry`
  在 provider 代码相同的两次运行里一次通过一次失败(重试 8 次、257 秒),记为 macOS CI 抖动。
  单进程 gtest 在 macOS 上会被其中某个用例整体带崩,四平台采集因此改为 ctest 逐用例隔离。
- **Linux(CI)**:原始与 post-p0 均 0 失败,16 个 SKIP。
- **Windows(本机 build-p0merge,单进程,隔离 HOME 在仓库外)**:5114 执行 / 9 SKIP / 1 失败
  `ComputerUsePointerOverlay.ShowsWithoutActivationOrInterceptingHitTests`,合并前的旧 exe 在同一环境同样失败,
  属本机桌面环境;隔离 HOME 放在仓库目录内会让 ExpertRegistry / DefaultSkillSeeder / SpawnSubagent 等 14 条误报,
  不要那样跑。

这些失败都在原始基线上就存在(或为环境抖动),按 §7.1 只记入清单,不阻断 P0。

## 留给人工的两项

1. P0-08:在 Windows TUI 里用微软拼音输入中文,确认候选窗位置与删除前一致。
2. P0-09:运行 TUI,逐个核对底栏 token / 缓存命中 / 模型负载三个 chip,并跑手工清单第 2、3 小节。
