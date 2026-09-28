# 一期 PA 接触点

A-11 实现后的源码登记,对应 src/layers.tsv 的 R11:pa。以下是最终 Windows 验收时进行 PA 移除演练的范围,当前尚未执行演练或构建。

| 接触文件 | PA 责任 | 移除 PA 时的修改 |
| --- | --- | --- |
| src/adapters/provider/retry_policy.cpp | PA 瞬时上游错误识别 | 删除 PA include 与特征分支,保留通用 provider 重试。 |
| src/engine/agent/model_step/active_model_view.cpp | 学习窗口、可信拒收与接受观测 | effective_window 返回声明值;观测返回空/无操作。其公开头不依赖 PA。 |
| src/engine/agent/recovery/context_overflow_recovery.hpp | 每回合 PA episode | 删除 episode 字段与 PA include,保留通用 stage/emergency 状态。 |
| src/engine/agent/recovery/context_overflow_recovery.cpp | PA 专用恢复入口与耗尽文案 | 删除 PA 分支、episode 重置和 PA 耗尽通知;保留通用三级恢复。 |
| src/engine/agent/recovery/pa_rescue_host.hpp / .cpp | 同步副作用适配器 | 随 PA 删除;其它协作类不依赖此适配器。 |
| src/engine/agent/compaction/compact.cpp | 压缩服务端错误的 PA 特征识别 | 删除 include 与 PA 判定,保留通用上下文溢出检测。 |
| src/adapters/pa/ | 纯策略、学习器、RescueDriver | 整体删除。 |

主请求只在当前迭代获取一次 provider;模型视图与恢复观测使用该次请求的租约。PA 驱动不引用 AgentLoop,通过 PaRescueHost 同步请求历史修复、等待、重试提示和通知。取消时不补恢复进度或 stream reset。成功修复先更新压缩代际,再 reset/进度/通知。手动压缩仍没有机械裁剪兜底。

最终演练在隔离的临时副本或虚拟文件视图进行,不得从用户当前 master 工作区实际删除 PA 模块;演练证据记录在统一验收报告中。
