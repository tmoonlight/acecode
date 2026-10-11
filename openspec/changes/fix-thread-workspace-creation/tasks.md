## 1. 实现与回归

- [x] 1.1 补齐创建目标解析、无工作区归属、可见注册及返回身份，并通过 ThreadTools 创建与失败清理回归。
- [x] 1.2 接通跨工作区续聊与目标恢复，验证无工作区续聊、磁盘恢复和同 ID 防误投递测试。
- [x] 1.3 更新工具 schema、描述与协议文档，验证旧参数兼容及非法目标类型拒绝。

## 2. 集成验收

- [x] 2.1 复用 Windows 构建目录完成单元测试编译，运行 thread/workspace/no-workspace 相关测试，记录真实结果。
- [x] 2.2 通过 OpenSpec 严格校验与 git diff --check，审查文件范围并区分源码验证和运行版本。

- [x] 2.3 发布整合时将纯标题/事件格式化辅助函数拆到独立实现文件，保持 thread_service.cpp 不超过既有行数上限；通过分层检查和线程相关原生回归。

## 验收记录

- 2026-10-11：复用 `build/refactor-phase1-windows`，通过 `scripts/dev_windows_env.bat` 加载 MSVC 环境后增量构建 `acecode_unit_tests` 成功。
- `ThreadCreation.*:ThreadTools.*:HeadlessSessionScope.*:WorkspaceRegistry*.*` 及五项 WebServerHttp 会话/工作区回归共 69/69 通过。其中新增 10 项创建/续聊测试及 1 项 AI 创建后侧栏 HTTP 列表测试。
- 新增 HTTP 回归验证 `/api/sessions?scope=no-workspace` 在新会话活跃及退出后都返回正确标题和无工作区归属。
- 测试报告：`C:/Users/shao/AppData/Local/Temp/acecode-thread-workspace-final-tests.xml`；构建目录是验证副本，未替换 `build/Release` 中正在运行的 Desktop/daemon。
- 不修改现有用户会话；原不可见历史会话没有迁移。未提交、未发布。
- `openspec validate fix-thread-workspace-creation --strict` 与本次文件的 `git diff --check` 均通过；144 个已记录基线的无关修改文件内容未变。

- 2026-10-12 发布整合验收：`thread_service.cpp` 1188 行，未超过既有 1211 行上限；分层、体积、所有权、路径和映射严格检查全部通过。Windows MinSizeRel 三目标构建成功，线程/技能/配置/通道/升级等 1382 项原生回归中 1380 项通过、2 项外部环境用例按设计跳过，无失败。
