<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

InputBar 与 ChatView 分别拒绝空输入；普通消息经 SessionClient 入队，由 AgentLoop 追加用户记录。前端存在完整 items、折叠投影、窗口化渲染和本地待发消息，后端模型历史与持久化 transcript 也并不完全相同。

## Goals / Non-Goals

目标是对真实末尾用户消息或用户主动中断的最后一条用户消息重试，并保留模型角色序列和结构化内容。不提供任意历史消息重试，不更改忙碌排队、首条消息创建和内置命令路由。

## Decisions

1. 普通重试从已加载完整 transcript 的最后一个 item 判断，绝不向前查找用户消息；排除空历史、非用户、缺少持久化身份、加载中、流式中、忙碌及只读状态。用户中断例外按决策 6 校验。点击时读取最新 store 再校验。输入含文字或附件/上下文时继续普通发送。
2. 新增 `POST /api/sessions/:id/messages/retry`，仅接受 `expected_user_message_id`。模型输入由后端原始消息恢复，不从展示文本或富文本重新展开。
3. AgentLoop 在队列锁下验证无活动和排队任务，检查模型历史末尾及完整持久化 transcript 的最后一条可见消息均为指定用户消息。跳过与前端一致的 is_meta、文件检查点、压缩检查点、回合计时、文件差异及隐藏目标上下文；不跳过可见 assistant、tool、system、error。事件观察器同时阻止尚未进入 JSONL 的错误、系统消息及部分输出。入队后在 worker 开始时再校验。
4. 使用现有 Chat worker 任务承载重试身份，在正常回合生命周期中复用原始 user 消息，不重新持久化、发出 user 事件或展开 skill/附件。复用回合身份和文件检查点。重试准备阶段不执行会改写末尾历史的前置压缩；正常模型边界压缩规则保留。
5. 在途提交继续只禁用发送动作，不禁用编辑器；回执须按 session 身份处理，不能影响切换后的会话或清除新草稿。
6. 用户主动中断作为明确的例外：回合收尾时持久化 `transcript_only` 系统记录，携带 `user_aborted: true` 和 `retry_user_message_id`。前后端只有完整 transcript 末尾为该标记时才能查找其指定的最后一条用户消息；后来出现的任何可见记录使该资格失效。插话、普通错误、同文案系统提示不构成此例外。
7. 中断标记只属于 transcript，不进入模型上下文。模型历史仍以原用户消息结尾时复用；已经有 assistant/tool 消息时复制原始结构化内容并分配新用户身份，复用正常持久化和检查点流程，不重复展开 skill。中断时已流出的正文以 `transcript_only` / `interrupted_output` assistant 消息持久化，避免重新加载时丢失输出，同时不把未完成的工具调用加入模型历史。
8. 本地点击停止只显示提示并标记中断待确认；收到后端回合结束前空输入发送仍禁用。历史恢复通过持久化中断标记恢复资格，按钮与 Enter 继续共用入口。

## Risks / Trade-offs

- 页面显示与后端状态竞态 → 前端提交时复核，后端队列门控及 worker 复核。
- 展示消息不能完整恢复原始模型输入 → 后端复用 ChatMessage 的 content、content_parts、metadata。
- 纯系统提示与错误出现在用户消息之后 → 严格拒绝，不从历史中挑选较早用户消息。
- 原生程序需要重新构建才能包含新端点 → 本任务验证源码、前端构建及 C++ 测试，不声称已更新安装包。
