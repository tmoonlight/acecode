# ACECode 对照 opencode 差距分析与超越路线（2026-09）

对照对象：`C:\Users\shao\opencode` 本地 checkout，HEAD `a453386e9d`（2026-09），包版本 v1.18.30。
ACECode 侧：master `d603cacb`。所有论断都逐条对照两边源码核实，附文件锚点；未核实的不写。

与 [竞品调研报告_完整版_2026-08.md](竞品调研报告_完整版_2026-08.md) 的关系：那份报告对标 11 个产品但**未覆盖 opencode**；本报告只做 opencode 单点深挖，并在 §6 说明 8 月报告 Phase 1 哪些项到现在仍未落地。

---

## 0. 一页结论

**规模**：opencode 已不是一个"TUI 工具"，而是一个 33 包 monorepo（~29 万行 TS 不含测试；678 个单测文件 + 111 个 e2e；27 条 GitHub workflow），同时在跑两套运行时（legacy 单进程 + Effect 化的 V2 durable session core），并且有 Zen/Go 两条付费模型网关 + 云 console + enterprise 包。ACECode 是 ~20 万行 C++ + 13 万行前端，397 个 C++ 测试文件 + 272 个前端测试。**体量相当，但 opencode 的投入方向是"平台化 + 生态"，ACECode 是"桌面纵深 + 自主执行"。**

**ACECode 明确领先的**（§3）：Agent Browser（CDP 级）、图像生成、Remote Control（IM 通道）、Loops 定时任务、Thread Goals 自主续跑、Expert 组件、PA 内网适配、蜂群/子代理写边界、TUI 工具行渲染与 prompt-cache 前缀不变量的严谨程度、中文优先体验。opencode 没有这些，或者只有雏形（后台子代理、workspace adapter）。

**opencode 领先、ACECode 要补的**，按"对用户价值 × 实现代价"排出五个最大的洞：

| # | 洞 | 一句话 |
|---|---|---|
| 1 | **权限粒度** | ACECode 的"总是允许"是按工具名整锅端（`permissions.hpp:166`），规则表硬编码在 `main.cpp:2764`；opencode 是 tree-sitter 拆命令 → `git status *` 级前缀记忆 + 配置化 glob 规则 + `external_directory` 越界门 |
| 2 | **项目级配置 + Agent 定义** | ACECode 没有可入库的 `.acecode/config.json`（只有 hooks/skills/model_override）；opencode 的 `opencode.json` 能把 model/permission/mcp/agent/command/instructions 全部随仓库分发，Markdown 定义 agent |
| 3 | **对外接口生态** | opencode：OpenAPI 3.1 + 生成式 SDK（JS/Go）+ ACP（Zed/JetBrains/nvim）+ GitHub Action + VS Code 扩展 + npm 插件 + TUI 插件；ACECode：REST/WS 文档 + shell hooks + MCP |
| 4 | **模型层广度** | opencode：models.dev 全目录 50+ provider、Anthropic/OpenAI/Copilot/Google 等 OAuth、reasoning variants 一键切、`small_model` 分流、按模型族分发 10 套系统提示；ACECode：5 个 provider 实现、单套系统提示、effort 只在 profile 里静态存 |
| 5 | **会话存储与流转** | opencode：SQLite（drizzle）+ 序列化事件 + export/import/share + `attach` 远程 TUI；ACECode：每会话两文件（CLAUDE.md 里已记录 1428 文件冷缓存 7.3s 的痛点）、无导出导入分享 |

结论：**不需要跟 opencode 比"平台化"的全面性**（那是它烧钱换来的），但上面 1、2 两项是"团队/企业能不能用"的门槛，3 里的 OpenAPI+ACP 是"能不能被别人的工具接入"的门槛，4 里的 variants + 模型族提示是"同一个模型跑得好不好"的门槛——这四块补齐后，加上 §3 的领先项，就是真正意义上的全面超越。

---

## 1. 规模与架构速览

