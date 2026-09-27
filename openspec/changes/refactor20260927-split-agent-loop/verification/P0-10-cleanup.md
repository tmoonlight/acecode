# P0-10 删除无调用代码的验证记录

本记录不代表任务验收完成。Linux CI、完整跨平台基线对照尚待执行；Windows 全量结果有两条在原基线上复现的失败，未增加过滤器或豁免。

## 范围

- 实现提交：`64a8ba0a`；合入母分支前置修复后的验证提交：`6d7c6e26`。
- 只修改 `src/agent_loop.cpp`、`src/agent_loop.hpp`。删除未被调用的 `run_agent`、`run_agent_with_display`、`is_hidden_goal_context_message` 和未定义、未调用的 `emit_progress_tick` 声明。
- 删除 `build_tool_context` 的三个未使用参数、`execute_tool_calls` 的 `turn_timing_status` 参数，以及 `ToolCallEntry::is_read_only` 和为它求值的只读查询。
- 全仓检索包括 `.mm` 调用方；保留函数体的非空代码逐行对照，除上述参数、字段和调用实参外没有改写控制流。修改前后的差异通过 `git diff --check`。

## Windows Release

使用 MSVC 19.38、Ninja、C++17、`BUILD_TESTING=ON` 的独立 `build-p0-10` 目录；依赖只读复用已安装的 vcpkg 库。`acecode` 和 `acecode_unit_tests` 构建成功。合入 P0-05 的测试 include 根修复后增量编译再次成功。

定向 AgentLoop 等相关测试：285 条通过。随后不加 filter 执行整个测试二进制：

| 结果 | 数量 |
|---|---:|
| 实际运行 | 5032 |
| 通过 | 5016 |
| 跳过 | 14 |
| 失败 | 2 |
| 禁用、不执行 | 1 |

全量原始记录位于验证 worktree 的 `build-p0-10/all-results.xml`；日志为本机临时目录中的 `acecode-p0-10-all-tests.log`。它们是本机产物，不作为可在其他机器复用的已通过 CI 结果。

## 失败的基线复现

在不含 P0-10 的 P0-05 完整构建二进制上，串行运行同一 filter：

```text
--gtest_filter=AgentLoopTurnSteering.InterruptStartsStructuredTurnBeforeOrdinaryQueue:ComputerUsePointerOverlay.ShowsWithoutActivationOrInterceptingHitTests
```

| 用例 | P0-10 全量及单独重跑 | P0-05 原基线单独重跑 |
|---|---|---|
| `AgentLoopTurnSteering.InterruptStartsStructuredTurnBeforeOrdinaryQueue` | 第 648 行，`wait_for_provider_turns(2, 250ms)` 为 false | 同位置、同条件失败；两次定向执行约 541/542 ms |
| `ComputerUsePointerOverlay.ShowsWithoutActivationOrInterceptingHitTests` | 第 95 行，遮盖前的命中窗口已是 `Windows.UI.Core.CoreWindow` | 同样命中该窗口，未命中测试自己的 `ACECode.PointerHitTest` 窗口 |

定向复现 XML 分别保留在 `build-p0-10/failed-only-repeat.xml` 和 P0-05 worktree 的 `build-p0-05/p0-10-baseline-failures.xml`。当前证据说明失败也存在于改动前；没有把失败记为通过，也没有据此更改验收条件。完整 G0 采集继续保留这些用例的实际结果。

## 未完成验证

- Linux CI 构建、全量测试和清单比对。
- 按最终确定的 G0 比对规则检查目标、编译单元及测试清单。
- 两条既有失败的处理方式及完整验证结果确认后，才可勾选 P0-10。
