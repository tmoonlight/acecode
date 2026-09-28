# P2-06 hooks 与 skills 拆分验证

任务认领由 PR #82 合入 master,基线 `d993ae5f00c2d4c475a04c05d5509ffbbe3b4d8e`,已包含 P2-03 / P2-04 / P2-05 的验收。工作分支为 `refactor20260927/P2-06`,复用已合入且无活动进程、无未提交改动的原 P2-03 工作区;构建使用新目录。

## 实现边界

- `51c37dea`:13 个文件的纯 `git mv`,逐项 R100,0 行增删。frontmatter 归 utils,opencode_command 与 skill_command_expander 归 skills,skill_commands 归 tui/commands;五个测试同步镜像归位。
- `f095f762`:19 个源/测试文件的 20 处 include 路径改写;CMake 删除失效的根级 skill_commands.cpp 子集项,已登记的 commands/ 子集继续确保该实现参与 acecode_testable。
- 模型注册源、模型加载完成和助手消息完成三个事件构造器及其私有辅助移到 agent/hook_bridge/hook_events。hooks/hook_payload 仅保留无 provider 依赖的启动前事件;调用方直接引用实际定义头,旧路径无转发壳。
- DefaultHookSeed 与只读种子表移到 hooks/hook_seeds。注册表只依赖 hooks;skills 安装器继续协调完整种子事务并读取该表,既有版本/哈希/迁移规则保持。
- 两类启动事件复用已有 platform 进程 ID 原语,保留原来的 int 转换。移除终端探测对 hooks 配置的无用 include。
- 同步 CLAUDE.md、技能说明和对应测试注释中的路径;help-source 生成器未引用此次移动路径,无须改写生成内容。

## 当前检查

src / tests include 规范化第二次检查为 0 改动;映射 strict、分层已有阻断项、行数和所有权 strict 通过。分层违规从 P2-05 的 87 项降为 83 项,本任务相关 hooks→provider、hooks→skills 和 environment→hooks 反向依赖消除。其余项目仍由后续 P2-07 / P2-08 处理。

前端首次测试因本工作区未安装 node_modules,缺少 @babel/core 而未能启动完整套件;已执行 pnpm install --frozen-lockfile,不改锁文件。依赖齐备后的完整 `pnpm test` 与 `pnpm build` 均通过。

对照机械提交前后的函数文本,三个模型事件构造器除了等价的进程 ID 原语调用外保持原样;hook 种子表函数完全相同。

## 本机原生验证

固定源码 `df9ef9567bb24eabb7b6294ea16f49fd991f845d`,使用 MSVC 2022 x64 / Ninja / Release,全新目录 `build/p2-06-hooks-skills`,BUILD_TESTING 与 Desktop 均开启。acecode、acecode-desktop、acecode_unit_tests 及五个 EXCLUDE_FROM_ALL 冒烟目标全部构建成功。

CMake File API 为 59 个目标、3637 个元组;对照已验收 P2-05 的 59 / 3623,目标和原有源文件归属、编译参数全部相同,无删除元组。14 个新增元组仅对应 hook_events 与 hook_seeds 的源、头及生成对象。

- 定向用例 351 / 351 通过,0 SKIP,包括三个 SkillCommandsReload、11 个 TerminalResolver 及 hooks / skills / seeder 用例。
- 独立 HOME/TEMP 下完整用例清单 5117,实际运行 5116,9 SKIP,0 失败,退出码 0。
- 对照 P2-05 完整本机记录,用例名和 SKIP 集合完全一致,未通过过滤隐藏失败。

`migrate_branch.py --check` 已执行,总体仍为非零:documentation 79、layers 2331(含尚未进行的最终分组检查)、migration_paths 60;include_normalization、map、ownership、seed 均为 0。此报告不代表 P3 已完成;过渡布局的实际分层违规仍是上文的 83 项。九个遗留分支的迁移通知已记录在 branch-inventory.md。

## 四平台验证

