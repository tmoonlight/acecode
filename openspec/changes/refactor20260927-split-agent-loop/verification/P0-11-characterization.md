# P0-11 表征测试验证记录

状态:测试独立阶段已验证,任务仍实施中、尚未验收,不勾选 tasks.md。
基于认领提交 `92eb910b`;工作分支 `refactor20260927/P0-11`。
当前仅新增 5 个测试源文件、1 个 helper 与本文档,生产代码零修改。
第 8 组及节流时钟用例尚待热点释放,不能据此声称 11 组全部完成。

## 覆盖边界

| 任务组 | 新增测试 | 钉住的断言 |
|---|---|---|
| 1 权限门 | `characterization_permissions_test.cpp` | mode/tool/rule/goal/headless/hook/用户选择的交叉决策;结果原文、执行次数、审计字段和顺序、PermissionRequest/Resolved、会话授权 |
| 2 apply_patch | `characterization_patch_test.cpp` | Add/Update/Delete/Move 的后续路径保护、整补丁一次确认、Plan 全路径条件 |
| 3 PreToolUse | `characterization_lifecycle_test.cpp` | 被拒调用无实时 ToolStart/ToolEnd,轨迹各一条且原因为 pre_tool_hook |
| 4 并行只读 | 同上 | 用有限条件变量门强制反序完成;每个 Pre/execute/Post 配对,展示与模型历史仍按提交顺序 |
| 5 run_shell | 同上 | hook 改写、完整长输出、实时 BusyChanged 仅 false、回调 true/false、JSONL `!cmd` + tool_result |
| 6 Stop 残留 | 同上 | Stop 请求续跑后遇迭代上限;下一用户回合 active=true,后续 Stop 才复位 |
| 7 swarm | 同上 | swarm 仅在指定回合启用,下一普通回合上下文清除且静态 system 不变 |
| 8 computer-use 租约 | 待热点释放后补入 | 正常收尾、hook 早退、异常与 abort 的释放边界;无真实桌面访问 |
| 9 静态 system | `characterization_context_test.cpp` | GPT/其它模型 × 自动/手工压缩,压缩 initial context 的 system 与主请求逐字节一致 |
| 10 压缩历史 | 同上 | 主请求使用模型别名,压缩仍使用原生 tool_calls;工具结果正文不被改写 |
| 11 AB-BA | `characterization_handoff_test.cpp` | 单向成功/失败释放 source 门;双向竞争在独立进程中有界复现现有僵持;借用临时目录不取得清理所有权 |

`compact_messages` 本身会做历史合法化与旧文本工具调用清洗。因此第 10 组
不声称压缩请求完全未经清洗,而是验证 AgentLoop 不提前套用
`model_facing_provider_messages` 的工具名改写。

AB-BA 是明确保留的已知问题,测试成功表示“复现现有僵持”,不表示不存在死锁。
GTest threadsafe death-test 重启子进程,两个回调在各自 source.queue 已持有后
通过有限屏障再请求 target.queue。协调门最多等 5 秒,两个 future 各观察
200 毫秒后子进程退出。未形成指定竞争或交接意外完成都返回非零。
仅 `testing::internal::InDeathTestChild()` 确认的子进程读取继承目录,
显式借用并核验它位于系统临时根、具有本探针前缀与匹配身份文件。
父进程忽略外部预设的 `ACECODE_P011_ABBA_ROOT`,只创建并清理自己的唯一目录;
删除前再次核验位置与身份。子进程从不删除目录,不留下永久等待的测试线程。

另一个锁定的现状:需要审批的 bash 没有确认通道且 PermissionRequest hook
未决定时,返回 `no_confirmation_channel` 拒绝,只出现 PermissionRequest,
没有 PermissionResolved。新增专门用例断言该序列,不在表征阶段补事件。

## 待协调的最小生产边界

P0-10 尚未验收,`agent_loop.hpp/.cpp` 严格热点仍归其所有。
当前只改测试与本文档,不抢占生产文件。

