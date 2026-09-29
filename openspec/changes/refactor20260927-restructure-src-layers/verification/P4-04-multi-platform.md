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