| 维度 | opencode | ACECode |
|---|---|---|
| 语言/栈 | TypeScript + Bun + Effect；TUI 用 opentui(Solid)；Web 用 SolidJS；Desktop 用 **Electron**（已从 Tauri 迁走，`packages/desktop/package.json`） | C++17 + FTXUI；Web 用 React 18 + Vite；Desktop 自研 webview 壳 |
| 包结构 | 33 个 workspace 包：`opencode`(CLI 主体 678 文件) / `core`(479) / `app`(596) / `tui`(204) / `ui`(247) / `console`(258, 云控制台) / `llm`(105, 原生 provider 协议) / `server` / `plugin` / `sdk` / `codemode` / `enterprise` / `slack` / `stats` … | 单仓：`src/` 40 个子目录 + `web/` + `ace-browser-bridge/` |
| 运行时 | legacy（`src/session/prompt.ts` 1631 行主循环）与 V2（`core/src/session/*`，durable inbox → runner → projector，见 `CONTEXT.md` 的术语表）双轨过渡 | 单轨：`agent_loop.cpp` 状态机 + `SessionRegistry` 多路复用 |
| 持久化 | SQLite + drizzle（`core/src/session/sql.ts`：session/message/part 表，带 cost/tokens 列） | JSONL + meta.json 每会话两文件 + writer lease |
| 客户端 | TUI / `opencode run`(带交互 footer) / Web / Desktop / ACP / GitHub Action / GitLab / VS Code 扩展 / Slack | TUI / `-p` headless / Web / Desktop / Remote Control(IM) |
| 测试 | 678 单测 + 111 e2e(Playwright) + storybook + perf 基准 | 397 C++ 测试 + 272 前端 Node 测试 |
| CI | 27 条 workflow（含 issue 去重/triage、docs locale 同步、nix、vscode 发布、storybook） | 2 条（package / test） |
| 文档 | 36 页 mdx，**24 种语言**同步 | docs/ 中文为主 + help 站 |

---

## 2. 差距清单（按层）

标注：**影响** = 对用户/团队的价值；**代价** = 在现有 C++ 架构上的实现难度（S/M/L/XL）；**优先级** = P0 门槛项 / P1 明显收益 / P2 锦上添花。

### 2.1 Agent 核心运行时

| # | opencode 做法 | ACECode 现状 | 影响 | 代价 | 优先级 |
|---|---|---|---|---|---|
| A1 | **按模型族分发系统提示**：`session/system.ts::provider()` 按 model id 选 `anthropic.txt / gpt.txt / codex.txt / gemini.txt / kimi.txt / beast.txt(gpt-4/o1/o3) / gpt-astra.txt(gpt-6) / meta.txt / trinity.txt / default.txt` 十套；工具面也随模型切：GPT 系用 `apply_patch`，其它用 `edit/write`（`tool/registry.ts:~300`） | `src/prompt/system_prompt.cpp` 单套提示（grep gpt/claude/gemini 无分支）；工具面固定 | 同一套提示在 Claude/GPT/Kimi/GLM 上表现差异很大，这是"同模型跑得好不好"的直接因素；ACECode 用户大量用国产/内网模型，更需要 | M | **P0** |
| A2 | **Agent 定义**：primary/subagent 两类，Markdown frontmatter（`~/.config/opencode/agents/*.md` / `.opencode/agents/*.md`）定义 description/model/prompt/temperature/top_p/permission/steps/color/hidden；内置 build/plan/general/explore/scout + 隐藏 compaction/title/summary；Tab 循环主 agent，`@name` 提及子 agent；`permission.task` 控制谁能派谁 | Expert 组件（`src/experts/`，Agent/Team + 头像 + 能力范围）方向类似但绑定桌面 UI；`spawn_subagent` 只有 prompt/wait/model 三参数，没有"子代理类型"概念；无 per-agent prompt/permission/temperature | 团队沉淀"审查员/文档员/安全员"这类角色是刚需；Expert 已有壳，缺的是把它接进 spawn 与权限 | M | **P0** |
| A3 | **嵌套指令按需注入**：`session/instruction.ts::resolve()` 在 `read` 工具读到文件时，向上找该文件祖先目录里的 AGENTS.md/CLAUDE.md，每条 assistant 消息只注入一次（Claude Code 同款） | `src/project_instructions/` 启动时按 cwd 祖先链 outer-first 加载；子目录里的 AGENTS.md 永远不会被看到 | monorepo 里 `packages/x/AGENTS.md` 是主流写法；不支持等于这些规则形同虚设 | S | **P0** |
| A4 | **结构化输出**：`prompt.ts` 支持 `format:{type:"json_schema"}`，注入 `StructuredOutput` 工具 + `toolChoice:"required"`，失败报 `StructuredOutputError` | 无（`src/headless`、`agent_loop.cpp` 无 json_schema） | 脚本化/流水线调用（almcli4acecode 那类）的基础能力 | S | P1 |
| A5 | **每 agent 步数上限** `steps` + `MAX_STEPS_PROMPT`（到上限后让模型总结收尾而不是硬停） | `agent_loop.max_iterations` 全局硬停 | 成本控制 + 优雅收尾 | S | P1 |
| A6 | **压缩策略**：`compaction.prune`（回合结束后异步清老工具输出，`PRUNE_PROTECT=40k` 保护近期）、`preserve_recent_tokens` 保留最近 2k–15k、可增量更新的结构化摘要模板（Objective/Work State/Next Move/Relevant Files，`core/src/session/compaction.ts`）、插件可改压缩提示 | `/compact` + cold tool archive（`add-compact-cold-tool-archive`）；摘要模板见 `compact_prompt.cpp` | 长会话稳定性；ACECode 已有骨架，缺 prune 与"增量合并旧摘要" | S | P1 |
| A7 | **task_id 续跑子代理**：`tool/task.ts` 允许传 `task_id` 让子代理在原上下文继续；后台任务完成自动通知父会话（`BACKGROUND_STARTED` 文案明确禁止父轮询） | `spawn_subagent(wait=false)` + `wait_subagent`，子会话结束即从面板移除，无续跑 | 多轮委派（"再改一下"）不用重头来 | S | P1 |
| A8 | **Session summary agent**：首轮异步生成会话摘要（`summary.ts`），侧栏/分享页用 | 只有自动标题 | 会话列表可读性 | S | P2 |
| A9 | **`invalid` 工具**：模型调了不存在的工具或参数校验失败时，由一个专门工具把错误以 tool result 形式回给模型，而不是断回合 | `resolve_model_tool_name_to_native` fail-open 透传未知名，但校验失败路径需核对 | 弱模型鲁棒性 | S | P2 |

