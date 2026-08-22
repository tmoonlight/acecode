## ADDED Requirements

### Requirement: 全局会话目录必须增量预热并复用
系统 SHALL 在 daemon 生命周期内维护按项目分片的全局会话目录快照，并 SHALL 在后台以有界批次预热。普通搜索请求 MUST 查询已构建快照而不是重新同步扫描全部项目；会话与工作区变化 MUST 只失效相关项目分片。目录范围仍 MUST 包含所有有效、未归档、非后台子代理会话，不得以工作区可见性过滤。

#### Scenario: 首次打开时目录尚未完成
- **WHEN** 用户在全局目录只完成部分项目预热时打开搜索
- **THEN** 系统 MUST 立即返回已扫描分片中的有界结果和真实进度
- **AND** 请求 MUST NOT 等待全部项目扫描完成

#### Scenario: 再次打开复用已完成分片
- **WHEN** 用户取消搜索后再次打开
- **THEN** 系统 MUST 保留先前成功构建的项目分片并从未完成位置继续
- **AND** MUST NOT 从第一个项目重新全盘扫描

#### Scenario: 单项目会话发生变化
- **WHEN** 一个项目中的会话被创建、更新、归档、恢复或删除
- **THEN** 系统 MUST 只标记并刷新该项目分片
- **AND** 其它项目分片 MUST 保持可查询

### Requirement: 元数据搜索必须服务端过滤并有界分页
全局元数据搜索 SHALL 接受查询、结果上限、请求 id 和可选分页 cursor。空查询 MUST 按更新时间返回最近的有界会话；非空查询 MUST 在所有已索引普通会话中匹配标题、摘要和工作区属性。响应 MUST 返回目录进度；只有完整且 generation 稳定的快照才可签发后续分页 cursor。

#### Scenario: 空查询打开搜索
- **WHEN** 客户端以空查询和 limit 50 请求会话
- **THEN** 服务端 MUST 最多返回最近 50 条会话
- **AND** MUST NOT 传输完整全局会话集合

#### Scenario: 查询仍在增量推进
- **WHEN** 快照未完成且客户端查询一个关键词
- **THEN** 服务端 MUST 返回当前已知的最佳有界命中以及 scanned、total、complete 和 generation
- **AND** 客户端 MUST 能在后续短请求中刷新结果而不阻塞输入与关闭操作

#### Scenario: 完整快照继续分页
- **WHEN** 完整快照中的命中数量超过 limit 且客户端提交有效 cursor
- **THEN** 服务端 MUST 从同一 generation 的稳定排序位置继续返回下一页
- **AND** 不得重复或跳过该 generation 中的结果

#### Scenario: 分页期间 generation 改变
- **WHEN** 客户端提交的 cursor 属于旧 generation
- **THEN** 服务端 MUST 明确报告 cursor 已失效并要求从第一页刷新
- **AND** MUST NOT 把旧 offset 应用于新排序

### Requirement: 搜索结果必须使用精简模型
元数据与正文搜索响应 SHALL 只包含结果身份、匹配、显示、进度和跳转所需字段。搜索响应 MUST NOT 包含完整 transcript、`session_path`、token usage、todos、attention 游标、expert 详情或其它完整会话管理字段。

#### Scenario: 返回元数据搜索结果
- **WHEN** 会话命中全局元数据搜索
- **THEN** 结果 MUST 包含会话 id、工作区身份/名称/cwd/可见性、no-workspace 标记、标题、摘要、创建/更新时间和必要的 active/status
- **AND** MUST NOT 调用完整 session 序列化来构造结果

#### Scenario: 返回正文命中
- **WHEN** 用户消息正文命中一个会话
- **THEN** 结果 MAY 额外包含匹配片段、附件名称、消息序号和分数
- **AND** MUST NOT 返回该会话的完整消息记录

### Requirement: 正文搜索必须以短批次增量推进
正文搜索 SHALL 在服务端为 `request_id + query` 建立有界作业，并 SHALL 在每个 HTTP 批次内限制处理项目数和时间。每批 MUST 返回当前累计最佳命中和真实进度；旧项目索引补建 MUST 接受同一取消检查。

#### Scenario: 大量项目中的正文搜索
- **WHEN** 正文搜索范围包含成千上万个项目
- **THEN** 单个 HTTP 请求 MUST 只推进有界项目批次或时间片
- **AND** 客户端 MUST 能在批次之间显示部分结果并决定继续或取消

#### Scenario: 查询文本发生变化
- **WHEN** 用户在旧正文作业完成前修改查询
- **THEN** 客户端 MUST 取消旧 request id 并为新查询创建新 request id
- **AND** 服务端 MUST NOT 继续为旧查询补建或扫描剩余项目

### Requirement: 关闭搜索必须端到端取消
客户端 SHALL 为每次搜索持有可中止 fetch 和服务端 request id。关闭按钮、Escape、点击遮罩、查询替换或组件卸载 MUST 立即中止当前网络等待，并 MUST 尽力调用幂等服务端取消端点。服务端 SHALL 在项目、meta 文件和正文索引 session 边界观察取消，停止该请求的剩余工作。

#### Scenario: 加载期间点击关闭
- **WHEN** 用户在目录或正文搜索仍进行时点击关闭按钮
- **THEN** 客户端 MUST 立即关闭弹层并中止当前 fetch
- **AND** 服务端 MUST 在下一个有界取消检查点停止该 request id 的剩余扫描

#### Scenario: 使用 Escape 或点击遮罩关闭
- **WHEN** 用户使用 Escape 或点击弹层遮罩关闭搜索
- **THEN** 系统 MUST 执行与关闭按钮相同的本地中止和服务端取消语义

#### Scenario: 重复取消同一请求
- **WHEN** 客户端多次取消同一个 request id
- **THEN** 服务端 MUST 幂等返回成功或已取消状态
- **AND** MUST NOT 恢复或重新创建该搜索作业

### Requirement: 渐进结果与失败状态必须保持可操作
SearchPalette SHALL 在预热和正文搜索未完成时保持输入、已有结果与关闭操作可用，并 SHALL 显示进度而不是用全局 loading 门遮挡列表。用户取消 MUST 保持安静且不得覆盖最近成功结果；超时或真实错误 MUST 显示可重试状态。

#### Scenario: 部分结果先到达
- **WHEN** 任一增量批次返回结果但全局进度尚未完成
- **THEN** SearchPalette MUST 立即展示或更新这些结果
- **AND** 输入框、结果选择和关闭操作 MUST 保持可用

#### Scenario: 用户主动取消
- **WHEN** 用户关闭面板或替换查询导致请求取消
- **THEN** 客户端 MUST NOT 显示错误提示
- **AND** MUST NOT 用空结果覆盖最近一次成功批次

#### Scenario: 非取消错误
- **WHEN** 搜索请求因超时、服务错误或无法恢复的目录错误失败
- **THEN** SearchPalette MUST 显示可见错误和重试操作
- **AND** 仍 MUST 允许用户关闭面板或继续编辑查询