[refactor-matrix 36341836305](https://github.com/tmoonlight/acecode/actions/runs/36341836305) 固定上述源码提交,Windows / Linux / macOS / Deepin 全部完成构建与采集。PR #84 合入的窗口预告随后已合并到本分支,仅改 AGENTS.md / CLAUDE.md,src、tests、CMake、scripts、assets、web 相对验证源码无差异。

| 平台 | targets / tuples | 用例清单 | 实际执行 / SKIP | 失败 |
|---|---:|---:|---:|---:|
| Windows x64 | 59 / 3637 | 5117 | 5110 / 10 | 4(既有基线) |
| Linux x64 | 50 / 3551 | 5037 | 5028 / 16 | 0 |
| macOS arm64 | 57 / 3696 | 5042 | 5035 / 14 | 10(既有基线) |
| Deepin x64 | 13 / 1400 | 不运行单测 | 不适用 | 构建成功 |

CI 使用 CTest 逐用例运行,实际执行统计包含脚本测试,与本机直接运行 gtest 的计数口径不同。四个平台全部保留既有 target 和源文件归属、编译参数;对照 P2-05,三个完整平台仅增加本轮两个拆出实现的 14 个元组,Deepin 增加 6 个。三个测试平台的用例与 SKIP 集合相对 P2-05 完全相同;相对 G0 仅有 P2-05 已登记的两个 TextFileToolErrors 新增用例。

Windows 与 macOS 的失败名称均在 G0 中,断言或异常特征也逐项对照此前 P2-05 日志一致。保留实际失败,未放宽断言、增加过滤或重跑取绿。Windows 既有失败为:


- `HookRunner.ShellCommandReceivesExactEnvironmentOverride`
- `McpManagerAsync.ProjectOverridesHaveIsolatedToolDiscoveryAndDispatch`
- `dev_desktop_script_unit`
- `verify_package_python_unit`

macOS 既有失败为:

- `BuiltinToolRegistry.NativeBrowserToolsCanBeUnregisteredAsOneGroup`
- `BuiltinToolRegistry.RegistersSharedCoreAndPlatformBrowserTools`
- `ManagedRemoteWebProxy.AutomaticPortFallsBackWhenAdjacentPortIsBusy`
- `ManagedRemoteWebProxy.StartsRealChildAndStopsWithoutOrphaningIt`
- `RemoteWebTcpProxy.TransparentlyForwardsPersistentBidirectionalBytes`
- `SandboxPolicy.ResolveAccessPrefersDeepestEntryThenDenyWriteRead`
- `SettingsCenterRender.NarrowTopRailFollowsDeepLinkedTab`
- `StateFileTest.ClaimFlagIsGrantedExactlyOnceAcrossProcesses`
- `WebServerHttp.ModelRoutesRejectUnauthenticatedRemoteAccess`
- `WebServerHttp.RealProxyRequiresTokenAndForwardsAuthenticatedHealth`

P2-04 曾记录的 `AgentLoopTurnSteering.InterruptStartsStructuredTurnBeforeOrdinaryQueue` 和已知 macOS SSE 时序失败,本轮均未出现。

三个完整平台的 provenance 源码 SHA 与固定提交一致。Deepin 的 source_revision 仍因容器 Git 归属检查为空;额外核对构建 job `108683316416` 的 checkout 命令、HEAD 提示及紧接 `git log -1 --format=%H` 的完整 SHA,均为 `df9ef9567bb24eabb7b6294ea16f49fd991f845d`,未将空 provenance 当作源码证明。

## 不变量与合入条件

本轮完整测试实际执行并通过 `AgentLoopTermination.RequestPrefixIsByteStableAcrossIterationsInATurn`、`SystemPromptTest.NonGptModelStateIsByteIdenticalToLegacyPrompt`、`HookAgentLoop.DispatchesAssistantCompletedAfterTextMessageCommit`、`HookAgentLoop.SessionStartAdditionalContextReachesNextRequestOnly`、`HookAgentLoop.PermissionResolvedPairsWithHookApprovalExactlyOnce` 与三个 `SkillCommandsReloadTest`。CMake 逐元组对照同时覆盖 Desktop 链接边界与按源编译属性。

P2-06 的实现、构建归属、测试清单、静态检查和已知失败复核已完成。任务复选框保持未勾选,PR #83 实际合入后立即登记验收并认领下一项。