### 2.2 工具层

| # | opencode 做法 | ACECode 现状 | 影响 | 代价 | 优先级 |
|---|---|---|---|---|---|
| T1 | **`webfetch` 工具**（URL → markdown/text，重定向提示） | 只有 `web_search`，没有取网页正文的工具；`browser_*` 能开页但重 | 查文档是最高频动作之一，没有 fetch 模型只能 `bash curl` | S | **P0** |
| T2 | **Shell 工具按 shell 出提示**：`tool/shell/prompt.ts` 对 bash / pwsh / Windows PowerShell 5.1 / cmd 各出一套描述（链式操作符差异、引号规则、`workdir` 参数替代 `cd`）；`timeout` 与 `workdir` 是显式参数 | `bash_tool` 一套描述；Windows 默认 cmd（用户决策）；system prompt 里有 PowerShell/Git Bash 指引段 | 已有一半；把指引挪进工具描述并加 `workdir` 参数即可 | S | P1 |
| T3 | **工具输出溢出统一落盘**：`tool/truncate.ts` 服务，2000 行/50KB 上限，超出写 `tool_*` 文件（7 天保留），模型可 `read`/`grep` 全量；bash 流式时按 `maxBytes*2` 滚动窗口 + 边收边写文件 | `tool_result_storage` + `preserve_full_output` 已等价（`bash_tool.cpp:618` 注释） | 已对齐 | — | — |
| T4 | **`read` 直接返回图片/PDF 附件**（jpeg/png/gif/webp/pdf → `attachments`） | `show_image` 单独工具 + `vision_analyze` 子代理；`file_read` 读图报错 | 模型自己有视觉时多一次工具往返；CLAUDE.md 已记录过这个坑 | S | P1 |
| T5 | **`question` 工具支持多题、多选、自定义答案**（`custom` 默认开，`multiple`） | `AskUserQuestion` 已有多题/推荐项 | 基本对齐；核对 multiple 与自定义输入 | S | P2 |
| T6 | **CodeMode**（实验）：模型写一段受限 JS 在沙箱里串多个 MCP 工具（`packages/codemode`），减少工具往返 | 无 | MCP 工具多时省 token；实验性 | L | P2（观望） |
| T7 | **`apply_patch`**（OpenAI 补丁格式）作为 GPT 系的编辑工具 | 无 | 只对 GPT 系有意义；与 A1 一起做 | S | P1 |
| T8 | **`edit` 的 9 级模糊匹配**（`edit.ts`: Simple/LineTrimmed/BlockAnchor/WhitespaceNormalized/IndentationFlexible/EscapeNormalized/TrimmedBoundary/ContextAware/MultiOccurrence 依次尝试，`isDisproportionateMatch` 防误配） | `file_edit_tool` 精确匹配 + CRLF 处理；需核对是否有容错层 | 弱模型编辑成功率 | S | P1 |

### 2.3 权限与安全

