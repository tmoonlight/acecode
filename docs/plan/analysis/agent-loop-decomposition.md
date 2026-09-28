# AgentLoop 拆分分析

## 目标

将 `src/agent/agent_loop.cpp` 的多回合运行时职责拆为可定位、可独立验证的实现单元，降低单文件耦合；不改变 `AgentLoop` 的公开 API、会话 JSONL、事件协议、工具权限语义或 provider 请求语义。

本分析的范围是代码组织和运行时内部边界，不引入产品行为。任何行为变更必须另建 OpenSpec change。

## 已核实的现状

- `src/agent/agent_loop.cpp` 目前约 6.7k 行，`src/agent/agent_loop.hpp` 约 1k 行。
- `AgentLoop` 同时拥有：worker 队列、canonical transcript、turn steering、工作区/安全边界、请求构建、压缩、provider streaming/recovery、工具生命周期、目标运行时、hooks、事件和 side-chat。
- `CMakeLists.txt` 通过 `file(GLOB_RECURSE ... src/*.cpp)` 收录生产和 `acecode_testable` 源码；新增 `src/agent/*.cpp` 不需要手工修改 source list。
- `src/loop/` 已是 scheduled-loop 子系统，不能承载 agent turn runtime。
- 关键回归套件已按职责分布在 `tests/agent/`；拆分应保留测试路径和测试名，避免将「文件移动」伪装为行为变化。

## 目录决策

新增 `src/agent/`，但**第一阶段不移动** `src/agent/agent_loop.hpp`：

```text
src/
  agent_loop.hpp                 // 暂时保留：公开 API、私有状态、组合根
  agent_loop.cpp                 // 逐批收敛为构造/公共入口/worker/turn 编排
  agent/
    agent_runtime_types.hpp      // 仅跨实现单元共享的内部 DTO
    agent_request_builder.cpp    // AgentLoop 成员函数定义
    agent_compaction.cpp         // AgentLoop 成员函数定义
    agent_tool_runner.cpp        // AgentLoop 成员函数定义
    agent_turn_runtime.cpp       // queue、steering、goal、side-chat
    agent_provider_runner.cpp    // streaming、retry、PA rescue、preamble
    agent_events.cpp             // 仅无业务决策的 event/persistence helpers
    agent_workspace_runtime.cpp  // cwd、folders、write root、sandbox/audit
```

第一阶段的 `.cpp` 均直接包含 `../agent_loop.hpp`，以 `AgentLoop::method` 形式迁移定义。这样仍可访问私有成员，避免为了「拆文件」而引入一个暴露所有状态的 `AgentLoop&` 服务定位器。

当至少三个职责已稳定迁移后，才评估将头文件移动为 `src/agent/agent_loop.hpp` 并在 `src/agent/agent_loop.hpp` 留兼容 forwarding header。该动作会波及大量 include，应是单独任务，不与行为迁移混做。

## 不可破坏的契约

| 契约 | 证据与保护方式 |
|---|---|
| prompt-cache 前缀逐字节稳定 | `build_api_request_messages()` 每个模型步骤执行；运行 `agent_loop_termination_test` 与 `system_prompt_test` 的稳定性用例。 |
| canonical transcript 只有一个 owner | `messages_` 继续仅由 `AgentLoop` 持有和提交给 `SessionManager`；模块不得维护第二份模型历史。 |
| worker 顺序与锁协议不变 | `queue_mu_`、`active_turn_mu_`、`active_provider_mu_` 的持有顺序和 worker dispatch 保持原样；先搬定义，后抽组件。 |
| 工具结果先持久化再以替换后的内容发出 | 保留 delivery replacement、attachments、task_complete 的 deferred ToolEnd 顺序。 |
| 写边界不随 Yolo 失效 | worktree / LOOP / inherited write root 和附加工作区目录继续同时参与校验。 |
| TUI callbacks 与 daemon events 同步兼容 | 不改变 `AgentCallbacks` 或 `SessionEventKind` payload；所有原有事件顺序由现有测试锁定。 |
| compact checkpoint 可 resume | 迁移后仍由同一 `SessionManager` 写 checkpoint、replacement history、notice 和 task suggestion。 |

## 状态所有权

```text
AgentLoop (唯一组合根与状态机 owner)
  owns canonical state
    messages_, session_manager_, permissions_, callbacks_, events_
    busy_, abort_requested_, active_turn_*, worker queues

  owns runtime configuration
    cwd_, PathValidator, sandbox runtime/rules, workspace folders
    model/context/prompt caches, tool preamble configuration

  delegates algorithms, never state ownership
    request builder      -> provider request bundle
    compaction           -> compact/repair outcome
    provider runner      -> streamed response / retry classification
    tool runner          -> canonical tool results / terminal outcome
    goal runtime         -> goal accounting and steering decisions
```

跨模块返回值应是窄 DTO；不得让新组件持有 `AgentLoop&` 后自行修改多份生命周期状态。第一阶段用成员函数移动；第二阶段若抽类，应按下列边界注入 callbacks。

