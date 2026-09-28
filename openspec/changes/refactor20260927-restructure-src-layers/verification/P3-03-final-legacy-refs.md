# 九个旧 ref 的最终布局迁移复核

执行日期:2026-09-29。主仓库保持 master,起点 c3348df148d51ff799a68b17ce6af0899b5ce2fc。为遵守 D27 尚不创建实施提交,使用一次性测试仓库中的未发布快照提交表示已暂存的最终源树,再运行 migrate(..., layout="final", projection=False)。这不是正式合入,也没有推进或重写任何原 ref。

[完整清单](windows-phase1/legacy-refs/manifest.json) 记录每个源 SHA、补丁文件、三方基础对象包、冲突与语义事项。9 次演练全部执行,0 个可直接机械合入。原 HEAD、index、工作区差异和九个原 ref 均未变化。第一次复核发现历史根目录入口遗漏,已补齐 main.cpp 映射及 include 改写并通过真实 Git 回归用例;下表基于修正后结果。

| 原 ref(origin/) | 当前障碍 | 人工迁移责任落点 |
| --- | --- | --- |
| chatview_optimize | main/AgentLoop、聊天渲染、构建和表征测试冲突;旧 ask_overlay_input.hpp 在主线已无对应文件 | 当前 TUI 的 chat/render/input/overlays 模块;工具输入需单独核对 |
| claude/ai-image-sharing-tool-8ewihy | CLI 入口、工具注册、show_image、TUI 状态共 4 处冲突 | 图像附件回到 session/image 与 tool;TUI 调用在 composer/render |
| claude/debug-acecode-crash-XNWgr | Desktop main/WebHost、Web server、Web 测试和前端共 7 处冲突 | 保留现有 O-08 WS/import 寿命修复,逐项比较旧崩溃补丁 |
| claude/desktop-skill-error-handling-r3j2h8 | 说明文档和 3 个前端/i18n 文件冲突;底层路径可机械转换 | skills 与 Web 调用点保持新边界;生成 catalog 需从源脚本重建 |
| claude/fix-desktop-context-compression-tkOxK | 旧 AgentLoop cpp/hpp 的压缩改动已不能放回当前门面 | agent/compaction、recovery、model_step 三处按实际修改意图分配 |
| claude/multi-model-config-design-wWhDX | CLI 入口、builtin_commands、slash_dropdown 共 3 处冲突 | 模型配置归 config;TUI 命令/启动/composer 各自接入 |
| codex/add-self-session-control | 涉及 ToolResult/ToolSummary 抽取,且 pinned_sessions_handler.cpp 已删除,工具拒绝自动复活 | 会话控制归 session_host;按新工具结果类型接入,不可恢复旧 handler |
| docs/askuserquestion-dual-entry-design | 涉及 ToolResult/ToolSummary 抽取,且 tui_init.cpp 已删除,工具拒绝自动复活 | Ask 服务、question policy 与 TUI overlay/composer 分别承接 |
| jb | AgentLoop、system_prompt、SessionRegistry 和设置搜索共 7 处冲突;还有路径函数语义抽取 | 提示词快照与 registry 新寿命约定必须保留;路径函数归 base/utils |

这些 ref 全部保留,没有标记弃用。表中的落点是基于本次结构拆分的迁移指导,不是已经完成的特性移植。自动补丁的冲突数为 0 也不代表成功:例如缺失目标文件会让 git apply 提前失败,以 success/返回码和 reason 为准。

2026-09-29 用户明确选择“保留原状，迁移另行安排”。因此九个 ref 及原工作区继续保留,本次不执行人工移植、不标记弃用、不合入独有特性。P3-03 保持未完成并移交后续安排,不再作为本次主线一期交付的前置条件。后续确需合入时仍须处理上述语义冲突、通过 --check 和对应验证。