| # | opencode 做法 | ACECode 现状 | 影响 | 代价 | 优先级 |
|---|---|---|---|---|---|
| P1 | **命令级权限记忆**：`tool/shell.ts::collect()` 用 tree-sitter（bash + PowerShell 语法）拆出每条子命令，`permission/arity.ts` 的命令前缀词典算出"人类可理解前缀"（`git checkout` / `npm run dev` / `python script.py`），"总是允许"记的是 `git status *` 这种模式，不是整个 bash | `PermissionManager::add_session_allow(tool_name)` 按工具名整锅记（`permissions.hpp:166,182`）；"总是允许 bash" = 本会话任何命令免问 | **安全与体验双输**：用户要么每条都点，要么一次放开全部。这是 ACECode 权限系统最大的短板 | M（tree-sitter 可用 tree-sitter C 库；先用 arity 词典 + 简单分词也能拿到 80%） | **P0** |
| P2 | **配置化规则**：`permission.{read,edit,glob,grep,bash,task,skill,webfetch,websearch,external_directory,doom_loop,...}` 每键可 `allow/ask/deny` 或 `{pattern: action}` 对象，**最后匹配者胜**；`~`/`$HOME` 展开；`.env` 默认 deny；全局/项目/agent 三层合并 | 规则表硬编码（`main.cpp:2764-2768`、`tui_init.cpp:272`），config 里无 permission 段；只有 4 档模式 | 团队无法下发"禁止 `git push`、允许 `npm test`"这类策略 | M | **P0** |
| P3 | **`external_directory` 越界门**：任何工具触碰 worktree 之外路径（含 bash 里的 `cd/rm/cp/mv/cat` 参数解析）先问一次，可 `always` 记目录 | `write_root` 写边界（worktree/LOOP/继承）只管写，读越界不问；bash 守卫只管写目标 | 读越界（`~/.ssh`、其它项目）无感知 | M | P1 |
| P4 | **`doom_loop` 作为权限键**：同一工具同参数连续 3 次 → 走 `ask`，用户可放行 | `agent_loop_doom_guard.cpp` 自动处理 | 基本对齐 | — | — |
| P5 | **`--auto` 模式**：自动批准未显式 deny 的请求，与 deny 规则共存；TUI 里显示 `auto` 徽标 | `yolo` 等价但绕过全部（deny 规则除外） | 对齐；差一个"auto 与 deny 共存"的语义确认 | S | P2 |
| P6 | **Policies**（实验）：`experimental.policies` 用 `effect/action/resource` 三元组控 provider 使用；**managed config**（`/etc/opencode`、`%ProgramData%\opencode`、macOS `.mobileconfig` MDM）用户不可覆盖；**remote config** `.well-known/opencode` 组织默认值 | 无 | 企业部署门槛；8 月报告已列"企业 SSO/SIEM 不做"，但 managed config 成本很低 | S | P1 |

### 2.4 配置与团队协作

| # | opencode 做法 | ACECode 现状 | 影响 | 代价 | 优先级 |
|---|---|---|---|---|---|
| C1 | **多层配置合并**：remote → global → `OPENCODE_CONFIG` → 项目 `opencode.json(c)` → `.opencode/` 目录 → `OPENCODE_CONFIG_CONTENT` → managed；JSONC；`{env:X}` / `{file:path}` 变量替换；JSON Schema 发布供编辑器校验 | 单个 `~/.acecode/config.json` + `<cwd_hash>/model_override.json` + `.acecode/hooks.json` + `.acecode/skills`；无项目级 config、无 schema、无变量替换 | **团队协作的基础设施**：模型/权限/MCP/指令随仓库走 | M | **P0** |
| C2 | **`instructions` 数组**：glob（`.cursor/rules/*.md`、`packages/*/AGENTS.md`）+ 远程 URL（5s 超时） | `project_instructions.filenames` 只认文件名 | 复用团队已有规则文件 | S | P1 |
| C3 | **References**：`references.{alias}` 把外部目录或 **git 仓库**（自动 clone 进缓存、按 branch 刷新）挂成 `@alias`，带 description 进系统提示，自动豁免 external_directory | 无 | "对照上游实现"类任务很常见（scout 子代理专门干这个） | M | P1 |
| C4 | **Commands**：Markdown 命令支持 `$ARGUMENTS`/`$1..$n`、`` !`shell` `` 注入命令输出、`@file` 内联、`agent`/`model`/`subtask` frontmatter | `opencode_command.cpp` 已实现 `$ARGUMENTS`；需核对 `!` 注入与 `subtask` | 基本对齐 | S | P2 |
| C5 | **Formatters**：写入后按扩展名调 prettier/biome/gofmt/rustfmt/clang-format 等 20+ 种（默认关） | 无 | 代码风格一致性；有 LSP 诊断注入的骨架可复用 | S | P2 |
| C6 | **Claude Code 兼容层**：读 `~/.claude/CLAUDE.md`、`.claude/skills`，`OPENCODE_DISABLE_CLAUDE_CODE*` 开关 | 读 CLAUDE.md 与 `.agent/skills`；`.claude/skills` 需核对 | 迁移成本 | S | P2 |

