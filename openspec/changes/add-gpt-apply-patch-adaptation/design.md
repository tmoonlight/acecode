<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

# add-gpt-apply-patch-adaptation — Design

## Context

opencode 在两处按模型分发(HEAD a453386e9d):

- `packages/opencode/src/tool/registry.ts`:`usePatch = modelID.includes("gpt-") && !modelID.includes("oss") && !modelID.includes("gpt-4")`;`apply_patch` 只在 usePatch 时给,`edit` / `write` 只在 !usePatch 时给。
- `packages/opencode/src/session/system.ts::provider()`:`gpt-4 / o1 / o3` → beast,`gpt` 且 `codex` → codex.txt,其余 `gpt` → gpt.txt,`gemini-` → gemini,`claude` → anthropic,默认 default。
- 补丁语言实现在 `packages/opencode/src/patch/index.ts`(移植自 codex-rs `apply-patch`):解析 → `computeReplacements`(`seekSequence` 四级容错)→ 倒序应用。

ACECode 的约束与 opencode 不同,决定了几条偏离:

- 工具注册在启动时一次性完成(main.cpp / worker.cpp / headless_runner.cpp 各一次),`/model` 中途切换不重建 ToolExecutor(CLAUDE.md 视觉一节已写明原因)。所以「按模型给工具」不能在注册层做,必须在**每次组装请求的模型侧工具表**时过滤。
- 会话可以 resume 到另一个模型:历史里的 `file_edit` 调用不能因为当前模型偏好 apply_patch 就变成「未知工具」。所以三个工具永远注册,`resolve_model_tool_name_to_native` 保持 fail-open,只有**定义表**按模型过滤。
- 静态 system prompt 有 prompt cache 前缀不变量:模型族分支只能依赖「切模型才变」的输入,与视觉那一行同一口径。
- 路径边界与权限规则都是按单个 `file_path` 设计的(`extract_context` 只认 `file_path` / `path`);apply_patch 一次涉及多条路径,必须逐条过。

## Goals / Non-Goals

**Goals**

- GPT-5 / gpt-5-codex 家族拿到 `apply_patch` + Codex 风格提示,Claude / 其它模型行为零变化(逐字节不变,有测试守卫)。
- `apply_patch` 与 `file_edit` 在安全语义上等价:同一套 deny 规则、写边界、危险路径确认、检查点、编码/换行保留、LSP 诊断、hunks 渲染。
- 多文件补丁在 TUI / Web 都能看出改了哪些文件、每个文件改了什么。

**Non-Goals**

- OpenAI Responses API 的原生 `apply_patch` 工具类型 / custom(freeform)tool + Lark grammar:需要 provider 协议层改造,本期用普通 function tool(Codex CLI 对非 codex 模型也是 function 形态)。
- opencode 其余模型族提示(beast / gemini / kimi / trinity / meta)的整套搬运:本期只做 GPT 系;`ModelFamily` 枚举预留 Gemini / Anthropic 等值但不改变它们的提示。
- bash 里 `apply_patch <<EOF` heredoc 调用的拦截(Codex 的 `maybeParseApplyPatch`):ACECode 的 bash 走沙盒 + 写守卫,补丁文本作为工具参数已足够。

## Decisions

### D1. 模型偏好判定放在 `src/tool/model_family`,AgentLoop 与 system prompt 共用

`model_prefers_apply_patch(model_id)`:小写后 `(含 "gpt-" 且不含 "gpt-4" 且不含 "oss") || 含 "codex"`。比 opencode 多了 `codex`(codex-mini-latest 等不带 `gpt-` 前缀但同样是 apply_patch 训练的)。判定只看 model id 不看 provider 名:PA 内网网关 / OpenAI 兼容代理转发 GPT-5 时 provider 名是 `openai`,只有 id 可信。

`filter_tool_definitions_for_model(defs, prefers)`:按**模型侧名**(经 `model_tool_name_for_native`)删除 `file_edit` / `file_write` 或 `apply_patch`。放在 `tools_.get_model_tool_definitions*()` 之后、`bundle.tool_defs` 赋值之前,两条分支(常规 / 紧急档)都过;紧急档核心工具名单加 `apply_patch`。

### D2. 补丁解析与应用是纯函数,工具只负责 IO

`apply_patch_format.{hpp,cpp}`:`parse_patch(text)` → `ParsedPatch{hunks}`;`derive_new_contents(path, chunks, lf_text)` → 新内容或错误。与 codex-rs / opencode 一致的语义:

