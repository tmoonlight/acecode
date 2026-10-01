## Purpose

定义 Web/Desktop 读取会话历史的行为：从文件尾部按页加载、每次打开只加载一次、增量校对与轮询，以及读取历史时不阻塞其它会话请求，使打开会话的耗时与会话总长度基本无关。

## ADDED Requirements

### Requirement: 历史接口支持尾部分页
`GET /api/sessions/:id/messages` 在 `since=0`（或省略）且带 `limit=N` 时 SHALL 按时间顺序返回最新的可见消息，条数不少于 N（不足 N 条时返回全部），并返回 `has_more`；`has_more` 为 true 时 SHALL 同时返回 `before` 游标，指向本页最早一条消息之前的位置。带 `before=<游标>&limit=N` 的请求 SHALL 返回紧邻该位置之前、不少于 N 条的可见消息。每个带 `limit` 的响应 SHALL 返回 `after` 游标，指向本页最后一条记录之后的位置；带 `after=<游标>` 的请求 SHALL 只返回该位置之后新增的可见消息。可见消息的过滤规则与现有全量快照一致：文件检查点、压缩检查点、隐藏的 goal 上下文消息不出现在结果中。带 `limit` 的尾部页 SHALL 携带与全量快照相同的运行状态字段。不带 `limit`、`before`、`after` 的请求 SHALL 保持现有的全量快照行为。`limit` 超过服务端上限时 SHALL 按上限处理；`limit`、`before` 或 `after` 无法解析时 SHALL 返回 400。

#### Scenario: 获取尾部一页
- **WHEN** 会话共有 1000 条可见消息，客户端请求 `since=0&limit=200`
- **THEN** 响应的 `messages` MUST 是会话末尾连续的至少 200 条可见消息，按时间先后排列，且包含最后一条可见消息
- **THEN** 响应 MUST 包含 `has_more: true`、非空的 `before` 游标和 `after` 游标
- **THEN** 响应 MUST 包含 busy、标题、摘要、权限模式等与全量快照相同的运行状态字段

#### Scenario: 向前翻页直到开头
- **WHEN** 客户端用上一页返回的 `before` 游标继续请求 `limit=200`
- **THEN** 响应 MUST 返回紧邻上一页之前、至少 200 条可见消息，两页之间 MUST NOT 重叠或遗漏
- **THEN** 当返回的页已包含会话第一条可见消息时，响应 MUST 为 `has_more: false` 且不含 `before` 游标

#### Scenario: 只取新增部分
- **WHEN** 客户端用上次响应的 `after` 游标请求，期间会话追加了 3 条可见消息
- **THEN** 响应 MUST 只包含这 3 条消息，并返回新的 `after` 游标

#### Scenario: 不带 limit 的旧调用
- **WHEN** 客户端请求 `since=0` 且不带 `limit`
- **THEN** 响应 MUST 与现有全量快照一致，包含全部可见消息

#### Scenario: 非法参数
- **WHEN** 客户端请求 `limit=abc` 或一个无法解析的 `before` 值
- **THEN** 服务端 MUST 返回 400 和错误码，且 MUST NOT 读取会话文件

### Requirement: 文件被改写后游标失效
分页游标 SHALL 只在会话文件仅被追加时保持有效。会话文件被整体改写（例如重试或回退重写了 JSONL）之后，使用旧游标的请求 SHALL 返回 409 与错误码 `HISTORY_CURSOR_STALE`，客户端 SHALL 丢弃已加载的更早页并从尾部页重新加载。

#### Scenario: 追加不影响旧游标
- **WHEN** 客户端拿到某个 `before` 游标后，会话又追加了新消息
- **THEN** 使用该游标的请求 MUST 正常返回游标之前的消息

#### Scenario: 改写使旧游标失效
- **WHEN** 客户端拿到某个 `before` 游标后，会话文件被重试或回退整体改写
- **THEN** 使用该游标的请求 MUST 返回 409 与 `HISTORY_CURSOR_STALE`
- **THEN** 客户端 MUST 重新请求尾部页，而不是展示新旧混合的历史

