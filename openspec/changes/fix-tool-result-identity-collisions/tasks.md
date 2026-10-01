## 1. 调用身份与输出隔离

- [x] 1.1 执行前分配无冲突 ID，并以 ConversationHistory 及 AgentLoop 回归验证连续复用、同批重复、恢复、压缩后隔离和事件配对。
- [x] 1.2 持久化使用独立文件名，并以 ToolResultStorage 回归验证同 ID 不同输出及清理后文件名冲突不会覆盖旧内容。
- [x] 1.3 旧历史重复 ID 不再批量应用歧义替换，以恢复回归验证正文和元数据保留。

## 2. 综合验证

- [x] 2.1 完成 Windows 增量构建、相关回归及 fast 测试，记录实际结果与未执行项。
- [x] 2.2 完成 OpenSpec 严格验证、git diff --check 和范围审查，明确可执行文件位置与运行实例切换状态。

## 验证记录

- 旧实现上新增的三个现场回归全部失败；守护用例通过。
- 修复后定向回归 119 个用例全部通过，含 ConversationHistory、ToolResultStorage、AgentLoopToolResultStorage、生命周期事件、历史修复、图片反馈、结束条件与流收集。
- Windows Release 增量构建 acecode、acecode_unit_tests 成功；新程序 --version 返回 acecode v0.9.30。
- 最终 fast（3 分片，短路径 C:/t/aid1001）：清单 5367，执行 4759，通过 4750、跳过 9、失败 0；快速档排除 607 个用例，另有 1 个默认禁用用例未执行，不宣称全量通过。
- 第一次整体回归 7 个失败来自隔离目录过长：6 个 seed 文件复制出现 Windows 路径超限，1 个设置页测试的配置路径挤占标签宽度。仅改短测试目录后整体通过，没有为这些环境失败修改产品代码。
- OpenSpec 严格验证、git diff --check、文件大小静态检查通过。
- 全仓分层扫描仍有 18 个 R8，指向此前已有修改引用的未跟踪新头文件；所有权扫描在未由本次修改的 agent_transcript.cpp 报告两个捕获（R15 和 R15-final 各记一项）。本次修改文件无命中；未修改其他工作以消除这些存量报告。
- 本次范围为 4 个生产文件、3 个测试文件及本变更文档；原有 dirty work 保留，未提交或推送。
- 新构建：build/refactor-phase1-windows/acecode.exe。当前运行实例仍来自 build/Release/acecode.exe，未替换或重启；用户切换到新构建后生效。实际在线模型与 Desktop 人工交互未执行，验证使用真实 AgentLoop 配合模拟提供者，不重放现场业务命令。
- 原始会话文件未改动；此前已被覆盖而未留存的工具正文不能从预览可靠还原。
