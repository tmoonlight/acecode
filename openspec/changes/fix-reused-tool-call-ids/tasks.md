## 1. 共享历史修复

- [x] 1.1 区分同批重复与跨消息复用，完成确定性 ID 重映射、原始 ID 预留和结果配对；通过 SessionHistoryRecovery 回归验证连续复用、用户续接、乱序结果、系统穿插、缺失结果、碰撞、幂等及输入不变。

## 2. 提供者协议验证

- [x] 2.1 增加 OpenAI 和 Anthropic 请求回归，验证两次同 ID 调用都保留且最终真实结果正确配对；通过两个提供者的相关测试套件。

- [x] 2.2 更新依赖旧丢弃行为的图片反馈测试，并覆盖同 ID 后续调用缺失结果时不复用旧图片；通过 ToolImageFeedback 套件。

## 3. Windows 综合验收

- [x] 3.1 完成 Windows 增量构建、相关会话/请求/压缩测试及 fast 回归；记录实际执行结果和未执行项。
- [x] 3.2 完成 OpenSpec 严格验证与 git diff --check，核对变更仅包含本次文件，记录运行实例是否已切换。

## 验证记录

- 旧代码运行新增回归：8 个用例全部失败，确认覆盖原始故障。
- 修复后相关套件：134 个用例通过，0 失败（SessionHistoryRecovery、OpenAI、Anthropic、CompactCheckpoint、ThreadRepair、AgentLoopCompactEvents、AgentLoopTermination 及守护用例）。
- 使用实际会话 `20260930-173329-a764` 离线回放第 6、12、14 行前缀：分别保留 2、3、3 组调用和结果，末条均为 tool，正文及元数据保持、重复投影幂等；原文件 SHA-256 不变，未发送网络请求。

- 首次 fast 回归仅 `ToolImageFeedback.RecoveredDuplicateCallBatchCannotReplayEarlierImages` 失败：旧测试预期直接丢弃跨消息同 ID，现改为验证重映射后图片仍按真实结果分组；新增缺失结果不能复用旧图片的保护。

- 图片反馈专项与守护用例：24 个通过，0 失败；保留每轮图片、重映射标签对应关系及缺失结果不串图均通过。

- 第二次 fast（6 分片）中本次相关用例全部通过，唯一失败是 AgentLoopTaskHandoff 的 5 秒等待超时；该套件单分片复测连同守护共 11 个用例通过。继续以 3 分片核验整体调度稳定性。

- 最终 fast（3 分片）：清单 5315，执行 4707，4698 通过、9 跳过、0 失败；快速档排除的 607 个用例及 1 个默认禁用用例未执行，不宣称完整全量通过。
- Windows Release 增量构建 `acecode_unit_tests`、`acecode` 成功；`build/refactor-phase1-windows/acecode.exe --version` 输出 `acecode v0.9.30`。
- `openspec validate fix-reused-tool-call-ids --strict`、`git diff --check` 通过；本次仅变更共享历史修复、对应回归及本变更文档。其他已有工作区修改保留。
- 当前运行实例未替换或重启；实际上游网络请求与 Desktop 人工交互未执行，使用新构建启动后修复生效。
