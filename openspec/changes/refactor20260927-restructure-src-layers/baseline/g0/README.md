# G0 四平台基线(P0-07)

本目录是 design.md §7.1 定义的 G0:每个平台一份 CMake File API 目标快照(含
EXCLUDE_FROM_ALL 冒烟目标)、gtest 用例清单 + 实际 SKIP + 失败、ctest 注册名;
Linux 另附四类 lint 报告。全部由 `.github/workflows/refactor-matrix.yml` 在全新
构建目录采集,artifact 原样落盘(`fileapi/` 原始 reply 与 `gtest-run.log` 不入库,
留在对应 run 的 artifact 里)。

## 目录

```
g0/
├── original/<platform>/   原始 G0:源码固定 3ddb7d43(P0-05 之后、P0-03 工具之前;
│                          src / CMake 与 7942011b 相同,tests 含 P0-05 的路径修复)
├── post-p0/<platform>/    Phase 0 完成时的快照(P0-01…P0-12、P2-01、P2-09 全部合入)
└── comparison/<platform>.json   compare_snapshots.py 的 original → post-p0 对照
```

平台:`windows-x64`(windows-2022,MSVC + Ninja,x64-windows-static,Release,
BUILD_TESTING=ON,Desktop ON)、`macos-arm64`(macos-15,arm64-osx,同上)、
`linux-x64`(ubuntu-22.04,x64-linux,同上)、`linux-deepin-x64`(buildpack-deps:buster
容器,MinSizeRel,BUILD_TESTING=OFF,Desktop ON + ACECODE_DEEPIN=ON,只有目标快照)。
每个目录的 `provenance.json` 记录源码 / 工具提交、run id、cmake 版本与 configure 参数。

## 采集与对照命令

```sh
# 采集(GitHub Actions 手动触发;source_ref 指向要采集的提交,tools 恒取工作流所在提交)
gh workflow run refactor-matrix.yml --ref master -f source_ref=<sha> -f label=<label> -f run_tests=true -f include_deepin=true
gh run download <run-id> -n refactor-matrix-<label>-<platform> -D <dir>

# 对照(D23):移除只允许 delete 行及其生成对象,新增只允许 P2-01 原语文件与 tests/ 新源
python scripts/refactor/compare_snapshots.py \
  --before original/<platform>/targets.json --after post-p0/<platform>/targets.json \
  --allowed-addition src/utils/abandonable_call.cpp --allowed-addition src/utils/abandonable_call.hpp \
  --allowed-addition src/utils/abort_signal.hpp --allowed-addition src/utils/joining_thread.hpp \
  --allowed-addition src/utils/lifetime_token.hpp --allowed-addition src/utils/scope_exit.hpp \
  --gtest-before original/<platform>/gtest.json --gtest-after post-p0/<platform>/gtest.json \
  --output comparison/<platform>.json
```

P1 起的逐元组比较以 `post-p0/` 为对照(D23);原始 G0 只用于追溯 P0 的删除与新增。
用例清单的比较同样以 `post-p0/` 的 `gtest.json` 为准:`tests` 是 `--gtest_list_tests`
的全量名单,`actual_results.skipped` 是实际 SKIP 名单及原因,`failures` 是当次失败。

## 清单采集方式

`gtest_inventory.py --run --via-ctest`:每个 ctest 条目(gtest_discover_tests 注册,
名字即 gtest 名)在独立进程中运行,`--test-timeout 900`;崩溃与超时记为该用例的
failure,不影响其它用例;SKIP 由 CTest 的 JUnit 报告给出。首轮单进程 gtest 在 macOS 上
挂起 / 崩溃且没有任何 XML,才改成这个方式;Windows / Linux 两种方式的用例清单一致。

## 采集结果

原始 G0:run 36295754425(`g0-original-v3`,源码 `3ddb7d43`;Deepin 来自 run 36292991337 `g0-original`);post-p0:run 36297269665(`g0-post-p0-v4`)。

| 平台 | 目标 target / 元组:原始 → post-p0 | 对照(D23) | 用例:原始 | 用例:post-p0 |
|---|---|---|---|---|
| windows-x64 | 59 / 3535 → 59 / 3529 | 一致(授权移除 19 文件 / 29 元组,授权新增 18 文件 / 23 元组) | 5033 列出 / 5026 执行 / 10 SKIP / 3 失败 | 5115 列出 / 5108 执行 / 10 SKIP / 4 失败 |
| macos-arm64 | 57 / 3594 → 57 / 3588 | 一致(授权移除 19 文件 / 29 元组,授权新增 18 文件 / 23 元组) | 4958 列出 / 4951 执行 / 14 SKIP / 10 失败 | 5040 列出 / 5033 执行 / 14 SKIP / 10 失败 |
| linux-x64 | 50 / 3449 → 50 / 3443 | 一致(授权移除 19 文件 / 29 元组,授权新增 18 文件 / 23 元组) | 4953 列出 / 4944 执行 / 16 SKIP / 0 失败 | 5035 列出 / 5026 执行 / 16 SKIP / 0 失败 |
| linux-deepin-x64 | 13 / 1367 → 13 / 1353 | 一致(授权移除 19 文件 / 21 元组,授权新增 6 文件 / 7 元组) | — | — |

失败用例(原本就失败的只记入清单,不阻断):

