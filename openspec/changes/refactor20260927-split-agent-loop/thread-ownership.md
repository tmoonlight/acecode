# 一期 AgentLoop 线程归属

本文记录已拆出的状态与加锁方向,最终验收结果另记。当前所有用例均等待一期整体实现后的 Windows 验证。

| 状态/入口 | 可进入线程 | 同步与调用边界 |
| --- | --- | --- |
| AgentTaskQueue 入队/控制回执 | worker、TUI、HTTP/API、跨会话交接 | queue 锁保护两个 deque、任务状态和回执顺序;扫描不复制队列。 |
| ActiveTurnGate steer/interject/interrupt | UI/API、worker 结束边界 | gate → queue → AbortSignal;不得从 queue 回取 gate。interject 在 gate 内先完成问题响应,再记录已接受输入。 |
| TaskHandoff | 当前会话控制调用 | source.queue → target.queue;释放两个队列锁后再发送转录/Goal 事件。 |
| ConversationHistory | worker;空闲恢复/控制调用 | 只有该类改写消息数组;空闲入口要求 !busy 且当前线程是 worker 或持有队列门,过渡期违例只告警。 |
| TrajectoryRecorder | 事件投递线程 | 独占 EventDispatcher 单槽 observer;销毁时卸载并通过 LifetimeToken 等待在途回调,不强持有宿主。 |
| WorkspaceBoundary | worker、并行只读工具 | cwd/文件夹/边界配置按值取快照;state 锁外访问 SessionManager、文件系统或其它协作对象。 |
| SessionExecSecurity | worker、并行工具、空闲配置控制 | 规则、审计接收器与拒绝反馈在 state 叶子锁下取值;审计调用与文件 IO 在锁外;SandboxRuntime 自有锁。 |
| SafeEditGuard | 写工具/后续 shell 检查 | 叶子锁只保护失败路径及时间;检查文件编码在锁外。十分钟有效期与显式绕过条件保持原样。 |
| GoalRuntime account_usage/emit_updated/emit_cleared | worker、并行只读工具经 ToolContext 回调 | cursor 叶子锁只保护 goal/thread ID、时间游标及通知去重;不跨 store、emit、回调或 queue.with_locked。 |
| GoalRuntime notify_objective_updated | TUI、daemon 控制线程 | 只写 atomic 待处理标志;worker 在下次请求前消费。 |
| AgentHookBridge apply/drain | worker、并行工具回调 | context 锁内追加或 swap;格式化、发送消息、运行 Hook 全部在锁外。 |
| AgentHookBridge Stop 状态 | worker | stop_active 跨回合保留;清理 request context 不重置 Stop 状态。 |
| PermissionHookSession | 单次同步工具执行作用域 | 请求输入按值固定,解决标志在回调前置位;正常离开补 implicit,异常展开不补新 Hook 事件。不得复制或被异步回调保留。 |

| SideQuestionService | API、每请求独立线程 | context 叶子锁内按值复制;provider/callback 在锁外,线程捕获自有共享状态;停机先抑制回调,再回收并等待线程。 |
| ActivityNarrator | worker、并行工具、配置线程 | 叶子锁保护阶段/批次/去重状态;配置与状态一次锁内读取,回调可重入查询,不持锁发送。 |
| AgentProgressEmitter | worker、并行工具 | shared_ptr 保证发射器状态寿命;750ms 节流锁外调用 narrator 与 EventDispatcher,时钟按回合取值。 |
| RetryProgressReporter | provider 回调、压缩、PA 重试 | 无可变状态;先状态回调再进度事件,三路径共享载荷出口。 |

| PromptContextCache | worker;Git 失效信号可由 UI/API 发送 | 内容键钉住技能/会话文本;切 cwd 只清 Git;跨线程只写 atomic stale。 |
| ApiRequestBuilder | worker、空闲旁路上下文预热 | capture 的注册表借用不离开调用;build 只消费值快照,Hook drain 在调用方,工具名映射每次仍经 ToolExecutor 查询。 |
| ActiveModelView | 当前模型步 | 从该步的 provider 租约取身份、能力与窗口;主请求的 prompt 与 chat 共用同一份 provider。 |
| ProviderStreamCollector::Call | provider 流回调 | 每请求独占临时输出、scanner、attempt/usage;回调仅持 LifetimeRef,返回前 revoke 等待在途并拒绝晚到事件。 |
| TurnUsageAccountant / ModelStepRecorder | worker | 先记入 worker 可达的回合用量再调用消费者;request/response/first-output 与 step 事件共享记录出口。 |

