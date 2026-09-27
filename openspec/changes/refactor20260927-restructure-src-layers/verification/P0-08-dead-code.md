# P0-08 死代码清理验证

本记录区分已执行的本机检查与尚未完成的验收。实现提交为 `ec39ef19`；
随后依次合入主线 `7621b4f6`、`8acc3863`，当前 C++/tests/CMake 源码为
`76fee055`；全量测试启动时分支为仅补充验证记录的 `121ef616`。
任务保持未勾选。

## 删除范围

删除前已逐项检索声明、调用、include 与文档引用。生产改动仅删除任务中
列明的未接入 TUI 副本、空翻译单元、旧 daemon supervisor 和无用 IME
实现。`desktop/daemon_supervisor.*`、`tui/message_render_cache.hpp`、
`web/handlers/pinned_sessions_handler.hpp` 均保留。`src/main.cpp` 的
226 行变化全为删除，保留行的原始字节未改写。

26 个文件共增加 6 行、删除 2138 行；新增行仅在帮助资料和测试目录说明。
帮助资料从 authored JSON 输入重新生成并核对；没有修改 React 代码。

## Windows 全新构建与目标快照

工作树：`C:/Users/shaoh/.codex/worktrees/refactor-integration/acecode`。
全新 `build-p0-08` 使用 MSVC 19.38、Ninja、Release、x64-windows-static、
`BUILD_TESTING=ON`、`ACECODE_BUILD_DESKTOP=ON`。vcpkg 已安装依赖只读复用，
`VCPKG_MANIFEST_INSTALL=OFF`；没有复用主仓的 acecode 构建输出。

全新构建 1006/1006 成功，包含 acecode、acecode-desktop 与单测。
合入测试路径护栏后重编译 488/488 成功。随后合入的 `8acc3863` 相对
`7621b4f6` 不改变 src、tests 或 CMake 输入。

File API 完整比较见 [P0-08-windows-target-diff.json](P0-08-windows-target-diff.json)。
对照为 P0-04 改动前 `12052b9e` 的独立 fresh configure；其生产 C++、
测试与 CMake 内容和原始 G0 源码 `3ddb7d43` 相同。

| 项目 | 清理前 | 清理后 |
|---|---:|---:|
| target 数量（含 EXCLUDE_FROM_ALL） | 59 | 59 |
| 源码、语言、编译定义/选项元组 | 3535 | 3506 |
| 新增 target / 元组 | — | 0 / 0 |

29 项删除逐项对应 10 个已删 `.cpp`、9 个已删头文件，以及两个已删
testable 翻译单元在五个消费目标中的 10 个生成对象引用。保留文件的
目标归属、编译定义、语言和编译选项没有变化。

这份差异是观测结果，尚不是已批准的 G0 例外。全局验收要求逐元组相等，
本任务又明确要求删除这些编译单元；原始 G0 保留，等待明确差异判定规则。

## 测试清单与完整执行

`acecode_unit_tests.exe --gtest_list_tests` 返回 0。与 P0-07 原始源码
`3ddb7d433f280181292e5cd2eede612b17cc8b42` 的实际清单逐名比较：
5033 项完全相同（5032 active、1 disabled），没有新增或删除。
原始输出及二进制 SHA-256 见
[P0-08-windows-inventory-diff.json](P0-08-windows-inventory-diff.json)。
这份清单记录本身不代表实际执行；完整执行证据见
[P0-08-windows-full-test.json](P0-08-windows-full-test.json)。

2026-09-26 20:00:10–20:08:49 UTC 在独占测试窗口执行全部单测，耗时
519.406 秒，进程退出码为 1。使用独占分配的短 Windows 盘根 HOME
（14 字符）和其内部 TEMP（19 字符），隔离键与 G0 工具相同。
运行命令没有 filter；原始 XML 与日志 SHA-256 已核对，XML 逐名覆盖
5033 个注册用例、实际执行全部 5032 个非禁用用例。

| 结果 | 数量 |
|---|---:|
| PASS | 5017 |
| SKIP | 13 |
| FAIL | 2 |
| DISABLED（不执行） | 1 |

两项失败为 `AgentLoopTurnSteering.InterruptStartsStructuredTurnBeforeOrdinaryQueue`
与 `ComputerUsePointerOverlay.ShowsWithoutActivationOrInterceptingHitTests`。
它们也出现在原始 `3ddb7d43` 的此前观测中；新的完整 G0 尚在重新采集，
不能把此前受限观测当作正式验收依据，也不能将本次结果称为全绿。

此前使用较长隔离 HOME/TEMP 的一次全量执行留下 9 项失败、14 项 SKIP。
其中 6 项种子资源测试与 1 项设置页渲染测试在本次完整复验中通过。
原始 `3ddb7d43` 与 P0-08 在相同长度 HOME/TEMP 下的成对诊断也复现了
相同的路径长度/布局问题；详细清单及原始文件校验见
[P0-08-windows-environment-comparison.json](P0-08-windows-environment-comparison.json)。
这组定向诊断仅用于解释环境影响，没有用它替代全量测试。

为与原始采集保持相同的可选桥接依赖条件，在本工作树的
`assets/channels/whatsapp` 执行 `npm ci --no-audit --no-fund` 成功，
锁文件未改。`ChannelBridge.RealBridgeStartsWithoutConnectingAnAccount`
由缺依赖 SKIP 恢复为 PASS。本次 13 个 SKIP 名与此前原始源码观测逐项相同；
正式对照仍待新的完整 G0。

原始 XML 的 128 个非 UTF-8 字节仅位于诊断字段；保留原始文件，以显式
字节偏移标记生成分析副本，并检查结构与测试身份没有变化。原始日志可能
包含用户配置诊断，只保留在本机；提交的证据仅含用例身份、结果与校验值。

## 待办验收

仍需新的完整 G0 对照、Linux/macOS/Deepin 全新构建、Windows 微软拼音
候选窗删除前后手工对照、远端 CI，以及明确的 G0 已删除元组判定规则。
任务要求单测全绿，本次仍有两项失败；本任务保持未勾选。
