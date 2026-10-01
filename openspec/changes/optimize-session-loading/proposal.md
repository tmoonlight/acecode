## Why

单个会话很长、会话条目很多时，用户反馈首次进入后展开工作区和打开会话非常慢。隔离实测（Release 0.9.30，热缓存）：打开一个 103MB 的会话要 8.2 秒才出内容；同一个 JSONL 被完整解析 3 次，完整历史给浏览器发了 2 次（每次约 76MB），而首屏只需要文件尾部约 2.4%；加载期间侧栏列表从 4ms 被拖到 1.1 秒。慢在"读得太多、读得太勤、读的时候占着锁"，与存储格式无关，所以本次只改加载方式，不迁移存储。

## What Changes

**阶段 0：埋点与诊断**

- daemon 对恢复会话、读取历史、会话列表类接口记录耗时、读取字节数和打开文件数；Web 记录"点击会话到出内容"的耗时。
- 新增只读诊断：按工作区统计会话数、最大 JSONL、各类记录的体积占比，不含任何会话内容，供反馈用户导出。

**阶段 1：去掉重复加载与锁阻塞（不改接口、不改存储）**

- 从侧栏打开未激活会话时，完整历史只下载一次；恢复完成后只补拉运行状态与增量事件。
- 读取会话历史时不再持有会话锁；会话列表不再在注册表锁内逐个等待各会话的锁。读一个大会话不再拖慢其它会话和侧栏。
- JSONL 每条记录只解析一次；下发历史时不再先序列化成字符串再解析回来。
- 恢复会话时，文件工具状态按路径去重，只处理最近压缩点之后的消息；去掉恢复时每条消息一行的误报警告日志。
- 侧栏：首屏不等置顶类接口；周期刷新不再枚举全部会话（`/api/sessions` 只取无工作区会话、置顶接口不为清理失效置顶而全量读取、已订阅的工作区状态不重发订阅）；"展开显示"按批拉取，不再一次拉全量。
- 搜索目录的后台预热推迟到启动后的首轮交互请求之后。

**阶段 2：历史分页与尾部读取（扩展接口，不改存储）**

- `GET /api/sessions/:id/messages` 支持 `limit` 与 `before` 游标，返回最近 N 条消息与 `has_more`；不带 `limit` 时保持现有全量行为。服务端从文件末尾倒读，只解析需要的记录；文件被改写后旧游标返回 409。
- Web/Desktop 打开会话先加载尾部一页；"显示更早的消息"、搜索跳转、时间轴跳转按需加载更早的页。
- 恢复会话时从文件尾部倒扫到最近一个有效压缩检查点，只解析该检查点及其后的记录来重建模型上下文；回退检查点、导出、分叉等需要全部历史的功能在使用时再读取。
- 回合结束后的自愈校对只拉取最近一轮；只读外部会话的轮询只拉取新增部分。

**不在本次范围**

- 不把会话本体迁入 SQLite，不改 JSONL 记录格式。
- 可重建的 SQLite 索引（会话元数据 + 消息字节位置）、JSONL 瘦身（每轮净差异单条上限外置、文件检查点改增量）作为后续 change，进入条件见 design.md。
- TUI 的恢复与显示行为不变，只受益于共享的解析与恢复优化。

## Capabilities

### New Capabilities

- `session-history-loading`：Web/Desktop 读取会话历史的契约，包括尾部优先的分页接口、每次打开只加载一次、增量校对与轮询、加载历史不阻塞其它会话请求。
- `sidebar-session-list-loading`：侧栏会话列表的加载与刷新成本，包括首屏不被全量枚举阻塞、周期刷新有界、状态订阅不重复、后台预热让位于交互请求。
- `session-load-diagnostics`：会话加载耗时埋点与只读的会话数据诊断。

### Modified Capabilities

- `session-resume`：新增要求，Web 恢复会话只解析最近压缩检查点及其后的记录，恢复耗时不随历史编辑次数增长。
- `sidebar-session-collapse`："展开显示"改为逐批露出并按批从服务端拉取，不再一次拉取全量列表；同时把规格与当前逐批露出的实现对齐。

## Impact

- **daemon**：`src/domain/session/`（`session_storage`、`session_serializer`、`session_manager`、`global_session_catalog`），`src/host/session_host/session_registry.cpp`，`src/apps/web/routes/`（`routes_sessions`、`routes_workspaces`、`routes_ws`）与 `server_helpers.cpp`，`src/engine/agent/event_payload/message_payload.cpp`，`src/adapters/tool/file_state_restore.cpp`，`src/engine/agent/transcript/conversation_history.cpp`。
- **Web**：`web/src/lib/`（`sessionTranscript`、`transcriptWindow`、`transcriptSelfHeal`、`connection`、`sidebarWorkspaceSessions`、`api`），`web/src/components/`（`ChatView`、`Sidebar`）。
- **接口**：`docs/daemon-api.md` 的 messages 接口新增参数与响应字段，向后兼容；诊断接口为新增。
- **存储与依赖**：无格式变化，不新增第三方依赖。
- **测试**：会话存储、注册表并发、web smoke、前端 Node 测试新增用例；隔离基准（合成大会话与 1500 个会话）用于各阶段前后对比。
