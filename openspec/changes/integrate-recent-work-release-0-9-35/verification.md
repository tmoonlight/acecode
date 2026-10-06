# 发布验证记录

审计时间：2026-10-06，Asia/Taipei。初始 master 为 93c934a4，origin/master 为 a67efe55，主线有 3 个未推送提交、230 个待提交文件。

## Worktree 结论

| 工作区 | 来源 HEAD | 结论与依据 |
| --- | --- | --- |
| acecode.worktrees/agent-office-live | 93c934a4a791e71bcf75d3f61aa69e00c45c0770 | 已包含在本地 master；工作区干净；两项办公室变更有 C++、Web、浏览器及 Windows 原生验证记录。 |
| .claude/worktrees/cranky-jemison-14c349 | 6d45f1ce50adf419c41fa0ef91877b8122031401 | 代码与 UTF-8 测试文件已经相同；诊断文件包含后一候选的改进；共享文档和 Web smoke 测试的全部新增行在主线中。 |
| .claude/worktrees/ecstatic-dirac-7e2088 | 8d64540dadd824825055aade4284f0320356955b | 诊断实现、头文件和测试逐文件相同；Web smoke 测试全部新增行在主线中。 |

其余 42 个非主线 worktree 不在固定窗口内，没有按提交时间假定其完成。九个待迁移 ref 按 src 搬迁约定保留，不合并、不推进、不删除。旧工作区未提交改动保持原状。完整路径、分支、HEAD、状态、reflog 与原始 SHA-256 清单保存在本次发布审计附件中。

## 已执行

- pnpm test：退出码 0。
- pnpm build：通过，4512 个正则字面量兼容性扫描通过。
- 分层、文档路径、src/tests include、文件行数、所有权 strict/final、路径映射七项检查全部通过。首次未暂存时检查器不认识新增模块；暂存明确清单后消除全部缺失路径结果。
- 桌面办公室浏览器回归：40 项通过，无页面错误、无外部网络请求。

## 测试进程汇总修复

首次 Windows 全量执行 6106 项，有 1 项 MCP 配置写入失败；MCP 套件单独复查 26 项通过。随后 20 次压力重复中，第 17 次在 5 秒启动等待处失败，最后一轮通过；进程退出 1，但旧运行器误报 OK。修复运行器以每进程退出状态和 XML 共同判断，保留超时负数退出码。新增三项报告行为回归，加上重构工具全套共 102 项测试通过；既有 MCP 业务源码未修改。所有原始日志保留，最终全量复查另行记录。

## Windows 最终复核

- Release 构建 acecode、acecode-desktop、acecode_unit_tests 成功，CMake File API 的实际库归属与链接边界检查通过。
- 全量清单 6107 项，执行 6106 项，9 项按设计跳过、1 项禁用。四分片复查的并行部分零失败；串行部分的 PointerOverlay 测试在显示产品浮层之前，被 Windows.UI.Core.CoreWindow 遮挡了测试底板，导致前置命中断言失败。
- 随后用修复后的运行器复查 McpManagerAsync、ComputerUsePointerOverlay 及守护项，共 31 项通过，所有子进程退出码均为 0。MCP 写入异常在单独 26 项和本次 31 项复查中未复现；压力重复里的 5 秒启动超时保留为间歇性测试观察。没有将两次全量执行记为连续零失败。
- 滚动浏览器 27 项通过；第一次冷启动在 Vite 关闭阶段退出，诊断副本与原脚本复跑均通过。

## 后续发布门禁

集成与版本 PR、跨平台 CI、最终包和公开下载验证尚待完成，最终结果随发布审计附件交付。
