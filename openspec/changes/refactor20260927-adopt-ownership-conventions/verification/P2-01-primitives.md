# P2-01 基础原语验证记录

实现分支为 `refactor20260927/P2-01`，独立工作树为 `C:/Users/shaoh/.codex/worktrees/refactor-raii/acecode`。下述构建、日志与临时探针均在该工作树的忽略目录 `build-p2-01*` 内；不向主仓构建目录写入文件，不终止用户进程。

## 实现与设计约束

- 新增 `JoiningThread` / `StopToken`、`ReapingThreadSet`、`LifetimeToken` / `LifetimeRef`、`ScopeExit`、`AbortSignal` 和三个 abandonable API。`JoiningThreadGroup` 从 `daemon/worker.cpp` 提取，保留公开 `threads` 集合及 `join_all()` 调用方式。
- `LifetimeRef::with` 在内部叶子锁中完成准入与在途计数，释放锁后调用业务代码。异常展开也会归还计数。`revoke()` 先禁止新准入，再等待已准入回调全部退出；等待会释放内部锁。嵌套回调内撤销自身 token 在 Debug 中断言，在 Release 中 fail-fast，避免静默降低等待保证。
- 自身线程上的 join 与 abandonable 工作启动是两处有日志的 detach 例外。R15 仅按规范路径、函数/类作用域及数量登记这两类原语实现；业务目录不新增豁免。
- abandonable 工作持有计数器租约及全部业务输入，调用者取消返回后不读取借用的 abort 标记。晚结果在锁外销毁，捕获资源销毁后才减少工作计数；等待超时不会销毁仍被 worker 使用的计数器。
- 新并发测试辅助头位于 `tests/test_support/utils/concurrency_gate.hpp`，通过测试 include 根引用。所有新增测试带中文场景说明。

## 已执行验证

Windows 环境：x64、Visual Studio 2022 Enterprise、MSVC `19.38.33133.0`，工具目录 `14.38.33130`。构建使用 C++17、Ninja、`x64-windows-static`；只读复用主仓已安装的 vcpkg 依赖，`VCPKG_MANIFEST_MODE=OFF`，没有触发依赖安装。

- 全新 `build-p2-01` 中的 `acecode_unit_tests` 初次完整构建成功，共 1000 个步骤；合入 P0-05 的测试 include 根及迁移 helper 后，正在复验集成目标。
- 独立最小测试目标直接编译新增五个测试源和 `src/utils/abandonable_call.cpp`，连接 GoogleTest。Windows Release 与 Debug 各 **24 项通过，0 失败、0 SKIP**。辅助头迁移后，两种配置均已重新构建并复验通过。
- Windows Release 曾连续运行 50 轮原语套件，全部通过；之后同步 predicate 的捕获调整和 helper 迁移均另行重编并复验。
- Linux ASan/UBSan：WSL Ubuntu、GCC `11.4.0`，`-fsanitize=address,undefined -fno-omit-frame-pointer`，Debug 配置。辅助头迁移后重新构建，**24 项通过，0 失败、0 SKIP**。
- `python scripts/layers/check_ownership.py --strict --output build-p2-01/ownership-p2-01.json` 通过，退出码 0，新增违反棘轮的条目为 0。5 条底层操作恰好由登记的原语例外覆盖：3 个线程所有者及 2 个 detach。
- R15 的未豁免 `std_thread` 从基线 104 降为 103，其余指标不变。`check_layers.py` 报告中的 R14 存量条目仍为 8，新 helper 没有任何条目。
- 运行时用例清单包含 **5057 个 GTest 名称**，即 P0-05 记录的 5033 个存量用例加本任务 24 个新用例；CTest 注册 5061 项（另有 4 个脚本测试）。本任务不修改任何存量测试文件，只新增五个测试源及一个 helper。
- `git diff --cached --check` 通过。

定向测试可通过正式项目目标重复运行：

```powershell
cmake --build build-p2-01 --target acecode_unit_tests -j2
./build-p2-01/tests/acecode_unit_tests.exe `
  '--gtest_filter=ScopeExitTest.*:AbortSignalTest.*:JoiningThreadTest.*:JoiningThreadGroupTest.*:ReapingThreadSetTest.*:LifetimeTokenTest.*:LifetimeTokenDeathTest.*:AbandonableCallTest.*' `
  '--gtest_output=xml:build-p2-01/primitive-results.xml'
```

## TSan 与平台限制

TSan 已编译但**未通过有效运行验证**。本机 WSL 内核 `6.18.33.2`、glibc `2.35-0ubuntu3.13` 与 GCC 11 的 `libtsan.so.0` 组合，在正常启动时于 `main` 前报告 `FATAL: ThreadSanitizer: unexpected memory mapping`。

仅对测试子进程使用 `setarch x86_64 -R` 后可启动，但标准库 `condition_variable::wait_for` 路径报告 mutex double-lock，随后 sanitizer 自身的死锁检测器失败。相同报告已用一个只包含标准库 mutex、condition_variable、thread、promise 的独立 23 行程序复现，不包含 ACECode 头文件。探针及输出保存在 `build-p2-01-tsan/stdlib_cv_probe.{cpp,log}`。没有通过屏蔽报告来把 TSan 标记为通过。

Windows 全量运行结果尚待补记；Linux 完整项目构建、远端 Linux CI 与其它平台 CI 未在本工作树执行，不能由本记录中的 focused sanitizer 测试替代。