### 2.5 模型 / Provider 层

| # | opencode 做法 | ACECode 现状 | 影响 | 代价 | 优先级 |
|---|---|---|---|---|---|
| M1 | **Variants**：每模型内置 reasoning 变体（Anthropic high/max、OpenAI none→xhigh、Google low/high），`ctrl+t` 一键循环，`--variant` CLI，config 可自定义/禁用变体 | `ModelReasoningOptions` 只在 saved_models profile 里静态存；无命令、无快捷键（8 月报告 Phase 1 #1 至今未做） | 同一模型"快/深"切换是高频操作 | S | **P0** |
| M2 | **Provider 广度**：models.dev 目录驱动 50+ provider（Bedrock/Vertex/Azure/Cloudflare/Ollama/LM Studio/OpenRouter/…），`@ai-sdk/*` 按需加载 + 自研 `packages/llm` 六协议原生实现（anthropic-messages / openai-chat / openai-responses / gemini / bedrock-converse / openai-compatible）；`/connect` 内置 OAuth（Anthropic 订阅、OpenAI、Copilot、Google 等） | openai-compat / anthropic / copilot / codex / grok 五个；OAuth 只有 Copilot/Grok/Codex；models.dev 只用于元数据 | ACECode 用户主战场是内网/OpenAI 兼容，广度不是第一优先；但 **Gemini 原生协议 + Bedrock** 是常见缺口 | L | P1（只补 Gemini/Bedrock） |
| M3 | **`small_model`**：标题/摘要等轻任务自动用便宜模型 | `summary_generation` 已有独立模型配置 | 对齐 | — | — |
| M4 | **Prompt cache 默认开**：`packages/llm` 的 `cache:"auto"` 在最后一个 tool 定义 / 最后 system / 最新 user 三处打断点 | `AnthropicProvider` 有 cache_control；CLAUDE.md 的前缀不变量比 opencode 讲得更透 | ACECode 更严谨 | — | — |
| M5 | **Provider 超时三件套**：`timeout` / `headerTimeout` / `chunkTimeout` 可配 | `ACECODE_OPENAI_STREAM_TIMEOUT_MS` 单值 | 内网慢模型场景需要 chunk 级超时 | S | P2 |
| M6 | **重试策略**：5 次指数退避 + `retry-after(-ms)` 头 + 可重试错误正则表（`retry.ts`） | `retry_policy.cpp` + PA rescue 链更激进 | ACECode 已更强（内网场景） | — | — |

### 2.6 会话与持久化

| # | opencode 做法 | ACECode 现状 | 影响 | 代价 | 优先级 |
|---|---|---|---|---|---|
| S1 | **SQLite 存储**：session/message/part 表，cost/tokens 列直接可聚合；`opencode db` 可直接 SQL | 每会话 `.jsonl + .meta.json`；CLAUDE.md 记录了 1428 文件冷缓存 7.3s 的列表问题及分页补丁；`state.sqlite3` 已用于 goal/loop | 列表/搜索/统计都在跟文件系统作斗争；`opencode stats` 那种跨会话成本报表在 JSONL 上做不了 | L（迁移 + 兼容旧数据） | P1 |
| S2 | **export / import / share**：`opencode export --sanitize`（脱敏）、`import <file|share-url>`、`/share` 生成公开链接（`share: manual/auto/disabled` 可由项目配置强制关闭） | Web 有 `exportSession` API；TUI 无 `/export`；无 import、无 share | 复盘、求助、跨机迁移 | S（export/import）/ L（share 需服务端） | P1（export/import）/ P2（share） |
| S3 | **undo/redo 基于 git 快照**：`/undo` 撤销最后一条 user 消息 + 文件；`/redo` 恢复；`snapshot:false` 可关（大仓库） | `/rewind` + per-user-turn checkpoints，语义等价（还多 web 时间线待接 UI） | 对齐 | — | — |
| S4 | **`attach`**：TUI 挂到远程 `serve`/`web` 实例（basic auth、mDNS 发现） | Remote Web mode 有；TUI 无法 attach 到 daemon | 一台机器跑 daemon、多终端接入 | M | P2 |
| S5 | **`stats`**：按天/项目/模型/工具聚合 token 与成本 | `/tokens` 会话内 | 依赖 S1 | S（S1 之后） | P2 |

