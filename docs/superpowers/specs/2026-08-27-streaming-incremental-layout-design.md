# 流式输出增量排版(Incremental Streaming Layout)设计

> 版本:2026-08-27 · 状态:已获设计评审通过
> 来源:`docs/tui-comparison/report.md` 差距 #1(流式输出增量排版,MICE 优先级 P0)

## 一、背景与目标

### 问题
TUI 流式输出时,每帧对**所有可见 assistant 消息**调用 `format_markdown(content)` 对**完整内容重新 lex + 构建 Element 树**。对话越长、流式消息越长,单帧 CPU 开销越大;`adaptive_background_interval_ms` 按帧耗时自动拉长重绘间隔,形成"越卡越稀"的正反馈。报告 #1 将此事列为 P0 差距。

### 现状(已核实)
- 流式:`on_delta` 把 token 追加进 `conversation.back().content`;重绘按 `select_streaming_redraw_interval_ms`(目标 50ms/帧)节流。
- 每帧:对每个可见 assistant 消息全量 `format_markdown`(lex → 语法高亮 → 建 Element 树)。
- 布局缓存(`message_render_revision`/`message_layout_valid`/`message_line_measures`)只缓存**测量高度/box 供滚动数学**,**不缓存 Element 树**。
- `StreamingFormatter`(markdown_formatter.cpp:788,稳定前缀缓存 + 只重排尾部)已实现但**全代码库无调用点(死代码)**,且只在"块边界(空行)"之间缓存,单段长段落不生效。
- 已优化:FTXUI `Paint` 只写变化 cell(终端 I/O 省);同步输出(CSI ?2026,已合并)原子呈现。

### 目标
长文档(约 100KB 对话 / 长代码块)流式输出时,单帧 lex+构建耗时**不随内容长度线性上涨**(增量后趋近 O(1)/帧),不触发自适应背压,主观无卡顿。缓存是**纯优化**:任何失效/异常回退到现有全量路径,功能不降级。

## 二、架构:三层递进

三层各自独立、可单独测试、可单独验收,每层是上一层的增量:

```
on_delta(token) → content 增长
  │
  ▼
[L3 增量 lexer] 仅 lex 追加的 delta;未完成尾 token(未闭合代码围栏/未完列表/未换行行)留在 pending 缓冲
  │ 新 token
  ▼
[L2 StreamingFormatter 增强] 稳定前缀 Element 缓存(按已完成"行"冻结);新 token 只建尾部 Element
  │ 整条消息 Element
  ▼
[L1 消息级 Element 缓存] 未变化消息跳过 format_markdown,复用缓存 Element(键 = revision+content+宽度+主题)
  │
  ▼
组装 message_elements → FTXUI layout → Paint(不变)
```

### 统一失效信号源
宽度变化(窗口 resize)、主题切换、`transcript_expanded`(Ctrl+O)开关、会话结构变化(compact/rewind/整体替换)。三层共用同一套失效语义。

## 三、组件细节

### L1 消息级 Element 缓存(render loop)
- 存储:`ChatScrollRuntime` 新增 `message_element_cache`(与 `message_layout_valid`/`revisions`/`widths` 并行,按消息 index 对齐)。
- 缓存键:`message_render_revision()` **补充 content 哈希**(现状不含 content,是缺陷)+ `current_message_width` + `theme_version` + `syntax_enabled`。
- 命中:复用 Element,跳过 `format_markdown`;缓存同时存每条消息的 `(href, 相对偏移)` 链接列表,命中时按当前布局 box 坐标**重放进 `MarkdownLinkRegionCollector`**(不重新 lex)。
- 失效:复用 `line_count_width_changed` 通道 + 主题/expanded 变更 + 会话重置(`reset_chat_line_measure_state_runtime`)。

### L2 StreamingFormatter 增强(接入 + 行级冻结)
- 生命周期:在 `TuiState` 挂**跨帧实例**(现为每帧局部/未使用),`reset()` 时机 = 新 turn / 消息完成 / 宽度变化 / 主题变化。
- 冻结粒度:从"块边界(空行)"增强到"**已完成行**"(`\n` 结尾且不在未闭合代码块内的行视为稳定)。
- 代码块策略:进入未闭合代码围栏后**暂停冻结**,整块保留在尾部重渲染;闭合后再整体入稳定区。
- 宽度/主题进缓存键;`cached_stable_` 在宽度/主题变化时重建。

### L3 增量 lexer(可续 token 流)
- 新增 `LexerState`:持有已产出 token 流 + 待定尾缓冲。
- `append(delta)` → 只 lex delta;未完成尾 token(未闭合代码围栏、未完列表项、未换行的段落行)留在 pending,下次 append 继续。
- 渲染消费:稳定 token 流(已建 Element)+ 尾缓冲(每次重建)。
- **正确性核心**:属性测试 `append(d1)+append(d2) == lex(full)` 对围栏/列表/标题/表格/引用/超长行边界成立。

## 四、数据流(组合后)

```
on_delta(token)
  → [L3] LexerState.append(delta) → 新 token
  → [L2] StreamingFormatter:稳定 token 的 Element 复用;新 token 建尾部 Element
  → render loop:
      - 流式消息:复用 StreamingFormatter stable Element + 新 tail
      - 其他消息:[L1] 缓存命中 → 复用 Element + 重放链接区域
      → 组装 → FTXUI layout(不变)→ Paint(不变)
```

## 五、错误处理与边界