| 边界 | 输入 | 输出 | 禁止持有 |
|---|---|---|---|
| 请求构建 | transcript 快照、稳定配置、工具定义 | `ApiRequestBundle` | provider 流、工具执行、队列锁 |
| 压缩 | transcript、initial context、provider snapshot | `CompactOutcome` / replacement history | 用户提交队列、完整 tool context |
| provider 执行 | immutable request bundle、stream callbacks、abort flag | `ProviderStepResult` | `messages_` 的写权限 |
| 工具执行 | tool calls、构造好的 `ToolContext`、lifecycle callbacks | `ToolRunOutcome` | provider retry、worker queue |
| turn steering | expected turn id、input | receipt / drained input | provider 或 session I/O |

## 迁移批次

### 0. 基线与搬迁规则

先建立本分析和任务链。每一迁移 commit 只允许：移动成员函数定义、最小 include 调整、对应测试补强；不得顺带格式化其他区域或重写事件 payload。

### 1. Request 与 workspace runtime

迁移 `set_cwd`、workspace folders、write root、sandbox/audit 以及 `build_api_request_messages` 的纯构建路径。两者必须分文件，但同一批串行完成，因为 request builder 读取 workspace/sandbox 派生状态。

保留在 `AgentLoop`：配置 setter、缓存字段、最终调用时机。

### 2. Compaction

迁移 context estimate、initial context、checkpoint、auto/manual compact、mechanical fallback 和 PA window observation。`run_compact()` 可以暂留编排层，具体 compact 调用转入文件。

### 3. Tool runner

迁移 `build_tool_context()` 和 `execute_tool_calls()`，包括 read-only 并发、serial writes、permission/sandbox/audit、hooks、streaming progress、attachment materialization、canonical result storage 与 terminal tool 后处理。

这是行数收益最大的一批，但只在 workspace runtime 已稳定后进行，防止复制路径校验。

### 4. Turn runtime

迁移 worker queue、submit/retry/control、turn steering、goal runtime、side-chat。`worker_main()` 可以先留在根文件，只将数据结构和 helper 定义移走；避免第一轮触碰任务调度和锁顺序。

### 5. Provider runner 与 events

迁移 streaming 收集、usage、retry、preamble、provider-error/PA rescue；随后迁移不含业务判断的 event 与 persistence encoding helpers。`run_agent_with_input()` 最终保留为唯一模型步骤编排器。

## 最终组合根

拆分后 `run_agent_with_input()` 只保留：

```text
prepare turn
  -> compact if required
  -> build request
  -> run provider step
  -> classify recovery / retry
  -> persist text reply OR run tools
  -> apply turn termination / steering
  -> emit terminal lifecycle
```

`AgentLoop` 的理想规模是约 1,000–1,500 行，但行数不是验收条件；每批验收标准是行为和协议不变。

## 集成枚举

必须以真实实现验证下列连接，不能只测试新 helper：

1. workspace 更新 -> sandbox writable roots -> request Environment -> tool path validation；
2. request builder -> context usage estimate -> provider stream Usage -> goal accounting；
3. provider tool call -> preamble -> assistant tool-call persistence -> ToolStart/ToolEnd -> canonical tool result；
4. context overflow -> PA observation/repair -> rebuilt request -> transcript checkpoint/replacement；
5. active-turn steer/interject -> tool question resolution -> tool result -> next provider request；
6. task_complete / session terminal action -> final ToolEnd -> BusyChanged -> Done -> post-turn action。

## 任务顺序与并行性

所有实现任务都修改 `src/agent/agent_loop.cpp` 或 `src/agent/agent_loop.hpp`，路径重叠且有严格状态依赖，因此必须串行。禁止为了并行创建会产生冲突的 worktree 分支。

| 顺序 | 任务 | 前置 |
|---|---|---|
| 1 | `agent-loop-split-001`：workspace/request 定义迁移 | 无 |
| 2 | `agent-loop-split-002`：compaction 定义迁移 | 001 |
| 3 | `agent-loop-split-003`：tool runner 定义迁移 | 001、002 |
| 4 | `agent-loop-split-004`：queue/steering/goal/side-chat 迁移 | 003 |
| 5 | `agent-loop-split-005`：provider/events 与编排收敛 | 004 |

## 验证矩阵

| 批次 | 最小回归 |
|---|---|
| 001 | `agent_loop_metadata_injection_test`、`agent_loop_plan_mode_test`、`agent_loop_workspace_folders_test`、`shell_write_guard_test`、请求前缀稳定性测试 |
| 002 | `agent_loop_compact_events_test`、`agent_loop_pa_rescue_test`、tool result storage 的预算交界用例 |
| 003 | `agent_loop_tool_lifecycle_events_test`、`agent_loop_ask_user_question_parallel_test`、`agent_loop_computer_use_scheduling_test`、`shell_write_guard_test` |
| 004 | `agent_loop_turn_steering_test`、`agent_loop_question_interjection_test`、`agent_loop_goal_test`、`agent_loop_worker_recovery_test` |
| 005 | `agent_loop_termination_test`、`agent_loop_empty_response_integration_test`、`agent_loop_tool_preamble_test`、`agent_loop_trajectory_test`，加前四批的完整 AgentLoop suite |

所有批次还必须运行 `git diff --check`；首次新增 `src/agent/*.cpp` 后额外确认其进入 `acecode_testable` 编译单元。