1. **750/500 ms 时钟**:750 ms 取时位于 `run_agent_with_input` 的
   `emit_agent_progress`;500 ms 取时位于 `execute_tool_calls` 的流输出闭包。
   现有实现直接调用 `std::chrono::steady_clock::now()`。
   待热点释放后提供每 loop 的可注入 steady-clock 函数;只允许空闲时配置,
   worker 启动一次操作时按值捕获快照。默认仍调用相同 steady clock,
   保持 750/500 ms 常量、首帧、force、key 变化、coalesce 和输出内容不变。
   测试用自有原子时钟验证阈值前一毫秒/恰到阈值,不依赖 sleep。
2. **租约释放**:现有 `abort`、正常收尾及 `DesktopTurnLease` 析构直接调用
   `computer_use::release_session`。Windows/macOS 的真实实现会启动桌面 helper,
   Linux 不支持。提议仅给该函数边界提供注入,默认函数保持原样,测试用自有
   fake lease 记录 owner、释放次数及其相对 on_turn_finished/Done 的顺序。
   此提议待主代理确认和热点释放后实施,不改变全局 computer-use runtime。

## 隔离与验证安排

- 新 helper 放 `tests/test_support/agent_loop/`,测试按项目 CMake glob 自动发现。
- 用临时 HOME/USERPROFILE、headless、RunMode 和工具名映射作用域守卫恢复原值。
- worker 先 shutdown,再退订并销毁 hooks/session;Done 后排 control fence,
  避免主线程在 worker 的 RAII 尾声之前读断言。
- 定向编译/执行后完成用例清单差分;既有 AgentLoop/Hook 扩大验证另行排队。
- Windows 使用当前 worktree 的全新 `build-p0-11`,MSVC x64 C++17,并行度 3。
  只读复用 vcpkg 安装依赖,不在主仓 build 构建。
- 全量测试向主任务排队,不与 P0-07/P0-08 的全量窗口争用资源。

## 本地实测证据

平台:Windows x64,MSVC 19.38.33133,Release,C++17。构建目录 `build-p0-11`,
`cmake --build build-p0-11 --target acecode_unit_tests -j3` 成功;
最终执行的是正常 CMake 链接的 `tests/acecode_unit_tests.exe`。
所有新文件少于 1000 行。

| 验证 | 结果 | 构建目录中的证据 |
|---|---|---|
| `--gtest_filter=*Golden*` | 48/48 通过,0 skip,0 fail,9 suites,20.510 秒 | `new-golden-results.log`、`new-golden-results.xml` |
| AB-BA 与并行读重复 | 每组 10 次,20/20 通过;外部 inherited-env 哨兵保留 | `concurrency-repeat.log`、`concurrency-repeat.json` |
| Windows 用例清单 | 5033 → 5081,仅新增 48,删除 0、改名 0 | `gtest-inventory-p0-11.json`、`inventory-comparison.json` |
| R15 strict | exit 0,新增生产所有权违规 0;脚本仅检查 src,测试捕获另经代码审查 | `ownership-final.json` |
| R14 | 保持既有 8 项,本次文件无违规 | `layers.json` |
| 范围/格式 | 生产文件无差异,`git diff --check` 通过 | Git diff |

清单基线来自 P0-07 固定 `3ddb7d433f280181292e5cd2eede612b17cc8b42` 的
Windows GTest 实际名称清单,5032 active + 1 disabled;从该提交至认领基线没有测试
与 agent_loop 源码变动。新增清单与定向 XML 的 48 个名称逐一对应。

XML SHA-256:
`7f424d07e643840737101a2ace34f2f612c39e6a4099a7cb0cbd4e0b7f0a3043`。
重复日志 SHA-256:
`642ff37d85df1c8625d1d4c746d0a01bc466c88ea54e58af47fefca2ba3ab818`。
清单比较 SHA-256:
`1bae4c32b5891472d41f955dfd2e2e430ca5544a7c1a44e1292eb167c75c586d`。

首轮临时定向链接执行 47 项时发现 6 项测试夹具错误:规则 pattern 没有转换 Windows
反斜线,以及补丁审计期望路径没有 lexically_normal。只修正测试路径构造,
正式二进制运行全部通过。此校准不对应生产行为变更。

尚未完成:computer-use 租约组、750/500 ms 可注入时钟用例、Windows 全量、
Linux CI。本提交涉及 design §7 的 1–3、6、20–28、31、37;
34 的节流与 20 的租约释放仍保留待验状态。