### Requirement: 分页读取量与会话总长度无关
返回尾部页或 `before` 页时，daemon SHALL 只读取和解析组成该页所需的记录，外加有限的预读，SHALL NOT 解析目标页之前的记录。

#### Scenario: 大会话的尾部页
- **WHEN** 会话文件大于 50MB，客户端请求最新的 200 条消息
- **THEN** 本次请求读取的字节数 MUST 与返回的页大小同一量级，而不是与文件大小同一量级（以加载诊断记录的读取字节数为准）

### Requirement: 打开会话只加载一次历史
Web/Desktop 每次打开一个会话时（包括需要先恢复的未激活会话），SHALL 最多请求一次持久化历史；会话变为实时状态之后，SHALL 只获取运行状态和本地尚未拥有的增量事件，SHALL NOT 再次请求全部历史。

#### Scenario: 从侧栏打开未激活会话
- **WHEN** 用户在侧栏点击一个未激活的会话
- **THEN** 客户端 MUST 只发出一次历史请求和一次恢复请求
- **THEN** 恢复完成、会话变为实时状态后，客户端 MUST NOT 再发出 `since=0` 的全部历史请求

### Requirement: 更早的消息按需加载
对话记录 SHALL 先只显示最新一页。用户要求查看更早的消息时，客户端 SHALL 加载紧邻已加载部分之前的一页。导航目标（搜索结果、时间轴跳转）位于尚未加载的部分时，客户端 SHALL 把已加载部分向前连续扩展到包含目标，再定位到目标；扩展可以由一次或多次请求完成，已加载的消息 SHALL 保持连续、不留空洞。

#### Scenario: 查看更早的消息
- **WHEN** 已加载的页之前还有消息，用户点击"显示更早的消息"
- **THEN** 客户端 MUST 请求紧邻已加载部分之前的一页，并把它插到现有内容之前，当前阅读位置 MUST 保持不动

#### Scenario: 搜索跳转到很早的消息
- **WHEN** 用户从搜索结果跳转到一条位于尚未加载部分中的用户消息
- **THEN** 客户端 MUST 把已加载部分向前扩展到包含该消息，然后滚动并高亮该消息
- **THEN** 该消息与原有尾部之间的所有可见消息 MUST 都已加载

### Requirement: 增量校对与轮询
回合结束后的自愈校对 SHALL 只获取最近一轮所需的消息，SHALL NOT 获取全部历史。查看由其它进程持有的只读会话时，周期轮询 SHALL 使用 `after` 游标只获取上次轮询之后新增的记录；没有新增时 SHALL NOT 重新传输已有消息；会话文件被改写导致游标失效时 SHALL 重新加载尾部页。

#### Scenario: 长会话中一轮结束
- **WHEN** 一个包含上万条消息的会话完成了一轮对话
- **THEN** 自愈校对请求 MUST 只返回覆盖最近一轮的尾部消息

#### Scenario: 只读会话没有新消息
- **WHEN** 客户端轮询一个由 TUI 持有的只读会话，且自上次轮询以来会话文件没有变化
- **THEN** 轮询响应 MUST NOT 包含已传输过的消息

### Requirement: 读取历史不阻塞其它请求
daemon 读取或序列化某个会话的持久化历史期间，其它会话的请求、会话列表请求和置顶状态请求 SHALL NOT 等待这次读取；该会话自身的新消息落盘 SHALL NOT 等待一次全部历史的读取。

#### Scenario: 读取大会话时请求侧栏列表
- **WHEN** daemon 正在读取一个大会话的历史，同时收到某个工作区的会话列表请求
- **THEN** 会话列表请求 MUST 在不等待该次历史读取的情况下返回

#### Scenario: 读取历史时其它会话的请求
- **WHEN** daemon 正在读取会话 A 的历史，同时收到会话 B 的草稿或权限请求
- **THEN** 会话 B 的请求 MUST 在不等待会话 A 历史读取的情况下返回

#### Scenario: 读取历史时会话正在运行
- **WHEN** 会话 A 正在运行一轮对话，同时客户端请求会话 A 的历史
- **THEN** 会话 A 新产生的消息 MUST 能在历史读取进行期间落盘
