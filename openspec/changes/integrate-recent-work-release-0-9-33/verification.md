# 0.9.33 发布验证

## 整合范围

时间窗口：2026-10-03 00:38:52 至 2026-10-04 发布执行期间（Asia/Taipei）。初始 master / origin/master 同为 119de292，0 个落后或超前提交；45 个注册 worktree 中，近期脏改动来自主工作区及两个 Claude worktree。

- `cranky-jemison-14c349`：HEAD 12f31951 已在 master。网络错误 UTF-8、清单解析和检查用例已被 329faf92 集成；补入剩余安装、解压、路径、任务接口的 UTF-8 处理和对应测试。
- `ecstatic-dirac-7e2088`：HEAD 329faf92 已在 master。补入升级诊断不可用说明去重及单元/HTTP 测试。
- 主工作区：新对话思考深度声明恢复、中文 Markdown 强调、蜂群通知不重复生成已处理耗时摘要及相关文档测试。
- 近期 worktree 提交均已在 master。reflog 快照 `7b17f3d73` 的斜杠命令及会话 UI 改动由 329faf92 和后续蜂群提交覆盖；较新 AGENTS.md 命名和蜂群命令行为保留，不重新应用旧快照。
- 源 worktree 保留，合并前核对源文件 SHA-256；三方合并冲突位于升级响应序列化和 HTTP 测试追加位置，保留公共 UTF-8 安全序列化及双方所有测试。
- 原先严格 CI 失败：`builtin_commands.cpp` 2069 行，超过既有上限 2060。`/tasks` 实现迁到同层 `tasks_command.cpp`，注册顺序与行为保持，未放宽上限或所有权规则。

## 已完成检查

- 前端全部修复在同一工作区通过 `pnpm test`、`pnpm build`；真实 ChatView 浏览器 fixture 验证宽屏/窄屏思考深度显示、选择传递、后台同步失败及草稿保留。
- 分层/尺寸和最终所有权严格检查通过；OpenSpec 三个相关变更严格验证通过。
- `tests/scripts/verify_release_assets_test.py`：6 项通过。
- 重构工具自测 99 项通过；include、文档路径和迁移映射严格检查通过。
- Windows Release 单测目标构建成功；旧 MSVC 结构化诊断曾输出 MSB8084 编码错误，关闭 `UseStructuredOutput` 后增量复核构建通过，无编译错误。285 项升级、UTF-8、蜂群/子会话、内置命令、种子及请求前缀守护测试通过，0 跳过、0 失败；使用短路径隔离 profile，未改变用户运行中的桌面实例。
- 整合提交 e470bfde 已推送到 `codex/release-v0.9.33` 并建立 PR #101；该提交的分层、前端与 macOS 安装器 CI 已通过。最终版本提交另由 CI 验证，不把前一提交结果冒充最终结果。
- `assets/models_dev/MANIFEST.json` 快照为 2026-09-29，满足 30 天要求；未改模型目录或图标输入。
- 对比最近正式版 v0.9.30 到整合前 HEAD：`assets/seed`、`src/domain/skills/default_skill_seeder.cpp`、`tests/skills` 无差异；seed 与 manifest 均为 2026-09-28.1，无新种子迁移。

## 待完成发布验证

本机原生测试、最终提交 CI、完整签名公证产物、更新镜像及公网校验结果在后续发布执行记录中补齐；本文件不预先声明发布成功。
