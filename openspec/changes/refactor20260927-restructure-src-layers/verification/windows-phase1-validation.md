# 一期 Windows 集中验证记录

日期:2026-09-28。实施起点与当前 HEAD 均为 c3348df148d51ff799a68b17ce6af0899b5ce2fc;改动尚未形成正式提交。按 D27 在 master 连续完成 P3/P4、P6A/P6B、P7-O 的实现后集中验证。

**当前状态:实现与下列 Windows 自动化验证已完成,整体验收和交付仍未完成。** 正式 P3 发布窗口仍为 2026-09-29 06:00–18:00 Asia/Taipei。未创建 pre/post 标签,未提交或 push;P3-02 与一期总验收不能提前勾选。

## 已执行

| 项目 | 结果和证据 |
| --- | --- |
| Windows MSVC Release 全新构建及后续增量 | acecode、acecode-desktop、acecode_unit_tests 和 5 个 EXCLUDE_FROM_ALL 冒烟目标均构建成功;构建目录 build/refactor-phase1-windows |
| C++ 全量 | [gtest.json](windows-phase1/gtest.json):清单 5302 条,执行 5301 条(含 9 SKIP),5292 通过、0 失败;227.2 秒。唯一禁用项是既有 StreamingBenchmark.DISABLED_PrintsFullVsCompatibilityTimeCurve |
| 用例保持 | [清单对照](windows-phase1/gtest-inventory-review.json):相对 G0 增加 187 条,相对 P2-07 增加 185 条,均无删除;相对 P2-07 无新增或减少的 SKIP |
| Web | pnpm test 和 pnpm build 成功;源路径表征测试已跟随真正的 ToolContextFactory 所有者更新 |
| 分层与棘轮 | [layers.json](windows-phase1/layers.json):R1–R14 全部 0,exceptions_used 为空;文件大小、映射表、src/tests include 规范化、文档路径严格检查均通过 |
| 所有权一期目标 | [ownership-summary.json](windows-phase1/ownership-summary.json):raw_new/raw_delete/detach/std_thread/raw_handle 均为 0;允许的原语实现和同步借用逐项登记,热点目标通过。全仓保留的 unsafe_capture=280、delayed_injection=22 属于设计允许的一期范围外存量 |
| 闸门负向验证 | Python refactor guard suite 95 条通过;ctest -R layer_lint 通过;故意漏登记 TUI 实现时 configure 按预期拒绝,见 [负例](windows-phase1/tui-testable-negative.json) |
| trace 双配置 | [trace-validation.json](windows-phase1/trace-validation.json):ON/OFF 均重新 configure 并构建三个主目标成功;最终恢复 OFF |
| 构建目标 | 59 个 target 及依赖图保持不变;新增 1167、移除 18 个规范化元组逐项有归属,无未知增删。唯一保留源元组选项变化为 image_processor 的 SYSTEM stb include,见 [归属审查](windows-phase1/target-membership-review.json) 与 [元组差异](windows-phase1/target-tuple-diff.json) |
| M1 纯搬迁 | [机械证明](windows-phase1/p3-mechanical-proof.json):索引树之间 1003 个 R100,无其它状态;后续实现不混入 M1 |
| 行追溯 | AgentLoop 原 cpp/hpp 共 7949 行、原 TUI main 共 8101 行全部覆盖,无遗漏和重复;两份 map 的 copy 均逐字节核对 |
| PA 移除演练 | [pa-removal.json](windows-phase1/pa-removal.json):一次性源码投影中去掉 PA 目录和宿主适配器,只修改表中 7 个接触点;10 个修改 TU/头文件消费者通过 MSVC 语法编译,实际工作区保持不变。此项不是 PA 禁用产品的完整链接或运行证明 |
| 工具进程与浏览器宿主 | 独立 broker 的取消、崩溃、协议、超时回归通过;agent_browser_host_smoke 的自有 WebView 窗口通过 |
| CLI/TUI | [runtime-cli-tui.json](windows-phase1/runtime-cli-tui.json):隔离 HOME 与 loopback 模型桩下,普通/JSON 输出、-c、用法错误码通过;ConPTY/WinPTY 均完成启动、对话、模型 picker、尺寸变化和正常退出。picker 文本断言已按保存的实际终端画面核对 |
| Desktop/Web | [desktop-runtime.json](windows-phase1/desktop-runtime.json):当前构建打开独立配置的 workspace、恢复会话,界面发送新消息并收到模型桩回复,Desktop 返回 0;截图已人工查看 |
| daemon 正常关停 | [daemon-runtime.json](windows-phase1/daemon-runtime.json):前台 ConPTY 收到 Ctrl+C 后,空闲 0.402 秒、实际流式请求在途 1.108 秒退出,返回 0 且 PID 等运行文件清除 |

D6 的 DaemonShutdownSequence/SubagentHostShutdown/TuiShutdownSequence,D7 的 AgentLoopShutdown/SessionRegistryShutdown/AgentTaskQueue,D8 的 PromptConfigSnapshot 与前缀稳定性,D9 的 AbandonableCall/McpManagerAsync/ImageGenerationCancellation/RegionDetector 相关用例均包含在全量结果中。取消、在途回调、排队控制、析构顺序及异常清理由这些测试覆盖。

## 验证中修复

- 构造时传入 LOOP 策略也必须同步 WorkspaceBoundary;原有子代理继承用例已通过。
- 无确认通道的 exec 拒绝分支保留原有 PermissionRequest 未完成语义,防止新 RAII 自动多发 PermissionResolved;原有黄金序列测试未放宽。
- trace 宏的 do/while 作用域不能容纳跨宏使用的快照变量;仅将这些 trace 专用声明改用统一配置头的条件编译。
- 补齐拆分后的编译/链接依赖,修复构造注入后测试夹具的声明与析构顺序;FTXUI 事件假件明确具备焦点能力;D9 关停测试最后一步更新为 AbandonedWork。
- 测试隔离根缩短为 N:/agf3。此前长路径导致 seed 的 MAX_PATH 失败,也改变了窄屏路径行的截断结果;没有修改相关生产逻辑或放宽断言。
- 行映射的 copy 行显式写明理由,避免空 TSV 尾字段触发 whitespace 检查;修复少量提取代码的行尾空格。

## 尚未完成,不得登记为通过

- Windows 交互桌面当前不可用。原生输入冒烟返回 desktop_unavailable;真实鼠标/UIA 输入、微软拼音候选窗、Windows Terminal/conhost 的人工外观及手工清单其余项目尚未完整验证。ConPTY/WinPTY 自动运行与组件测试不能代替这些项目。
- Desktop 在 Windows 上仍沿用基线 DaemonSupervisor::stop 的 TerminateJobObject/TerminateProcess 路径。实测进程退出,但运行文件保留;这不能证明 worker 的正常析构或慢 MCP/LSP/子代理场景的 Desktop 退出验收。前台 daemon 信号退出已单独验证。本次没有把这个既有行为擅自改造成新的关停协议。
- 9 个遗留 ref 的本次最终快照迁移复核、最终任务勾选、正式窗口提交、机械提交 blame 登记、pre/post 标签和 push 尚待完成。
- macOS/Linux/Deepin、跨端 CI 及打包补验按 D27/P4-04 后移。ASan/TSan 尚未执行,本记录不声称通过。当前仅是 Windows Release 的本机证据。
