# 一期 Windows 集中验证记录

日期:2026-09-28–29。实施起点与当前 HEAD 均为 c3348df148d51ff799a68b17ce6af0899b5ce2fc;改动尚未形成正式提交。按 D27 在 master 连续完成 P3/P4、P6A/P6B、P7-O 的实现后集中验证。

**当前状态:实现与下列 Windows 自动化验证已完成,整体验收和交付仍未完成。** 用户于 2026-09-29 明确取消等待 06:00,立即交付。正在生成提交并通过受保护 master 所需的 PR 合入;标签、push 和最终任务状态以实际完成结果为准。

## 已执行

| 项目 | 结果和证据 |
| --- | --- |
| Windows MSVC Release 全新构建及后续增量 | acecode、acecode-desktop、acecode_unit_tests 和 5 个 EXCLUDE_FROM_ALL 冒烟目标均构建成功;构建目录 build/refactor-phase1-windows |
| C++ 全量 | [gtest.json](windows-phase1/gtest.json):清单 5304 条,执行 5303 条(含 9 SKIP),5294 通过、0 失败;228.5 秒。唯一禁用项是既有 StreamingBenchmark.DISABLED_PrintsFullVsCompatibilityTimeCurve |
| 用例保持 | [清单对照](windows-phase1/gtest-inventory-review.json):相对 G0 增加 189 条,相对 P2-07 增加 187 条,均无删除;相对 P2-07 无新增或减少的 SKIP |
| Web | pnpm test 和 pnpm build 成功;源路径表征测试已跟随真正的 ToolContextFactory 所有者更新 |
| 分层与棘轮 | [layers.json](windows-phase1/layers.json):R1–R14 全部 0,exceptions_used 为空;文件大小、映射表、src/tests include 规范化、文档路径严格检查均通过 |
| 所有权一期目标 | [ownership-summary.json](windows-phase1/ownership-summary.json):raw_new/raw_delete/detach/std_thread/raw_handle 均为 0;允许的原语实现和同步借用逐项登记,热点目标通过。全仓保留的 unsafe_capture=280、delayed_injection=22 属于设计允许的一期范围外存量 |
| 闸门负向验证 | Python refactor guard suite 96 条通过;ctest -R layer_lint 通过;故意漏登记 TUI 实现时 configure 按预期拒绝,见 [负例](windows-phase1/tui-testable-negative.json) |
| trace 双配置 | [trace-validation.json](windows-phase1/trace-validation.json):ON/OFF 均重新 configure 并构建三个主目标成功;最终恢复 OFF |
| 构建目标 | 59 个 target 及依赖图保持不变;新增 1168、移除 18 个规范化元组逐项有归属,无未知增删。唯一保留源元组选项变化为 image_processor 的 SYSTEM stb include,见 [归属审查](windows-phase1/target-membership-review.json) 与 [元组差异](windows-phase1/target-tuple-diff.json) |
| M1 纯搬迁 | [机械证明](windows-phase1/p3-mechanical-proof.json):索引树之间 1003 个 R100,无其它状态;后续实现不混入 M1 |
| 行追溯 | AgentLoop 原 cpp/hpp 共 7949 行、原 TUI main 共 8101 行全部覆盖,无遗漏和重复;两份 map 的 copy 均逐字节核对 |
| PA 移除演练 | [pa-removal.json](windows-phase1/pa-removal.json):一次性源码投影中去掉 PA 目录和宿主适配器,只修改表中 7 个接触点;10 个修改 TU/头文件消费者通过 MSVC 语法编译,实际工作区保持不变。此项不是 PA 禁用产品的完整链接或运行证明 |
| 工具进程与浏览器宿主 | 独立 broker 的取消、崩溃、协议、超时回归通过;agent_browser_host_smoke 的自有 WebView 窗口通过 |
| CLI/TUI | [runtime-cli-tui.json](windows-phase1/runtime-cli-tui.json):隔离 HOME 与 loopback 模型桩下,普通/JSON 输出、-c、用法错误码通过;ConPTY/WinPTY 均完成启动、对话、模型 picker、尺寸变化和正常退出。picker 文本断言已按保存的实际终端画面核对 |
| Desktop/Web | [desktop-runtime.json](windows-phase1/desktop-runtime.json):当前构建打开独立配置的 workspace、恢复会话,界面发送新消息并收到模型桩回复,Desktop 返回 0,空闲退出 0.14 秒,运行文件与写者租约均清除;截图已查看 |
| daemon 正常关停 | [daemon-runtime.json](windows-phase1/daemon-runtime.json):前台 ConPTY 收到 Ctrl+C 后,空闲 0.402 秒、实际流式请求在途 1.108 秒退出,返回 0 且 PID 等运行文件清除 |
| Desktop 在途 MCP | [desktop-mcp-runtime.json](windows-phase1/desktop-mcp-runtime.json):真实本地 MCP 调用已进入阻塞桩后关闭 Desktop,2.18 秒正常退出;工具先取消、再关闭 MCP,会话保存 Aborted 结果,无残留运行文件或写者租约 |
| Windows 原生输入 | [native-smoke.json](windows-phase1/native-smoke.json):direct/helper 均返回 0,覆盖自有窗口的 UIA、Unicode 输入、鼠标、拖放、双显示器坐标和焦点失效拒绝 |
| 实窗终端 | [Windows Terminal](windows-phase1/windows-terminal-runtime.json) 和 [conhost](windows-phase1/conhost-runtime.json) 均完成启动、对话、模型选择、Unicode 中文输入、退出 0 和 resume 提示;已查看各自截图。Unicode 输入验证不等于微软拼音候选窗验证 |
| TUI 子会话运行中退出 | [tui-subagent-runtime.json](windows-phase1/tui-subagent-runtime.json):ConPTY/WinPTY 下各执行 /exit 与空闲态双击 Ctrl+C,4 组均退出 0、打印 resume 提示并清除写者租约。首次夹具过早把回复首块当成父回合空闲,已改为等待真实空闲状态后复核 Ctrl+C |
| Windows ASan | [asan-summary.json](windows-phase1/asan-summary.json)、[用例明细](windows-phase1/asan-gtest.json):构建成功,489 条所有权/取消/回调/析构/WebServer 相关用例通过,0 SKIP、0 失败,51.5 秒;日志无 ASan 错误报告。使用下述受限配置 |
| 旧分支保留 | [P3-03 复核](P3-03-final-legacy-refs.md):九个旧 ref/worktree 均保持原状;用户已决定迁移另行安排,不作为本次交付阻塞项 |

