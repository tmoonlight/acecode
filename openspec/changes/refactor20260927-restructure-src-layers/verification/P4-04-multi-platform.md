# P4-04 多平台补验

## 2026-09-29 首轮结果与编译修复

首轮代码:82fad0cde02a694ab100f70548360bc6b0efde60。

- [package 36468672076](https://github.com/tmoonlight/acecode/actions/runs/36468672076):Windows x64/ARM64、Linux x64/ARM64/ARMv7、Deepin x64/ARM64/ARMv7 构建并上传成功;macOS x64/ARM64 在 helper_main_macos.mm 编译失败。
- [test 36468534513](https://github.com/tmoonlight/acecode/actions/runs/36468534513):Web、分层和 installer 检查通过,Linux 测试程序编译失败,尚未进入单测执行。
- [refactor-matrix 36468666045](https://github.com/tmoonlight/acecode/actions/runs/36468666045):Windows 与 Deepin 通过;macOS 和 Linux 分别遇到上述编译错误。

本次修复:

1. macOS helper 的 main 位于全局作用域,显式调用 acecode::spawn_owned_detached,保留原来的值捕获和所有权语义。
2. MCP 命令测试分别保存启用和禁用后的 ToolCapabilityPolicy 值快照,再将局部对象地址传给同步查询;避免对临时对象取地址,并保证禁用后的断言读取新的策略。

本机验证:复用 build/refactor-phase1-windows 的 MSVC Release/Ninja 增量构建 acecode_unit_tests 成功;run_fast_tests.py --profile full --filter "BuiltinCommands.*" 执行 47 条(含强制守护用例),0 SKIP、0 失败,5.4 秒。全量用例清单仍为 5304 条,无增删。严格分层与所有权检查通过。

本次编译修复的原生 CI 结果见下节。P4-04 完整验收保持未勾选,既有人工专项与九个旧 ref 的后续安排不变。

## 原生构建继续复核

[修复后 package 36516779969](https://github.com/tmoonlight/acecode/actions/runs/36516779969) 已越过 macOS helper 的原报错位置,继续编译至 TerminationSignal 时发现 macOS SDK 把 sigemptyset 定义为函数式宏,`::sigemptyset(...)` 展开后语法无效。已改为不带作用域限定的调用,同时兼容 Linux 的函数声明与 macOS 的宏,不改变信号掩码语义。全仓同类带作用域限定的 sigset 调用仅此一处。

该补充修复已通过 Windows MSVC/Ninja 增量构建、TerminationSignal.* 与守护用例共 12 条(0 SKIP、0 失败)、严格分层检查。后续原生 CI 已通过,结果如下。

## 修复验证通过

验证代码:21842477dd8fb5a862ba519bc765202a5fe4cdb5,包含 7f112802 的限定名/策略快照修复与 21842477 的信号宏兼容修复。

| 流水线 | 结果 |
| --- | --- |
| [test 36517786705](https://github.com/tmoonlight/acecode/actions/runs/36517786705) | 成功;Linux CLI/Desktop/测试程序编译及完整 ctest 通过,Web、严格分层与 macOS installer 检查通过 |
| [package 36517783704](https://github.com/tmoonlight/acecode/actions/runs/36517783704) | 成功;Windows x64/ARM64、macOS x64/ARM64、Linux x64/ARM64/ARMv7、Deepin x64/ARM64/ARMv7 共十个平台全部构建并上传成功;分支验证按配置跳过 release/npm 发布 |

本次三处编译兼容问题已解决。P4-04 的完整矩阵快照、各平台用例与 SKIP 集合对照仍是后续验收事项,不因本次编译修复提前勾选。

## 2026-09-30 固定 SHA 完整补验

本次源码固定为 master `5e9258d16a9caf978c982e262e985b416e19b040`（v0.9.30）。
[refactor-matrix 36687146493](https://github.com/tmoonlight/acecode/actions/runs/36687146493) 使用 `source_ref` 完整 SHA、`run_tests=true`、`include_deepin=true`、label `p4-04-5e9258d-20260930`，已跟踪至终态 success，五个 job 全部成功。四平台 CLI/Desktop 构建均通过。

对照 [P2-06 矩阵 36341836305](https://github.com/tmoonlight/acecode/actions/runs/36341836305)，基线源码为 `df9ef9567bb24eabb7b6294ea16f49fd991f845d`。9 月 28 日旧矩阵的失败属于旧源码，不能作为本次 SHA 的结果。

**验收范围完成：四平台构建、完整 target/File API、三个测试平台用例/SKIP 与已登记失败对照均已补齐。此结论不表示所有测试断言通过。** 工作流会保存 CTest 的非零返回码后继续上传证据，因此必须以以下原始测试结果为准。

### 测试与 SKIP 对照

“执行”不含 SKIP 和禁用项，包含 CTest 的额外脚本检查；GTest 清单与 CTest 注册总数分别列出，避免混淆。

| 平台 | GTest 清单：P2-06 → 本次 | CTest 注册 | 执行 | 通过 | 失败：P2-06 → 本次 | SKIP：P2-06 → 本次 | 禁用未跑 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| windows-x64 | 5117 → 5304 | 5309 | 5298 | 5296 | 4 → 2 | 10 → 10 | 1 |
| linux-x64 | 5037 → 5218 | 5227 | 5210 | 5210 | 0 → 0 | 16 → 16 | 1 |
| macos-arm64 | 5042 → 5223 | 5232 | 5217 | 5206 | 10 → 11 | 14 → 14 | 1 |
| linux-deepin-x64 | 未启用测试 | — | — | — | — | — | — |

三个平台均无删除用例、无新增或移除 SKIP，无未交代的启用用例漏跑。唯一禁用项仍是 `StreamingBenchmark.DISABLED_PrintsFullVsCompatibilityTimeCurve`，单列为未跑，不混入 SKIP。Deepin 工作流固定 `BUILD_TESTING=OFF`，本次 `include_deepin=true` 只增加构建与快照，并未执行 Deepin 单测。

新增用例逐条关联当前源码的 TEST/TEST_F 声明，见 [test-delta-register.json](p4-04-20260930/test-delta-register.json)；每平台 37 项 hooks/skills、关停、任务队列、prompt 快照与取消守护全部执行通过。SKIP 名称与原因、失败名称、完整用例清单均保存在平台报告与原始 archive 中。

CTest 的参数诊断包含 ASLR 地址和对象内存转储；对照逻辑 ID 时仅去掉 `# GetParam()` 诊断并归一化指针地址，保留用例/参数名与字符串值。原始名称和原始差异仍保留，归一化后无重名碰撞。没有借此删除测试。

### 失败逐项登记

详见 [failure-review.json](p4-04-20260930/failure-review.json)，保留本次与基线原始断言块，不通过重跑筛选绿色结果。

- **windows-x64**：与 P2-06 同名失败 2 项，基线失败未复现 2 项，相对 P2-06 增加 0 项。
  同名失败：`HookRunner.ShellCommandReceivesExactEnvironmentOverride`、`dev_desktop_script_unit`。
  未复现：`McpManagerAsync.ProjectOverridesHaveIsolatedToolDiscoveryAndDispatch`、`verify_package_python_unit`。
- **linux-x64**：与 P2-06 同名失败 0 项，基线失败未复现 0 项，相对 P2-06 增加 0 项。
- **macos-arm64**：与 P2-06 同名失败 10 项，基线失败未复现 0 项，相对 P2-06 增加 1 项。
  同名失败：`BuiltinToolRegistry.NativeBrowserToolsCanBeUnregisteredAsOneGroup`、`BuiltinToolRegistry.RegistersSharedCoreAndPlatformBrowserTools`、`ManagedRemoteWebProxy.AutomaticPortFallsBackWhenAdjacentPortIsBusy`、`ManagedRemoteWebProxy.StartsRealChildAndStopsWithoutOrphaningIt`、`RemoteWebTcpProxy.TransparentlyForwardsPersistentBidirectionalBytes`、`SandboxPolicy.ResolveAccessPrefersDeepestEntryThenDenyWriteRead`、`SettingsCenterRender.NarrowTopRailFollowsDeepLinkedTab`、`StateFileTest.ClaimFlagIsGrantedExactlyOnceAcrossProcesses`、`WebServerHttp.ModelRoutesRejectUnauthenticatedRemoteAccess`、`WebServerHttp.RealProxyRequiresTokenAndForwardsAuthenticatedHealth`。
  相对该次基线增加：`OpenAiProviderErrorRecovery.SseKeepaliveCommentsDoNotTriggerRetry`。

Windows 两项保留失败的具体断言与基线相同：HookRunner 的中文环境变量经 shell 输出为问号；dev_desktop 的两个子测试在 cp1252 stdout 输出中文时抛出 UnicodeEncodeError。macOS 十项保留失败的断言/进程异常也逐项对照一致，详情保留原文。

macOS 的 `OpenAiProviderErrorRecovery.SseKeepaliveCommentsDoNotTriggerRetry` 是已登记的历史间歇性失败。P2-06 记录明确写过该次未复现；[P2-05 run 36336950956](https://github.com/tmoonlight/acecode/actions/runs/36336950956) 原始日志在同一文件 396/398 行报告调用次数 5（预期 1）、Retry 4（预期 0），本次对应为 6/5，均为额外重试。测试文件从 P2-06 到本次 SHA 完全未改，夹具使用 100 ms 超时与 50 ms keepalive 间隔。证据见 [historical-sse.json](p4-04-20260930/historical-sse.json)。这支持归入已知问题；并不证明其根因已经修复。

### 四平台完整 target 对照

P2-06 后的 [P5 构建库改造](../../refactor20260929-build-layer-libraries/verification.md) 已把 OBJECT/聚合源码改为 12 个 STATIC 层库和无源码 INTERFACE 聚合。因此完整元组确有登记过的变化，不能把结果写成“逐元组相等”。本次保存全部差异，同时核对生产翻译单元、唯一归属、最终链接图与逐源编译属性。

| 平台 | target 数：前 → 后 | 元组数：前 → 后 | 完整元组移除 / 新增 | 生产 TU：前 → 后 | 源路径新增 / 移除 |
| --- | --- | --- | --- | --- | --- |
| windows-x64 | 59 → 68 | 3637 → 2071 | 2951 / 1385 | 464 → 636 | 173 / 1 |
| linux-x64 | 50 → 59 | 3551 → 1974 | 2946 / 1369 | 462 → 632 | 173 / 3 |
| macos-arm64 | 57 → 66 | 3696 → 2115 | 2953 / 1372 | 471 → 641 | 173 / 3 |
| linux-deepin-x64 | 13 → 22 | 1400 → 1356 | 1352 / 1308 | 463 → 633 | 173 / 3 |

- 四平台均验证 12 个 STATIC 库、144 个 TUI 实现全归属 `acecode_tui`、每个生产 TU 仅一个主构建归属、无 whole-archive、嵌入 Web 资源仅归属 `acecode_web`。CLI/测试消费完整生产库；Desktop 项目库仅 base_core/desktop_support（Deepin 加专用窗口效果库），可见项目 include 仅 apps/base。专用会话写者和状态文件 driver 的最小依赖也通过。
- 每平台新增的 173 个生产源文件来自已合入的 TUI/AgentLoop 拆分与后续实现，逐项保存在平台 JSON。共同移除 `src/apps/tui/tui_helpers.cpp` 是 B-06 已登记拆分；非 Windows 另移除两个仅 `_WIN32` 有实现的 pointer_capture/pointer_overlay TU，文件仍在仓库，由 P5 显式平台清单排除。
- 保留源的语言与编译选项无变化；编译宏差异是 P5 缩小私有依赖可见性后去掉不再需要的 ASIO/CURL/COMPILING_WINPTY_DLL 传递定义，或移除同一源此前在聚合 target 的重复编译配置。完整逐源设置保存在 `*-source-settings.json.gz`。Deepin manifest 的 `ACECODE_DEEPIN=1`、channel bridge 特殊定义和 macOS OBJCXX 清单另列在平台 JSON。
- Linux job 的严格分层、行数、最终所有权、文档路径与 src/tests include 六份报告通过，exceptions 为空。原始 lint 报告随 Linux archive 保存。

### 打包、Web 与 Windows 补充证据

复核同一完整 SHA 的既有 [test 36591040199](https://github.com/tmoonlight/acecode/actions/runs/36591040199) 和 [package 36591874094](https://github.com/tmoonlight/acecode/actions/runs/36591874094)，均 success。前者包含 Web `pnpm test`/build、严格闸门、macOS installer 与 Linux 完整 CTest；后者十个平台全部构建并上传成功。本次复用已完成且 SHA 一致的打包证据，没有再次 dispatch package/release/deploy。

从该 package run 下载实际 Deepin x64 产物，在 WSL Ubuntu 的隔离 HOME 中用本地 404 manifest 桩探测，输出 `Platform: linux-deepin-x64`。仅请求 manifest 后按预期退出 1，未下载或安装更新，二进制前后 SHA-256 相同。见 [deepin-current-target.json](p4-04-20260930/deepin-current-target.json)。这验证 Deepin 包的平台识别，不声称执行了 Deepin 桌面人工测试。

ShaoPCIII 复用 `build/refactor-phase1-windows`，持有项目构建锁后以 MSVC Release/Ninja 构建 CLI、Desktop、unit_tests 及五个 EXCLUDE_FROM_ALL 冒烟目标，成功；本机构建图检查通过。应用 [verify-package 技能](../../../../.agents/skills/verify-package/SKILL.md) 后检查成功，日志为 PASS (16 checks)，其中 Desktop launch 因用户已有 Desktop 进程而明确 SKIP，未关闭或重启用户实例。见 [构建记录](p4-04-20260930/windows-build.json)、[图检查](p4-04-20260930/windows-layer-graph.json)、[包检查原始日志](p4-04-20260930/windows-package.log)。

设计 §7.2 的五项运行冒烟沿用已验收的 [Windows 一期实测](windows-phase1-validation.md)：TUI 对话、`acecode -p`、daemon/Web 会话、Desktop workspace、ConPTY/winpty；本次没有把五个构建目标冒充五项运行冒烟，也没有宣称重做这些人工步骤。已有 [人工延期范围](windows-phase1-manual-coverage.md) 保持。

### 本次发现与本地修复

Deepin 的 P2-06 与本次原始 `provenance.json` 均出现空 `source_revision`，日志显示旧容器 Git 的 dubious ownership 拒绝。两个 checkout 步骤均打印了各自完整预期 SHA，见 [deepin-source-proofs.json](p4-04-20260930/deepin-source-proofs.json)；原始证据未补写或篡改。

本地 `.github/workflows/refactor-matrix.yml` 已改为仅对本次 `$GITHUB_WORKSPACE` 配置全局 safe.directory，在独立严格步骤中 `rev-parse --verify HEAD`，验证 40 位 SHA 并写入 GITHUB_ENV，再供 provenance 使用。YAML 解析、bash 语法及隔离异主目录正/负向测试通过：未信任时返回 128、显式信任后取得完整 SHA、仓库不存在时失败且不输出空 SHA，见 [provenance-fixture.json](p4-04-20260930/provenance-fixture.json)。本机测试 Git 为 2.34.1；实际 Buster 容器复测仍待推送该工作流补丁后手动触发，不能算作本次固定 SHA 的已执行修复。本次未推送。

### 证据重现与剩余事项

[evidence-index.json](p4-04-20260930/evidence-index.json) 保存 GitHub artifact ID/链接、原始下载文件的 SHA-256 和本地归档 SHA-256。8 份 ZIP 是对原始 artifact 内容的确定性重打包，并非声称 ZIP 字节与 GitHub 下载包相同；保留完整 File API reply、targets、provenance、GTest/CTest 清单和执行日志。全量元组差异采用 gzip 保存。

归档已逐文件验证 581 个原始文件的哈希，并从重新解压的 8 份 ZIP 独立重放四平台对照，结果与保存报告完全相同，见 [archive-verification.json](p4-04-20260930/archive-verification.json)。

解压 8 个 ZIP 到同一证据目录后，可在本次 SHA 的 checkout 运行：

```sh
python openspec/changes/refactor20260927-restructure-src-layers/verification/p4-04-20260930/compare_evidence.py --repo . --evidence <解压目录> --output <报告目录>
```

任务 5.4 按“构建通过、用例/SKIP 无未登记变化”的验收条件完成；上述 Windows/macOS 已知失败仍保留，不因勾选而关闭。剩余：已登记平台失败的专项治理、本地 Deepin provenance 修复在实际 Buster CI 的复测、既有人工专项。P3-03 九个旧 ref/worktree 保持原样，不清理、不迁移，P3-03 仍不勾选。本次只有本地工作流/验收记录/证据改动，无提交、推送、合并或发布。