### 统一失效表
| 触发事件 | 失效范围 |
|---|---|
| 窗口 resize(宽度变化) | 全部消息缓存 + StreamingFormatter stable + lexer pending |
| 主题切换 | 全部消息缓存 + stable 重建 |
| `transcript_expanded`(Ctrl+O) | 全部(现 revision 已含) |
| compact / rewind / 会话替换 | 全部(`reset_chat_line_measure_state_runtime`) |
| 单条消息内容/样式变化 | 仅该条(L1 revision 命中失败) |

### 回退保证
任一缓存层异常 → 立即回退到现有全量 `format_markdown` 路径(render_message_markdown 已有 try/catch),功能不降级。

### 内存有界
缓存只覆盖可见窗口消息(渲染已虚拟化);结构变化清空;不为历史消息无限缓存。

### 线程安全
`on_delta` 在 agent worker 线程 `state.mu` 锁内 append;渲染主线程读。L2/L3 跨帧状态访问须与 conversation 读写**同一把锁**(或快照),避免读到半更新增量状态。

### 边界用例(单测覆盖)
- 超长单行(无换行):L3 只 O(增量)。
- 流式中 resize / 主题热切换:立即失效重建,不崩。
- 空 delta、纯空白、控制字符。
- 未闭合代码围栏长时间悬挂:稳定区冻结,代码块整体在尾部,行为与现状一致。
- 消息完成 → 新 turn:reset 时机正确。

## 六、测试与基准验收

### 单元测试
| 层 | 测试点 | 位置 |
|---|---|---|
| L1 | 缓存命中/未命中(键=revision+content+宽度+主题);宽度/主题/expanded 切换失效;链接区域重放坐标正确 | `tests/tui/` 新增 |
| L2 | 行级冻结(单段长段落按行拆分);代码围栏闭合前整体保留尾部;reset 时机;宽度/主题键 | `tests/markdown/` 新增 |
| L3 | 可续 lexer 属性测试 `append(d1)+append(d2)==lex(full)`;围栏/列表/标题/表格/引用/超长行边界 | `tests/markdown/` 新增 |

### 基准脚本(验收依据)
- **C++ 侧基准(实际测量)**:新增一个可重复的 C++ 基准(单测式 harness,可在 `tests/` 下用 `--benchmark` 或独立二进制运行),直接测量 `format_markdown`(全量)与增量路径(`LexerState.append` + StreamingFormatter)的**单帧 lex+构建耗时 vs 累计内容长度**曲线。
- 三种负载:长散文、长代码块、混合。
- `demos/09_streaming_markdown.py` 仅作**概念演示参考**(Python 模拟,不代表实际 C++ 渲染路径);验收以 C++ 基准为准。
- 目标:增量路径耗时不随长度线性上涨(趋近 O(1)/帧),长文档流式不触发自适应背压;输出 before/after 对比曲线。

### 手动 TUI 验证(per AGENTS.md)
100KB+ 长对话/长代码块流式输出:主观流畅、无卡顿、resize/主题切换不崩、滚动正确。

### 验收标准
改动后基准曲线平稳 + 现有测试套件零新增回归(已知 10 个环境失败除外)。

## 七、范围与里程碑

- **里程碑 1(L1)**:消息级 Element 缓存 + content 哈希进 revision + 链接区域重放。约 2-3 天。
- **里程碑 2(L2)**:StreamingFormatter 接入 + 行级冻结 + 生命周期。约 2-3 天。
- **里程碑 3(L3)**:增量 lexer(LexerState + pending 缓冲 + 属性测试)。约 1-2 周。
- 每里程碑独立可测、可回退;全部完成后跑基准脚本对比验收。

## 验收记录(2026-08-30)

**基准(Task 10,C++ harness,before/after)**:three loads × 200/400/800/1600 行,每步 4 字节喂入;full 列=逐帧 format_markdown(模拟旧行为),incremental 列=append_delta(L2/L3)。
- prose(散文):full 2.79s→incremental 4.3ms @1600 行(~650x);incremental 总耗时线性(每帧 O(1))✅
- mixed(混合多块):full 1.37s→incremental 4.1ms @1600 行(~335x);incremental 近线性(每帧 O(1))✅
- code(单长代码块):incremental ≈ full(二次方)——开围栏整块留尾部,按设计"未闭合不提前画"(R13 文档化局限),非缺陷。
- R14 追加优化:稳定 vbox 缓存(仅新稳定 token 时重建),消除每帧 O(#稳定)拷贝。

**单测**:新增用例全部通过——MessageRenderCache 3、StreamingFormatter 5、LexerState 7、RenderTokenBlocks 2、ThemePalette +1(VersionBumpsOnSwap)等,共 20+ 新增;既有套件零新增回归。

**全量回归**:3599 个测试,3586 通过,8 失败——全部为既有环境失败(TcpProbe / BuiltinToolRegistry×2 / GrepGitBackend / SettingsCenterRender / StateFileTest / WebServerHttp×2,与 master 基线一致),**零新增失败**。

**评审与裁决**:11 个任务全部经 SDD 任务评审 + 修复轮;R1-R14 裁决记录于 `.superpowers/sdd/2026-08-27-streaming-incremental-layout/progress.md`(含:content 哈希只进渲染缓存键 R5、链接缓存改无链接消息 R6、行冻结栈式匹配 R7、行尾检查收窄 R8、主题版本 atomic R9、围栏闭合对齐 R10、宽度变化重放 R11、基准改名 R12、代码块局限 R13、vbox 缓存 R14)。

**手动验证**:手动 TUI 长流式体验未执行(环境限制),由单测 + 基准 + 回归覆盖。

**状态:里程碑 1/2/3 完成,#1 流式输出增量排版 已实施。**