### 2.7 TUI

| # | opencode 做法 | ACECode 现状 | 影响 | 代价 | 优先级 |
|---|---|---|---|---|---|
| U1 | **可配置键位**：`tui.json` 的 `keybinds` 140+ 个动作、leader key（`ctrl+x`）、which-key 提示面板、`none` 禁用、多绑定 | 无 keybind 机制（8 月报告 Phase 1 #3/#5 未做） | 终端用户核心诉求 | M | P1 |
| U2 | **命令面板** `ctrl+p` 搜索全部动作（80+ 条，见 `tui/src/app.tsx`） | 斜杠命令 + 排序 | 可发现性 | S | P1 |
| U3 | **外部编辑器** `/editor`（`$EDITOR`，`--wait`） | 无（Phase 1 #2 未做） | 长 prompt 编辑 | S | P1 |
| U4 | **主题**：33 套 JSON 主题 + 亮/暗自动跟随终端 + 锁定 | 主题系统 + AI 生成主题（`theme_create`）——ACECode 更强 | — | — | — |
| U5 | **时间线/跳转/分叉**：`dialog-timeline`、`dialog-fork-from-timeline`、`messages_last_user`、`session_child_first/cycle/parent` 子会话导航键 | `/fork`、`/checkpoint`；子代理只在侧栏列表 | 长会话导航 | M | P2 |
| U6 | **Prompt stash / 模型收藏与最近循环 / agent Tab 切换 / variant ctrl+t** | 无 | 高频操作效率 | S | P1（并入 M1/A2） |
| U7 | **Attention**：问题/权限/完成时播放音效 + 终端失焦时桌面通知，可自定义 sound pack | Desktop 有通知；TUI 无 | 终端用户 | S | P2 |
| U8 | **`opencode run` 带交互 footer**：非交互模式下权限/提问仍能在底部一行内应答（`cli/cmd/run/footer.*.tsx`） | `-p` 纯 headless（自动拒绝/自动应答） | 脚本与人工混用场景 | M | P2 |

### 2.8 Web / Desktop

| # | opencode 做法 | ACECode 现状 | 影响 | 代价 | 优先级 |
|---|---|---|---|---|---|
| W1 | **i18n 65 种语言**（`app/src/i18n/`）+ 文档 24 语 | 中/英 | 海外分发 | S（机翻流水线） | P2 |
| W2 | **Review tab + 行级评论**（`context/comments.tsx`：在 diff 上选行写评论，回灌给 agent） | 变更面板 + diff2html，无行评论 | 代码审查工作流 | M | P1 |
| W3 | **多服务器**：Web 可同时连多个 opencode server（`dialog-select-server`、`settings-servers`） | 多 workspace = 多 daemon，但 UI 一次只连 active | 对齐（形态不同） | — | — |
| W4 | **Desktop 自动更新 + 多窗口 + onboarding**（Electron updater） | `acecode upgrade` 显式 + macOS 自更新 | 桌面用户默认期待静默更新 | M | P2 |
| W5 | **Session 标签页拖拽、文件标签页、终端面板** | ConsoleDock + SidePanel | 对齐 | — | — |

### 2.9 集成与生态

| # | opencode 做法 | ACECode 现状 | 影响 | 代价 | 优先级 |
|---|---|---|---|---|---|
| E1 | **OpenAPI 3.1 + 生成式 SDK**：`/doc` 端点；`packages/sdk`（JS）、Go SDK、`sdk-next`（Effect 内嵌模式）；`CONTEXT.md` 对 Page/cursor/事件流有完整契约 | `docs/daemon-api.md` 手写文档；无 schema、无 SDK | 第三方（含 almcli4acecode）接入成本；有了 schema 才能生成客户端和做契约测试 | M | **P0** |
| E2 | **ACP（Agent Client Protocol）**：`opencode acp` 让 Zed / JetBrains / Avante / CodeCompanion 直接把 opencode 当 agent 用 | 无 | 一次实现，进入所有 ACP 编辑器 | M | P1 |
| E3 | **GitHub Action / GitLab**：`/opencode` 评论触发、PR 自动审查、schedule、`opencode pr <n>` 拉 PR 分支 | 无（Loops 是本地定时） | CI 里跑 agent 是团队采用路径 | M（headless 已有，主要是 action 包装 + 评论回写） | P1 |
| E4 | **VS Code 扩展**：`sdks/vscode`，`Cmd+Esc` 唤起、选区/文件引用注入、自动安装 | 无 | IDE 用户 | M | P2 |
| E5 | **npm 插件**：`chat.params/chat.headers/permission.ask/tool.execute.before|after/shell.env/tool.definition/experimental.session.compacting/...` 20+ 钩子 + 自定义工具（TS/任意语言）+ 自定义 auth 流程 + provider 钩子 + **TUI 插件**（自定义路由/组件/键位） | shell hooks（`docs/hooks.md` 8 类事件）+ MCP + connectors | 8 月报告已决定不做 TS 扩展运行时；但 **`tool.definition`（改工具描述）和 `chat.params`（改采样参数）两个钩子用 shell hooks 也能补** | S | P2 |
| E6 | **Slack 包**、**console 云控制台**、**enterprise 包** | Remote Control 覆盖 IM；无云 | 不跟 | — | — |