- 信封 `*** Begin Patch` / `*** End Patch`(按行 trim 后比较,允许 CRLF、允许外层 heredoc 包裹)。
- Update 段:`@@` 开新 chunk(其后文本为上下文锚点),` ` / `-` / `+` 三种行,`*** End of File` 标 EOF 锚定;空行视为空上下文行(模型输出常被去尾空白);其它前缀报错并给出行号,不静默吞。第一个 chunk 允许省略 `@@`(隐式空锚点)。
- Add 段:每行 `+` 前缀;空行视为空内容行。
- 上下文锚点先 seek 再从其后匹配;`old_lines` 为空 = 追加到文件末尾(上游行为,保留);找不到时去掉尾部空行重试一次;四级比较:精确 → rstrip → trim → Unicode 标点归一(弯引号 / 破折号 / 省略号 / nbsp)。
- 替换按起始行排序后倒序应用。结尾换行:原文有则有、原文空则加、原文无则不加(上游总是补一个,这里偏离以免产生无意义 diff 行)。

### D3. 工具执行:先全量校验,再按顺序落盘

- 参数名 `input`(Codex function tool 同名),`patchText` / `patch` 作为别名 fail-open 接受。
- 校验阶段不写盘:Add 目标已存在且非空 → 拒(与 `file_edit` 空 old_string 语义一致,避免无 diff 的静默覆盖);Delete / Update 目标必须是普通文件;Update 走 `check_edit_file_size` + `read_text_file_buffer`(lossy 解码 → `file_read_not_safe_for_edit`);Move 目标已存在 → 拒。
- **不要求先 file_read**(Codex / opencode 同款):补丁自带上下文即是校验;但落盘后 `record_write` 记基线,后续 `file_edit` 不会被「未读过」挡住。
- 每个文件:`acquire_write_guard` → `track_file_write_before`(Delete / Move 源文件也记,/rewind 才能恢复被删文件)→ `safe_write_text_file`(沿用源文件元数据,新文件 UTF-8/LF)→ `record_write`;Delete / Move 源 `invalidate_agent_read_state`。
- 输出 `Success. Updated the following files:` + `A/M/D path` 行(Move 显示 `M new (from old)`),每个非删除文件追加 LSP 诊断块。不回显 diff —— 模型刚写过这份补丁。
- `ToolResult.hunks`:所有文件的结构化 diff,每个 hunk 带 `file`;`summary`:单文件 → Created / Edited / Deleted + 路径,多文件 → Patched + `N files`;`metadata.files[]`。全部路径都在 scratch 目录 → `exclude_from_turn_change_summary`。

### D4. AgentLoop 权限门按路径集合走一遍

新增 `apply_patch::extract_target_paths(arguments_json, cwd)`(Add/Update/Delete 路径 + Move 目标,解析为绝对路径)。权限门里对 `apply_patch` 取路径集合,`ctx_path` 取第一条(日志 / hook payload / 确认框沿用),但 exec-rules 保护、`matched_rule` Deny、`should_auto_allow`、`path_validation_error`、`is_dangerous_path`、Plan 模式计划文件判定全部对集合逐条评估:任一 Deny 即拒;任一需确认即确认;Plan 模式要求全部路径都是计划文件。`recent_safe_edit_failures_` 不记 apply_patch。

### D5. hunk 带 file:一次结果多文件

`DiffHunk` 加可选 `file`(空 = 沿用 summary.object,file_edit / file_write 不填,序列化逐字节不变)。codec 只在非空时输出 `file`,并附带该 hunk 的 `additions` / `deletions`(Web 聚合器已支持 per-hunk 统计,多文件时 message 级 `+N/-M` 无法按文件归属)。TUI `render_diff_view` 在 hunk 的 file 变化时插入文件标题行;Web `hunksToUnifiedDiff` 按 file 分组输出多段 `--- a/ +++ b/`(diff2html 原生支持多文件)。

### D6. 系统提示分支

`SystemPromptModelState{model_id, family, prefers_apply_patch}` 作为 `build_system_prompt` 的尾参数(默认 nullptr = 旧行为)。偏好 apply_patch 时:file_edit / file_write 相关 bullet 全部不出,换成 apply_patch 指引;shell 指引里「多行内容优先用 file_write」改指 apply_patch;追加 `# Model-specific guidance (GPT)` 段(取自 opencode gpt.txt / codex.txt 中与 ACECode 不冲突的部分:自主推进、最小改动、脏工作区、ASCII 默认、无寒暄开场、只在真正阻塞时提问)。opencode gpt.txt 里的 `multi_tool_use.parallel`、commentary/final channel 属于 Responses API harmony 通道,chat completions 下不存在,不搬。

## Risks / Trade-offs

- 判定只看 id 子串:自定义模型名含 "codex" 但实际是别的模型会被误判 —— 代价只是换了编辑工具,功能不丢。
- Add File 拒绝覆盖非空文件比 Codex 严格,模型需先 Delete 再 Add;错误文案里写明。
- 多路径权限门的确认框只显示一次(列出全部文件),用户不能只放行其中一部分 —— 与 Codex 一致。
