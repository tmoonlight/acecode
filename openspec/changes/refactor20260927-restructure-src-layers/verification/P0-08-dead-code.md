# P0-08 死代码清理验证

本记录区分已执行的本机检查与尚未完成的验收。实现提交为 `ec39ef19`；
随后依次合入主线 `7621b4f6`、`8acc3863`，当前验证源码为 `76fee055`。
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

## 测试清单与待办验收

`acecode_unit_tests.exe --gtest_list_tests` 返回 0。与 P0-07 原始源码
`3ddb7d433f280181292e5cd2eede612b17cc8b42` 的实际清单逐名比较：
5033 项完全相同（5032 active、1 disabled），没有新增或删除。
原始输出及二进制 SHA-256 见
[P0-08-windows-inventory-diff.json](P0-08-windows-inventory-diff.json)。
这里仅验证注册清单，不代表实际执行、SKIP 或测试结果。

全量测试正在等待独占测试窗口，避免与其他工作树争用 GUI 和固定端口。
另外尚未完成 Linux/macOS/Deepin 全新构建、Windows 微软拼音候选窗的
删除前后手工对照，以及远端 CI。不能据本机编译和清单结果勾选本任务。