### 2.10 工程与交付

| # | opencode | ACECode | 建议 |
|---|---|---|---|
| Q1 | 111 个 Playwright e2e + 视觉稳定性/性能基准 | 前端 Node 单测为主 | 给 Web 加最小 e2e（登录→发消息→工具行→权限弹窗）当回归底线 |
| Q2 | 27 条 workflow：issue 去重/triage 用 agent、docs 多语同步、storybook | 2 条 | 用自己的 headless 跑 `triage.yml` 式自动化，本身就是产品演示 |
| Q3 | `AGENTS.md` + `CONTEXT.md`（领域术语表 + 关系 + 开放问题） | `CLAUDE.md` 极详尽（实现记忆） | ACECode 的 CLAUDE.md 更有用；可以借鉴 `CONTEXT.md` 的"术语表"形式给 session/goal/loop 立一份 |
| Q4 | 安装：curl 脚本 / npm / brew / choco / scoop / mise / docker / AUR / nix | npm + GitHub Releases + 自更新 | 补 brew tap / scoop / winget 三条低成本渠道 |

---

## 3. ACECode 领先项（守住并放大）

这些是 opencode **没有**或只有雏形的，也是"超越"的根基：

| 能力 | ACECode | opencode 对应物 |
|---|---|---|
| Agent Browser（CDP 级点击/填表/截图/evaluate，双宿主，开发宽松策略） | `src/tool/agent_browser/`、`src/desktop/agent_browser_*` | 无 |
| 图像生成/编辑工具 | `image_generate` | 无 |
| Remote Control（IM 通道托管会话、提问桥接、出站摘要） | `src/remote_control/` | 只有 Slack 包（云侧） |
| Loops（本地定时任务 + workspace_touched 审计） | `src/loop/` | 只能借 GitHub Action schedule |
| Thread Goals（自主续跑、budget/usage 状态机、in-turn steering） | `thread_goal_store`、`maybe_continue_goal` | 无 |
| 子代理写边界继承 + 事后 `workspace_touched` 兜底 | `fix-subagent-write-boundary` | 后台子代理仍是实验 flag |
| 蜂群/网状多代理（进行中） | `add-mesh-swarm-mode` | 无 |
| PA 内网适配（错误识别、预算学习、随机拒收救援） | `src/pa/` | 无 |
| Expert 组件（头像状态、能力范围） | `src/experts/` | agent 无 UI 实体 |
| Vision 子代理（主模型无视觉时自动路由） | `vision_analyze` | 无（读图直接给模型） |
| 记忆工具（`memory_read/write` 索引） | `src/memory/` | 无（只有 AGENTS.md） |
| Prompt-cache 前缀不变量的测试守卫 | `RequestPrefixIsByteStableAcrossIterationsInATurn` | 无同等测试 |
| 主题 AI 生成 + 图标/标题配色 + EVA 资源包 | `theme_create` | 33 套静态 JSON |
| Windows 纵深（ConPTY/winpty、conhost 兼容布局、自绘 toast、junction 坑） | 多处 | 官方建议 Windows 用 WSL |
| 会话标题跨 daemon、分页、attention 节流等性能修复 | CLAUDE.md 记录 | SQLite 天然没这些问题 |
| 中文文案、中文 IM、国产模型（Kimi/GLM/DeepSeek DSML 修复） | 全局 | 只有 kimi.txt 提示 |

---

## 4. 建议路线

### P0（门槛项，建议一个迭代内并行开 4 个 openspec change）

1. **权限 2.0**（P1 + P2）：
   - 新增 `config.permission` 段，语义照抄 opencode（键 = 工具/能力名，值 = `allow|ask|deny` 或 `{pattern: action}`，最后匹配胜，`~` 展开，`.env` 默认 deny）。
   - bash 走"命令拆分 + arity 前缀词典"，"总是允许"记 `git status *` 级模式；可先用 `tree-sitter-bash` 的 C 库（ACECode 已是 C++，比 opencode 的 wasm 更顺），或先用简单分词拿到 80%。
   - 现有 4 档模式保留为规则集的预设（`default/accept-edits/yolo/plan` = 四组默认规则），避免破坏 TUI/Web 现有交互。
