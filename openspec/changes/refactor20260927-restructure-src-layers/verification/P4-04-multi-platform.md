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

macOS 与 Linux 的修复后 CI 待确认。P4-04 完整验收保持未勾选,既有人工专项与九个旧 ref 的后续安排不变。
