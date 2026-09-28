<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

见 proposal.md。后台请求 ID 由 UUID 生成，同一请求的重放保持身份；原 PR 的 requestId 重置保护与该协议一致。App 的认证分支会提前返回，所有 Hook 必须位于这些分支之前。

## Goals / Non-Goals

**Goals:** 保留同请求回答状态，同时确保所有认证路径调用相同的 Hook 序列。

**Non-Goals:** 不修改权限优先级、提问协议或侧栏布局；不增加依赖。

## Decisions

- 保留 QuestionPicker 的请求 ID 保护。仅依靠父组件 memo 无法覆盖订阅重放产生的新对象。
- 将 visibleQuestionReq 的 useMemo 移到依赖计算之后、认证提前返回之前。删除 memo 会牺牲已实现的引用稳定性，移动它可同时保留优化和 Hook 约束。
- 使用生产 App 的语法树检查组件本层 Hook 与提前返回的顺序，避免仅测试 QuestionPicker 而漏掉启动路径；另做真实 React 认证切换验证。

## Risks / Trade-offs

- 静态源码模式可能漏掉嵌套控制流 → 使用语法树检查组件本层语句，并以真实 React 启动验证补充。
- 当前 master 有已知侧栏间距断言失败 → 比较基线确认来源后，将旧 gap-0 断言对齐到已提交的 gap-2 布局，验证合并结果的完整测试。
