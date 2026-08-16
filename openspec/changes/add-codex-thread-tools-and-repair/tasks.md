## 1. 会话域共用能力

- [x] 1.1 将 pinned session 持久化 helper 从 Web 层抽到 session 域，并保持现有文件格式与 Web 行为
- [x] 1.2 实现 thread 域服务的工作区解析、创建、fork、列举、turn 读取、消息投递和多目标等待
- [x] 1.3 实现 title、pin、archive 与级联硬删除，并清理搜索索引及关联持久化状态

## 2. Codex 对齐工具

- [x] 2.1 为十个 Codex 对齐动作实现独立工具定义、camelCase 参数解析和有界 JSON 返回
- [x] 2.2 在 daemon、TUI 与 headless 运行时注册 thread 工具，并覆盖工具可发现性和关键调用测试

## 3. 会话修复

- [x] 3.1 增加带诊断的有效 history 重建与确定性逻辑组裁剪引擎，并覆盖损坏记录、tool pair 和当前输入保留测试
- [x] 3.2 实现活跃/非活跃 thread 的追加式 repair checkpoint 与 `repair_thread` 工具，并覆盖 no-change、成功和不可恢复结果
- [x] 3.3 在 AgentLoop 接入明确上下文溢出的有限自动恢复、partial-output 禁止重放和 emergency request profile

## 4. 文档与验证

- [x] 4.1 更新 daemon API/会话文档与 `docs/ACECode 待办.md`，记录工具清单、删除语义和修复边界
- [x] 4.2 运行 OpenSpec 严格校验、相关 C++ 单测、代码质量与构建检查，并修复发现的问题
