---
id: ai-theme-001
scope: AI 主题创建
status: done
depends-on: []
---

# objective

按已确认原型实现 AI主题卡片、预填任务、内置技能、逐步确认和本地主题交付。

# context

- openspec/changes/add-ai-theme-workflow/proposal.md
- openspec/changes/add-ai-theme-workflow/design.md
- openspec/changes/add-ai-theme-workflow/specs/ai-theme-creation/spec.md
- docs/themes.md

# path

后台：src/adapters/themes、src/base/config、src/adapters/tool/theme_create*、原生工具注册、主题路由、对应测试。
界面：ThemeCards、SettingsPage、App、ChatView、主题加载与测试、国际化。
技能：assets/seed/skills/acecode/ai-theme、seed 清单/版本及对应测试。

# verification

原生主题及工具确认链路测试；Web 测试/build；seed 验证；已确认原型对照截图；不同作者独立审查。

# delivery

三项开发以相同的已批准契约和当前工作区快照为基线，分别在隔离检出完成。原工作区已有改动只进入基线，不作为本任务提交。

| 范围 | 开发分支 | 独立验证重点 |
|---|---|---|
| 后台与原生工具 | `codex/ai-theme-backend-20260912` | 真实确认、资源版本、会话隔离、安装和离线读取 |
| Web/Desktop | `codex/ai-theme-frontend-20260912` | 不自动发送、不丢草稿、真实命令、主题加载及历史事件保护 |
| 内置 skill | `codex/ai-theme-skill-20260912` | 发现与分发、资源完整性、调用协议和中断续作 |

集成后验证以下实际连接：外观卡片 → 新任务草稿；技能清单 → `/ai-theme`；原生工具 → ThemeStore → API；新完成事件 → 主题准备 → 外观持久化。测试使用本地图片及受控人工回调，不调用计费图片生成服务。

# evidence

- 开发前主检出 `pnpm test` 全部通过，覆盖本来就存在的专家预填改动。
- OpenSpec 变更 `add-ai-theme-workflow` 严格校验通过。
- 本地原生验证的 Release 输出设为 `build/ai-theme-validation-20260912/Release`；当前运行程序来自另一构建目录。
- 自动截图验证受环境限制：浏览器连接引用缺失的服务文件；远程调试及仅截图两种无头 Edge 启动均被自动审批拒绝，仅返回 `blocked by policy`。未生成截图，未尝试绕过拒绝。
- skill 独立审查通过，7 项分发、资源与用户修改保留检查通过；实际工具返回字段另行核对通过。
- 首轮前端审查复现「安装开始后手动换色被完成事件覆盖」；首轮后台审查发现 TUI 问题 ID 丢失。两项按阻塞问题修复并复核。
- 两项阻塞问题均已修复并独立复核：后台专项 48/48、前端修复相关 25 项检查通过；真实 TUI 通道也覆盖色系、原型确认和安装。
- 集成后的 51 个开发文件与最终审查分支一致，原工作区 HEAD 和已有未提交改动保留。
- 集成后的 `pnpm test`、`pnpm build`、`git diff --check`、OpenSpec 严格校验通过；Windows `acecode_unit_tests`、`acecode` 和 `acecode-desktop` 构建通过。
- 本次新增 17 项原生测试在完整 CTest 中全部通过。完整运行发现 29 项失败/超时，基线为 27 项；9 项额外失败全部串行复跑通过，涉及既有远控测试端口竞争、种子发布波动和进程退出超时。其余失败中首项调度测试复跑仍失败，断言与基线一致；未将完整回归标为全绿，也未修改无关模块。
- 本地构建位于 `build/ai-theme-validation-20260912/Release/`；对应 `share/acecode/seed/` 和模型目录已放入构建目录，AI 主题 skill 的源文件及打包文件哈希一致。
- 实际服务启动验证也被自动审批拒绝，原因仅为 `blocked by policy`；没有启动测试 daemon，没有替换当前运行中的程序。现场启动和视觉验证仍需在允许的环境中进行。
- 完整日志、分支补丁、独立审查和复跑记录保存在 `C:/Users/shao/Documents/Codex/2026-09-09/bang/work/ai-theme-implementation/`。
- 三个本任务临时隔离检出及其开发分支已清理；当前主检出保留实现与已有改动，本地验证构建和记录保留。
