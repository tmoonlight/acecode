# 长时间运行内存增长排查（2026-09-10）

## 结论与证据范围

用户报告同事隔夜看到约 17 GB 内存，桌面端与 acecode.exe 各约 8–9 GB。本次从 master 的 `fdc77c1a` 开始检查，项目版本为 0.9.13；尚未取得同事安装包的确切版本、异常时进程树、堆快照或当晚任务记录，因此不能将任一代码缺陷认定为这次事故的唯一原因。

已经确认并修复五类内存放大或无界保留问题。长代码流和无换行命令输出尤其值得核对：前者能在桌面渲染端留下数百个完整历史版本，后者会在后端持续积累长行并反复广播越来越大的快照。两者均有明确代码或合成测试证据。

桌面的 WebView 渲染进程与原生 acecode-desktop.exe 应分别统计。当前机器进程内存不能用于证明同事机器的历史状态。

## 已接入的修复

| 问题 | 原有行为 | 本次修复 |
| --- | --- | --- |
| Markdown 高亮缓存 | 最多 512 条，但每条保留完整源码键和 HTML；未闭合代码块每次更新都会新增缓存 | 未闭合代码块继续高亮但不缓存；闭合块同时限制 512 条和估算 8 MiB 字符存储 |
| 巨大单行进度 | `current_line` 无字节上限，重复进入 ToolUpdate | 每行只保留 UTF-8 完整的最近 4 KiB；完整命令结果不由进度预览决定 |
| 事件重放缓存 | 仅限制 1024 条，单条可能含巨大的完整输出 | 增加每会话估算 8 MiB 总预算；超大单条实时交付但不进入重放缓存 |
| 订阅异常后的队列堆积 | live/replay 回调抛错可令 delivering/catching_up 停留在阻塞状态，后续事件持续入队 | 隔离单次回调异常，继续清空队列与其他订阅；空回调不注册 |
| 大结果广播与诊断副本 | 工具大结果在预算处理前发送给客户端；Provider 正常流的原始诊断全文一直累积 | PostToolUse 之后、ToolEnd/结果消息/trajectory 之前沿用原有结果阈值落盘并发送路径和预览；正常 HTTP 流诊断样本限制 64 KiB |

工具结果仍沿用 Bash 30,000 字节、其他工具 50,000 字节阈值，以及 2,000 字节预览。保留原有持久化替换审计记录、结构化文件 diff、metadata 和附件。现有 Hook 接收到的输入未因这一步改变。

Provider 的非 2xx 错误正文还用于额度和重试分类，因此继续保留原有完整错误处理语义。新增大 429 错误回归确认 `insufficient_quota` / `credit_balance_too_low` 不会因截断变成无限重试。诊断裁剪也保留 UTF-8 边界，避免中文响应在 JSON 序列化时报错。

## 仍需决定的部分

### 大命令输出在运行期间落盘

`BashOutputCapture` 已实现并通过测试：100 KiB 以内保留原文，超过后边运行边写入文件；完整文件可逐字节回读，存储失败有明确错误，内存不会退回无限积累。

**尚未接入 Bash 实际执行路径。** 原因是它会让自定义 PostToolUse Hook 收到“文件路径＋预览”，而不是超大输出全文。已向用户询问该兼容性取舍，未收到答复前不改变 Hook 契约。因此，运行中 `bash_tool.cpp` 的 `full_output` 仍可能随输出量增长；本次已先限制进度快照和执行完成后的多端副本。

建议采用文件路径＋预览；有全文需求的 Hook 从文件读取。完整日志保留，但其容量压力转移到磁盘。

### 会话驻留、轨迹面板及终端传输

- `SessionRegistry` 已加载会话持续驻留；归档不直接释放 AgentLoop，定时任务每次新建的会话也可能不断积累。建议另做“已结束、无订阅、无待办的会话自动卸载并可从磁盘恢复”，需要协调序号重放、子任务及 goal 状态。
- 轨迹面板持续保留请求记录，搜索索引还序列化完整前后 prompt。建议按需加载明细和限制热缓存；需要决定全内容搜索如何覆盖未加载历史。
- Crow WebSocket 发送队列和 xterm 输入队列缺少端到端消费确认。高速终端输出或慢客户端可能积压，需要设计流控；不能仅用 scrollback 行数代替传输队列限制。
- HTTP 错误正文、没有结束分隔符的协议帧，以及存储失败时保留的原始工具结果仍保留现有行为。若统一设置容量硬上限，需要明确超限终止和错误恢复策略。

这些部分未偷偷采用丢弃历史、强制终止任务或修改外部依赖的方式处理。

## 验证

- C++ Release `acecode_unit_tests` 构建成功。
- 相关 20 个测试套件共 **187 项通过**，覆盖真实 AgentLoop 大结果事件与持久化审计、事件顺序和异常恢复、64 MiB 命令采集器、UTF-8 进度、Provider 正常流及大额度错误。
- `pnpm test` 全量通过；`pnpm build` 通过，包括产物正则兼容检查。
- Markdown 堆实验：同一个 256 KiB 代码块做 512 次增量渲染，丢弃返回值并强制 GC。旧实现净保留约 **128.53 MiB**；修复后未闭合块约 **0.35 MiB**，闭合变体约 **2.11 MiB**。这是特定合成输入结果，不是整应用内存保证。
- OpenSpec 严格校验与 `git diff --check` 通过。
- 未进行同事机器隔夜复现，未发布安装包。当前运行中的 Desktop/daemon 未被替换或重启；原有侧栏未提交修改保留。

测试日志位于 `build/memory-fix-affected-tests.log`、`build/memory-fix-web-tests.log`、`build/memory-fix-web-build.log`，测试可执行文件位于 `build/memory-fix-20260910/Release/acecode_unit_tests.exe`。

## 主要源码入口

- `web/src/lib/markdown.js`、`markdownHighlightCache.js`
- `src/utils/stream_processing.hpp`
- `src/session/event_dispatcher.cpp`
- `src/agent/agent_loop.cpp`、`src/session/tool_result_storage.cpp`
- `src/provider/stream_diagnostic_capture.hpp` 及三个 Provider 的 streaming 路径
- 待接入组件：`src/tool/bash_output_capture.cpp`

OpenSpec：`openspec/changes/bound-long-running-memory`。