D6 的 DaemonShutdownSequence/SubagentHostShutdown/TuiShutdownSequence,D7 的 AgentLoopShutdown/SessionRegistryShutdown/AgentTaskQueue,D8 的 PromptConfigSnapshot 与前缀稳定性,D9 的 AbandonableCall/McpManagerAsync/ImageGenerationCancellation/RegionDetector 相关用例均包含在全量结果中。取消、在途回调、排队控制、析构顺序及异常清理由这些测试覆盖。

## 验证中修复

- 构造时传入 LOOP 策略也必须同步 WorkspaceBoundary;原有子代理继承用例已通过。
- 无确认通道的 exec 拒绝分支保留原有 PermissionRequest 未完成语义,防止新 RAII 自动多发 PermissionResolved;原有黄金序列测试未放宽。
- trace 宏的 do/while 作用域不能容纳跨宏使用的快照变量;仅将这些 trace 专用声明改用统一配置头的条件编译。
- 补齐拆分后的编译/链接依赖,修复构造注入后测试夹具的声明与析构顺序;FTXUI 事件假件明确具备焦点能力;D9 关停测试最后一步更新为 AbandonedWork。
- 测试隔离根缩短为 N:/agf4。此前长路径导致 seed 的 MAX_PATH 失败,也改变了窄屏路径行的截断结果;没有修改相关生产逻辑或放宽断言。
- 行映射的 copy 行显式写明理由,避免空 TSV 尾字段触发 whitespace 检查;修复少量提取代码的行尾空格。

- 最终旧分支演练发现根目录 main.cpp 不在映射与源码 include 处理范围,现已补齐并用真实 Git 三方补丁用例验证;AGENTS.md/CLAUDE.md 的映射 SHA-256 同步更新。

- Desktop 旧 Windows stop 路径直接强制终止 daemon,跳过有序关停。按既定 D6 补齐进程寿命绑定的停止事件,保留 5 秒强制兜底;56 条定向用例和真实慢 MCP 退出均通过,最终全量结果已包含此修复。

## 范围限制与交付待办

- ASan 范围限制:首次链接遇到未插桩第三方静态库的 STL 注解不匹配,独立验收构建按微软文档统一禁用 string/vector 容器注解,保留 /fsanitize=address 并启用 alloc_dealloc_mismatch。此配置不覆盖容器已分配容量内的越界;第三方预构建库未重新插桩。依据:[MSVC 容器注解与静态库配置](https://learn.microsoft.com/en-us/cpp/sanitizers/error-container-overflow?view=msvc-170)。
- 用户已于 2026-09-29 确认:人工专项后补,本次按 Windows 自动化及已完成实测交付。微软拼音候选窗、真实 Copilot/IM 账号与其余未执行人工项仍保留未验收,详见 [人工清单覆盖审查](windows-phase1-manual-coverage.md);不阻塞本次主线交付。
- 最终任务勾选、提交、机械提交 blame 登记、pre/post 标签和 push 尚待完成。
- 已核实远端 master 的活动 ruleset 要求 PR 合入,不要求状态检查或批准票。本地实施仍在 master;交付阶段通过远端 PR 保留全部机械/行为提交,不 squash。为落实 D27 不跑跨端 CI,交付顶端与合并提交使用 [skip ci],不修改工作流或保护规则。依据:[GitHub 跳过工作流](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/skip-workflow-runs)。
- macOS/Linux/Deepin、跨端 CI、TSan 及打包补验按 D27/P4-04 后移。本记录不声称跨平台验证通过。
