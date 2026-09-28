<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

OpenAI-compatible provider 已把 Codex2API 流中的 `usage` 解析为 `TokenUsage`。`AgentLoop::call_provider_and_collect` 在一次成功 model step 后发出逐 step `usage`，而 `run_agent_with_input` 负责一个用户 turn 内的多次 model step 和最终 `busy_changed` / `done` 边界。

## Goals / Non-Goals

目标是在 daemon API 的 terminal turn event 上提供可直接消费的本 turn 汇总，并保持逐 step 事件兼容。非目标包括计费金额换算、修改 Codex2API、重算历史 turn、增加独立 REST endpoint 或改变 session 累计统计。

## Decisions

在 `run_agent_with_input` 内维护局部 `TokenUsage` accumulator，只在 model step 已通过 provider error/abort 判定并完成现有 usage 入账后累加。这样内部 provider retry 的 provisional usage、失败请求和未入账中止请求不会重复计数。

计数字段按 model step 求和：`prompt_tokens`、`completion_tokens`、`total_tokens`、cache read/write 与 reasoning。`context_breakdown` 同样按类别求和，代表本 turn 所有请求的输入构成总和。

`has_data` 表示汇总是否完全来自 provider 上报：至少有一个已入账 step 且所有已入账 step 的 `has_data=true` 时为 true；混入 ACECode 估算或完全没有模型请求时为 false。

regular turn 的 terminal `busy_changed` 和紧随其后的 `done` 都携带同一 `turn_id` 与 `usage`。重复是有意的：状态型客户端可只监听 `busy_changed`，完成型客户端可只监听 `done`。原有逐 step `usage` / `model_step_finish` 不变。

## Risks / Trade-offs

同一汇总出现在两个 terminal frame 会增加少量 payload，但避免客户端必须耦合某一种终止事件。客户端若自行累加逐 step `usage`，不得再把 terminal `usage` 叠加；文档会明确 terminal usage 是摘要。

## Validation

用脚本 provider 构造同一 turn 的两次模型请求，分别返回 usage，断言 terminal `busy_changed` 与 `done` 的 `turn_id` 一致、字段求和正确且逐 step usage 仍保持两条。再覆盖 provider usage 与估算混合时 `has_data=false`。运行对应 `acecode_unit_tests` 过滤用例、OpenSpec strict validate、`git diff --check`；按用户要求不进行完整编译。