| CompactionController | worker | 独占 checkpoint 窗口链与 atomic generation;先落 checkpoint 再替换历史;provider 重试回调只捕获 LifetimeRef。 |
| ContextOverflowRecovery / RequestRecoveryState | worker | 返回决策与可选 timing 变更;通用 stage 与 PA episode 相互独立,使用请求的同一 provider 租约。 |
| PaRescueAdapter / PaRescueHost | 同步恢复调用 | 适配器固定借用服务,不回指门面;纯策略不保存宿主,所有 IO/等待/发布由宿主实施。 |

A-12 至 A-14 继续在本表追加旁路问答、进度、请求、工具批次与最终装配的线程归属。Windows 本轮不声称执行 Linux TSan。

| ToolBatchScheduler / ToolCallSlot | worker | 批次独占调用槽;只读线程按值返回 ToolCallOutcome,worker 按原始显示顺序收割。FutureJoinGuard 在显示回调异常、取结果异常和正常离开时等待余下调用;取消检查仍仅在批次边界。 |
| ToolContextFactory / ToolSessionHost | worker、已 join 的工具线程 | 工具执行作用域固定构造依赖;组合工作区、安全状态和 prompt cache,切 cwd 的清理顺序只有一份实现;工具回调捕获 LifetimeRef,作用域销毁先等待在途回调。 |
| AskQuestionBinding | 工具线程 | daemon 在调用时检查 Goal;TUI 在装配时固定 timeout/origin;回调只持 LifetimeRef,绑定结束后返回 cancelled。 |
| ToolLifecycleEvents::Stream | 工具流线程 | 单调用拥有 stream token;callback 在进度叶子锁外,结束后晚到 chunk 被抑制。 |
| PermissionAuditScope / ToolPermissionGate | 串行工具线程 | 唯一 decide 入口承接能力/doom 校验后的审批,不执行工具;审计 sandbox/category/target 固定,mode 与 exec detail 每次记录读取;Scope 不可复制或跨异步保存。 |
| ToolResultCommitter | worker | 先 budget、再按原调用顺序落 tool/post-user-prompt、最后 replacement metadata;task_complete 的 End 使用实际落盘 ID,terminal actions 只作为返回值交给回合。 |

A-12 的工具链由组合根独占的批次作用域管理,成员按依赖顺序声明,不会保存 ToolExecutionServices 参数包,也不回指 AgentLoop。批次期间绑定的可空服务指针均为构造借用,FutureJoinGuard 与 LifetimeToken 在作用域销毁前完成等待;A-13/A-14 继续把装配入口交给回合执行器与显式 services。

| TurnContext | worker 独占 | Chat 任务建立后持有,正常或恢复结束统一 reset;已计费用量跨异常展开保留。computer-use 租约在异常报告前释放;正常显式释放与析构释放两次调用保持原样。 |
| TurnRunner / RequestContextFactory | worker | 构造参数包立即拆成固定协作引用,没有门面回指;请求按步捕获同一 provider。临时配置借用在 O-10 按既定方案替换。 |
| ModelStepSink / TurnModelStepSink | 当前 provider 调用 | adapter 借用 worker 回合状态;collector 在返回前关闭并等待 Call 的 LifetimeToken,之后不会再访问 sink;用量先写回合记录再通知。 |
| TurnFinalizer | worker 收尾与恢复 | 正常、prompt hook 阻止和异常恢复共用有序步骤表,逐项保留回调、转录、busy、Done 差异;恢复接受空 TurnContext,各报告步骤分别隔离异常。 |
| UserShellTask / TurnLifecycle / AssistantOutput / ResponseRecovery | worker 同步调用 | 各对象固定构造依赖;Shell BusyCycle 不增加 BusyChanged(true),部分中断输出仍只入转录,文本拒绝判定优先于空回复重试。 |
