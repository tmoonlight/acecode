<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

会话名字有两套推导:侧栏用会话列表里的 `title`(用户改名 / 大模型生成)与 `summary`(`SessionManager::extract_summary` 对最近一条 user 消息 `content` 的 80 字节截断);顶部标题栏由 `useSessionTranscript` 维护,初值抄侧栏对象,但 `loadTranscriptHistory` / `transcript_replace` 随后用 `titleFromMessages`(最后一条 user 消息全文)覆盖,ChatView 发送时还会 `setTranscriptTitle(text)`。侧栏在 `summary` 以 "..." 结尾时另拉整份消息把跑马灯「水合」成全文。

`@session` 引用 / skill / 选区展开后,`content` 是给模型的长文本,用户敲的原文在 `metadata.display_text`;摘要与两条前端路径都没用它。

## Goals / Non-Goals

**Goals:** 会话显示标题是一个字段、一条派生规则、一条实时同步路径;长度由服务端约束;显示文本口径与消息气泡一致。

**Non-Goals:** 改变自动标题生成的触发 / 重试策略;改变「无标题会话显示最近一条用户消息」的既有语义(不改成首条);改 TUI。

## Decisions

- **摘要口径进存储层。** `SessionStorage::visible_user_message_text`(display_text 优先)+ `SessionStorage::summarize_user_message_text`(空白折成单行、UTF-8 安全 80 字节 + "...",英文在词边界断)。`SessionManager::extract_summary` 与 `list_sessions` 补齐旧 meta 的路径都委托给它,内存值与磁盘值逐字节一致。放存储层而不是 SessionManager,是因为补齐旧 meta 的代码在 session_storage.cpp 里,两边必须同实现。
- **服务端推 summary,前端不自己截。** AgentLoop 在用户消息落盘后(`append_user_turn_message` 与 soft-steer 插话两处)发 `session_updated{summary}`;`list_active` 填 `info.summary`(内存值,不读盘);`GET /messages?since=0` 快照带 `title` / `title_source` / `summary`(live 优先,磁盘 user 改名压过过期内存标题,与 `session_info_to_json` 同规则)。前端若自行按字符截断,会与 C++ 的字节 / 词边界规则出现细微偏差,而用户的要求正是「必须一致」。
- **Web 只保留一条派生。** transcript store 存 `title` / `titleSource` / `summary` 三个原始字段,对外 `title = sessionDisplayTitle({...ref, title, title_source, summary})`,与侧栏同一函数;`session_updated` 在 transcript reducer 与 Sidebar 列表里做同样的字段合并。删除 `titleFromMessages`、`setTitle`、`sidebarFullTitle.js`;跑马灯滚动的就是显示标题本身。
- **不改自动标题流程。** 生成标题就绪时的 `session_updated{title,title_source}` 路径原样保留,只是现在它覆盖的是同一个字段。

## Risks / Trade-offs

- [侧栏 hover 不再能看到超长请求的全文] → 这是有意为之:全文既不受限也与顶部不一致;完整原文在消息气泡里。
- [新会话在 summary 事件到达前顶部短暂显示「新会话N」] → 事件在用户消息落盘时即发出(毫秒级),且 messages 快照兜底;不引入前端本地截断以避免第二套规则。
- [老 daemon 不发 summary 事件] → 前端保留 ref 初值与快照字段,侧栏 5 秒轮询仍会带回 summary;只是顶部刷新变慢,不会出错。