- windows-x64:原始 `HookRunner.ShellCommandReceivesExactEnvironmentOverride`, `dev_desktop_script_unit`, `verify_package_python_unit`;post-p0 `HookRunner.ShellCommandReceivesExactEnvironmentOverride`, `McpManagerAsync.ProjectOverridesHaveIsolatedToolDiscoveryAndDispatch`, `dev_desktop_script_unit`, `verify_package_python_unit`
- macos-arm64:原始 `BuiltinToolRegistry.NativeBrowserToolsCanBeUnregisteredAsOneGroup`, `BuiltinToolRegistry.RegistersSharedCoreAndPlatformBrowserTools`, `ManagedRemoteWebProxy.AutomaticPortFallsBackWhenAdjacentPortIsBusy`, `ManagedRemoteWebProxy.StartsRealChildAndStopsWithoutOrphaningIt`, `RemoteWebTcpProxy.TransparentlyForwardsPersistentBidirectionalBytes`, `SandboxPolicy.ResolveAccessPrefersDeepestEntryThenDenyWriteRead`, `SettingsCenterRender.NarrowTopRailFollowsDeepLinkedTab`, `StateFileTest.ClaimFlagIsGrantedExactlyOnceAcrossProcesses`, `WebServerHttp.ModelRoutesRejectUnauthenticatedRemoteAccess`, `WebServerHttp.RealProxyRequiresTokenAndForwardsAuthenticatedHealth`;post-p0 `BuiltinToolRegistry.NativeBrowserToolsCanBeUnregisteredAsOneGroup`, `BuiltinToolRegistry.RegistersSharedCoreAndPlatformBrowserTools`, `ManagedRemoteWebProxy.AutomaticPortFallsBackWhenAdjacentPortIsBusy`, `ManagedRemoteWebProxy.StartsRealChildAndStopsWithoutOrphaningIt`, `RemoteWebTcpProxy.TransparentlyForwardsPersistentBidirectionalBytes`, `SandboxPolicy.ResolveAccessPrefersDeepestEntryThenDenyWriteRead`, `SettingsCenterRender.NarrowTopRailFollowsDeepLinkedTab`, `StateFileTest.ClaimFlagIsGrantedExactlyOnceAcrossProcesses`, `WebServerHttp.ModelRoutesRejectUnauthenticatedRemoteAccess`, `WebServerHttp.RealProxyRequiresTokenAndForwardsAuthenticatedHealth`

用例清单差异(post-p0 相对原始,来自 comparison/*.json 的 gtest 段):

- windows-x64:用例 5033 → 5115(新增 82、删除 0;新增套件 AbandonableCallTest, AbortSignalTest, AgentLoopContextGolden, AgentLoopHandoffGolden, AgentLoopHandoffGoldenDeathTest, AgentLoopLifecycleGolden, AgentLoopPatchGolden, AgentLoopPermissionGrantGolden, ComputerUseLeaseGolden, JoiningThreadGroupTest, JoiningThreadTest, LifetimeTokenDeathTest, LifetimeTokenTest, ModelLoadChip, P0_11/AgentLoopPatchGolden, P0_11/AgentLoopPermissionGolden, P0_11/AgentLoopStaticPromptGolden, ProgressThrottleGolden, ReapingThreadSetTest, ScopeExitTest, ToolStreamThrottleGolden),SKIP 10 → 10(新增 无,消失 无)
- macos-arm64:用例 4958 → 5040(新增 82、删除 0;新增套件 AbandonableCallTest, AbortSignalTest, AgentLoopContextGolden, AgentLoopHandoffGolden, AgentLoopHandoffGoldenDeathTest, AgentLoopLifecycleGolden, AgentLoopPatchGolden, AgentLoopPermissionGrantGolden, ComputerUseLeaseGolden, JoiningThreadGroupTest, JoiningThreadTest, LifetimeTokenDeathTest, LifetimeTokenTest, ModelLoadChip, P0_11/AgentLoopPatchGolden, P0_11/AgentLoopPermissionGolden, P0_11/AgentLoopStaticPromptGolden, ProgressThrottleGolden, ReapingThreadSetTest, ScopeExitTest, ToolStreamThrottleGolden),SKIP 14 → 14(新增 无,消失 无)
- linux-x64:用例 4953 → 5035(新增 82、删除 0;新增套件 AbandonableCallTest, AbortSignalTest, AgentLoopContextGolden, AgentLoopHandoffGolden, AgentLoopHandoffGoldenDeathTest, AgentLoopLifecycleGolden, AgentLoopPatchGolden, AgentLoopPermissionGrantGolden, ComputerUseLeaseGolden, JoiningThreadGroupTest, JoiningThreadTest, LifetimeTokenDeathTest, LifetimeTokenTest, ModelLoadChip, P0_11/AgentLoopPatchGolden, P0_11/AgentLoopPermissionGolden, P0_11/AgentLoopStaticPromptGolden, ProgressThrottleGolden, ReapingThreadSetTest, ScopeExitTest, ToolStreamThrottleGolden),SKIP 16 → 16(新增 无,消失 无)

备注:ctest 模式下 5 个 `EveryTab/ManagementCenterRenderTest` 参数化用例在 `executed` 名单里以 ctest 名出现(gtest 打印的参数值含指针地址,跨运行不稳定);`tests` 名单来自 `--gtest_list_tests`,不受影响。