2. **项目级配置**（C1）：`<worktree>/.acecode/config.json`（+ `.jsonc`）与全局合并，允许 `saved_models`（不含 key）、`permission`、`mcp_servers`、`project_instructions`、`agents`、`skills.disabled`；发布 JSON Schema 到 `assets/`；`{env:X}`/`{file:p}` 替换。
3. **Agent 定义 + variants**（A2 + M1）：Markdown agent（frontmatter：description/model/variant/prompt/permission/steps）落在 `~/.acecode/agents` 与 `.acecode/agents`；`spawn_subagent` 增 `agent` 参数；Expert 组件改为"agent 的桌面壳"而不是并行体系；`/thinking` 或 `/variant` 命令 + TUI 快捷键 + Web 下拉。
4. **模型族提示 + webfetch + 嵌套指令**（A1 + T1 + A3）：三个都是 S 级代价、高频收益，可并作一个 change。

### P1（明显收益，两个迭代）

- E1 OpenAPI schema（先从 `daemon-api.md` 反推出 `openapi.json`，加契约测试）；E2 ACP（`acecode acp`，stdio JSON-RPC，复用 SessionRegistry）；E3 GitHub Action（headless 包装 + 评论回写）。
- S1 SQLite 会话存储（迁移工具 + 双写过渡）；S2 export/import。
- T4 `file_read` 直接返回图片/PDF；T8 edit 容错层；T2 shell 工具 `workdir`/按 shell 描述；A4 结构化输出；A5 steps；A6 prune；A7 task_id 续跑。
- U1 键位配置 + U2 命令面板 + U3 外部编辑器（8 月报告 Phase 1 欠账）。
- W2 Review 行评论；P3 external_directory；P6 managed config；C2/C3 instructions glob + references。

### P2（择机）

- M2 Gemini/Bedrock 原生协议；S4 `attach`；S5 `stats`；U5/U7/U8；W1/W4；E4 VS Code；E5 两个钩子；C5 formatters；T6 CodeMode 观望。

### 依赖

```
P2 权限规则 ──→ C1 项目级配置（规则要能入库）──→ A2 agent 定义（per-agent permission）
A1 模型族提示 ──→ T7 apply_patch（GPT 系工具面）
S1 SQLite ──→ S5 stats / 会话搜索
E1 OpenAPI ──→ E2 ACP / E4 VS Code / SDK
```

---

## 5. 明确不跟

- **Effect/TS 双运行时重写**：opencode 的 V2 core 是它自己的债，不是我们的方向。
- **Zen/Go 模型网关、云 console、enterprise 按席收费**：商业形态问题，不是产品能力。
- **Electron**：ACECode 自研壳的体积/启动优势要守住；只补自动更新。
- **npm 插件运行时**：8 月报告已定；hooks + MCP + 项目级配置覆盖 90% 场景。
- **65 语 i18n**：先做英文质量，其余等有分发需求再机翻。
- **CodeMode**：等它从 experimental 出来再评估。

---

## 6. 与 2026-08 报告 Phase 1 的衔接

8 月报告 Phase 1 的五项（`/thinking`、Ctrl+G 外部编辑器、会话元命令、离线开关、`/hotkeys`）截至本次核实 **均未落地**（`src/tui/commands/` 无 thinking/effort/hotkeys 命令，`src/tui` 无 keybind 机制，`$EDITOR` 仍只用于 `/memory edit`）。本报告的 M1、U1、U3 与之重叠，建议直接合并到 P0/P1 排期，不再单独立项。

---

## 7. 核实方法

- opencode：读 `AGENTS.md`、`CONTEXT.md`、`packages/web/src/content/docs/*.mdx`（36 页）建立功能清单，再逐项进 `packages/opencode/src`、`packages/core/src`、`packages/plugin/src`、`packages/tui/src`、`packages/app/src`、`packages/llm/src` 核对实现存在且非 stub。
- ACECode：对 `src/tool/builtin_tool_registry.hpp`、`src/tui/commands/*.cpp` 的注册点、`src/permissions/permissions.hpp`、`src/config/config.hpp`、`src/prompt/system_prompt.cpp`、`docs/hooks.md` 做 grep 级核实；"无"的结论均来自 grep 零命中 + 目录结构确认。
- 未核实、只依据文档的项已在表中用"docs"或"需核对"标出。
