## MODIFIED Requirements

### Requirement: Memory Directory Layout
系统 SHALL 把记忆分两个作用域存放:
- **全局作用域**:`<data_dir>/memory/`(默认数据目录下即 `~/.acecode/memory/`,与现有位置相同),存放跨项目的个人偏好。
- **工作区作用域**:`<data_dir>/projects/<工作区标识>/memory/`,`<工作区标识>` 与该工作区会话存储目录使用的标识相同(由会话所属工作目录得出),MUST NOT 读取 git 远端地址参与计算。

每个作用域目录 MUST 只在顶层识别条目文件 `<safe-name>.md`(名字仅含 `[A-Za-z0-9_-]`,1–64 字节)与生成的索引 `MEMORY.md`;保留子目录 `inbox/`(待整合的观察)与 `archive/`(已整合的观察)供记忆摘要使用,其余子目录一律忽略。所有进程共用的记忆状态库位于 `<data_dir>/memory/state.sqlite3`。

#### Scenario: First-run creation
- **WHEN** 记忆启用且全局目录不存在
- **THEN** 创建空的全局目录;工作区目录在首次写入该作用域时才创建;两个作用域都没有条目时不注入任何记忆上下文

#### Scenario: Read existing index
- **WHEN** 升级前 `~/.acecode/memory/` 下已有合法条目
- **THEN** 它们作为全局条目原样加载,不需要迁移、不改写文件

#### Scenario: worktree 会话共享工作区记忆
- **WHEN** 工作区 W 的会话进入 ACECode worktree 后读写工作区记忆
- **THEN** 读写的都是 W 的工作区作用域,与在主检出中运行的会话看到同一份条目

#### Scenario: Oversized index truncated
- **WHEN** 某个作用域的条目索引超过 `config.memory.max_index_bytes`
- **THEN** 注入的记忆上下文只列出预算内的条目并附一行省略说明;磁盘上的 `MEMORY.md` 不被修改

#### Scenario: Unexpected subdirectory
- **WHEN** 作用域目录下存在 `skills/` 等非保留子目录
- **THEN** 该子目录不被扫描,只有顶层 `*.md`(`MEMORY.md` 除外)算作条目

### Requirement: Memory Entry Frontmatter
每个条目文件 SHALL 以 `---` 包围的 YAML frontmatter 开头。必填字段:`name`(非空)、`description`(非空)、`type`(`user` / `feedback` / `project` / `reference` 之一)。系统维护的来源字段:`created_at`、`updated_at`(UTC ISO-8601)、`source`(`manual` 或 `summary`)、`source_sessions`(会话 id 列表)。未知字段 MUST 保留在磁盘上并被忽略。缺少来源字段的旧条目 MUST 仍然有效,并在下一次被系统写入时补齐。

#### Scenario: Valid entry
- **WHEN** 条目含 `name`、`description`、`type: user` 与正文
- **THEN** 条目加载成功,正文可经 `memory_read` 读取

#### Scenario: Missing required field
- **WHEN** 条目缺少 `type`
- **THEN** 该条目不加载、记录警告,进程不崩溃

#### Scenario: Invalid type value
- **WHEN** 条目 `type: notes`(不在四类之内)
- **THEN** 该条目不加载并记录警告

#### Scenario: Malformed frontmatter
- **WHEN** frontmatter 缺少结束的 `---` 或引号不配对
- **THEN** 退回简单 `key: value` 解析;仍无法解析则跳过该条目并记录警告

#### Scenario: 旧条目补齐来源字段
- **WHEN** 一个没有 `created_at` / `updated_at` 的旧条目被 `memory_write` 以 update 方式改写
- **THEN** 写入后的文件含 `updated_at`(本次时间)与 `source: manual`,`created_at` 取文件原有的修改时间

### Requirement: System Prompt Injection
记忆启用且会话未关闭记忆时,系统 SHALL 在会话的第一次模型请求时生成一份**记忆上下文快照**,包含全局与当前工作区两个作用域的索引,并在该会话之后的每次请求中逐字节复用,直到该会话的历史被压缩(摘要压缩或改写历史的线程修复)才从磁盘重建。快照 MUST 满足:
- 每个作用域的索引不超过 `config.memory.max_index_bytes`(默认 8 KiB);超出的条目不列出,改为一行说明省略了多少条、可用 `memory_read` 查看。
- 条目带有时间信息时,每行标注距今天数(如「3 天前」)。
- 附带使用说明:记忆是历史记录,依赖其中的路径、命令、仓库状态前 MUST 用工具核实;与用户当前指令冲突时以当前指令为准。
- 快照只进入发给模型的请求,MUST NOT 写入会话记录文件。

两个作用域都没有条目、记忆被配置关闭或本会话关闭记忆时,MUST NOT 出现记忆上下文。

#### Scenario: Index present
- **WHEN** 全局或当前工作区作用域中有条目,会话发出第一次请求
- **THEN** 请求中包含记忆上下文,按作用域分段列出条目(带距今天数)与使用说明

#### Scenario: 会话内快照稳定
- **WHEN** 会话进行中通过 `memory_write` 新增了一条工作区记忆
- **THEN** 本会话后续请求中的记忆上下文与写入前逐字节相同;新条目在下一个会话或本会话压缩之后出现

#### Scenario: 压缩后重建
- **WHEN** 会话发生摘要压缩后发出下一次请求
- **THEN** 记忆上下文按当前磁盘内容重建,包含压缩前新写入的条目

#### Scenario: 超出预算
- **WHEN** 工作区索引超过 `max_index_bytes`
- **THEN** 只注入预算内的条目,并注入一行省略说明;磁盘上的 `MEMORY.md` 不被修改

#### Scenario: 工作区隔离
- **WHEN** 工作区 A 的会话发出请求
- **THEN** 记忆上下文只含全局与 A 的工作区条目,不含工作区 B 的条目

#### Scenario: Empty index
- **WHEN** 两个作用域都没有条目
- **THEN** 请求中不出现记忆上下文

#### Scenario: Memory disabled by config
- **WHEN** `config.memory.enabled=false`,或本会话执行过 `/memory off`
- **THEN** 请求中不出现记忆上下文,记忆工具也不在模型侧工具表中

### Requirement: memory_read Tool
系统 SHALL 向模型提供 `memory_read` 工具,参数均可选:`scope`(`global` / `workspace` / `all`,默认 `all`)、`name`、`type`、`query`(对名字、描述、正文做不区分大小写的子串匹配)。行为:
- 无参数:返回两个作用域的条目列表,每项含作用域、`name`、`description`、`type`、`updated_at`。
- 给 `name`:返回该条目的全部字段与正文;未指定作用域时先查工作区再查全局。
- 给 `query`:返回匹配条目及一小段命中上下文。
- 条目不存在:返回 `{found:false}`,不算错误。
读取结果 MUST 反映磁盘当前内容,包括其他进程刚写入的条目。

#### Scenario: List all
- **WHEN** 模型调用 `memory_read({})`
- **THEN** 返回全局与当前工作区的全部条目,每项标明所属作用域

#### Scenario: Filter by type
- **WHEN** 模型调用 `memory_read({type:"feedback"})`
- **THEN** 只返回两个作用域中 `type` 为 `feedback` 的条目

#### Scenario: Load specific entry
- **WHEN** 全局与工作区各有一个名为 `build` 的条目,模型调用 `memory_read({name:"build"})`
- **THEN** 返回工作区那一条;指定 `scope:"global"` 时返回全局那一条

#### Scenario: 关键字查找
- **WHEN** 模型调用 `memory_read({query:"撰写窗口"})`
- **THEN** 返回名字、描述或正文含该词的条目及命中片段

#### Scenario: 读到其他进程的写入
- **WHEN** 另一个 ACECode 进程刚写入条目 `x`,本进程随后调用 `memory_read({name:"x"})`
- **THEN** 返回 `found:true` 与该条目内容

#### Scenario: Entry not found
- **WHEN** 模型读取不存在的名字
- **THEN** 返回 `{found:false}`,工具调用状态为成功

### Requirement: memory_write Tool
系统 SHALL 向模型提供 `memory_write` 工具。参数:`name`(必填,清洗为 `[A-Za-z0-9_-]{1,64}`)、`type`(必填,四类之一)、`description`(必填,非空)、`body`(必填)、`mode`(`create` / `update` / `upsert`,默认 `upsert`)、`scope`(`global` / `workspace`,可选)。未给 `scope` 时:`user`、`feedback` 写入全局,`project`、`reference` 写入工作区;会话没有所属工作区时一律写入全局。

写入 MUST 原子完成(临时文件 + 重命名),并更新该作用域的 `MEMORY.md`,每个条目恰好一行索引。写入前 MUST 对 `description` 与 `body` 做敏感信息脱敏;发生脱敏时工具结果 MUST 说明。系统 MUST 记录来源字段(`source: manual`、当前会话 id、时间)。模型显式写入不受墓碑限制。

#### Scenario: Create new entry
- **WHEN** `memory_write` 以 `mode: "create"` 写入一个不存在的条目
- **THEN** 条目文件创建在对应作用域目录,该作用域的 `MEMORY.md` 多出一行索引,条目带来源字段

#### Scenario: 按类型选择作用域
- **WHEN** 模型写入 `type: project` 的条目且未指定作用域
- **THEN** 条目写入当前工作区作用域;`type: user` 的条目写入全局作用域

#### Scenario: 显式指定作用域
- **WHEN** 模型写入 `type: feedback` 且 `scope: "workspace"`
- **THEN** 条目写入当前工作区作用域

#### Scenario: Update existing entry
- **WHEN** `mode: "update"` 更新已存在的条目
- **THEN** 条目文件被改写,`MEMORY.md` 中对应行原地替换,其他行顺序不变

#### Scenario: Create mode collision
- **WHEN** `mode: "create"` 但同名条目已存在
- **THEN** 返回错误,磁盘不变

#### Scenario: Update mode missing
- **WHEN** `mode: "update"` 但条目不存在
- **THEN** 返回错误,磁盘不变

#### Scenario: Path traversal attempt
- **WHEN** `name` 为 `../../etc/passwd`
- **THEN** 名字清洗后被拒绝,返回错误,不写任何文件

#### Scenario: 写入前脱敏
- **WHEN** `body` 含 `password=hunter2` 与 `sk-` 开头的密钥
- **THEN** 落盘内容中二者被替换为 `[REDACTED]`,工具结果提示发生了脱敏

#### Scenario: Atomicity under interruption
- **WHEN** 进程在写临时文件与重命名之间被杀掉
- **THEN** 作用域目录中不出现半截的条目文件;重启后只看到已提交的条目

### Requirement: Memory Permission Model
`memory_read` SHALL 按只读工具处理,在所有权限模式下自动放行。`memory_write` 的目标解析到当前数据目录下某个记忆作用域目录内时,SHALL 在除 Yolo 以外的模式下自动放行(Yolo 本就全部放行);解析到作用域目录之外(包括经符号链接逃逸)时 MUST 在任何模式下拒绝。

#### Scenario: Read auto-approve
- **WHEN** `default` 权限模式下调用 `memory_read`
- **THEN** 不弹确认直接执行

#### Scenario: Write auto-approve in-scope
- **WHEN** `default` 模式下 `memory_write` 写入当前工作区作用域
- **THEN** 不弹确认直接执行

#### Scenario: Write rejected out-of-scope
- **WHEN** `memory_write` 的目标经符号链接解析到作用域目录之外
- **THEN** 无论何种权限模式都拒绝并返回错误

### Requirement: `/memory` Slash Command
系统 SHALL 在 TUI 与网页 / 桌面对话里提供同一套 `/memory` 命令,两处对同一输入产生相同的结果文本(`edit` 除外):
- `/memory` 或 `/memory list [--scope=global|workspace] [--type=<t>]`:列出条目,按作用域分组。
- `/memory view <name>`:显示条目字段与正文。
- `/memory edit <name>`:TUI 用 `$EDITOR` 打开;网页提示到「设置 > 个性化 > 记忆」编辑。
- `/memory forget <name>`:删除条目及其索引行,并记录墓碑。
- `/memory flush`:立即为当前会话提炼观察并整合当前工作区与全局的收件箱;记忆摘要未开启时只说明如何开启,不做任何模型调用。
- `/memory off` / `/memory on`:本会话关闭 / 恢复记忆(不注入记忆上下文、不提供记忆工具、不参与记忆摘要),设置随会话持久化。
- `/memory reload`:从磁盘重新扫描。

#### Scenario: List all
- **WHEN** 在 TUI 与网页对话中分别执行 `/memory list`
- **THEN** 两处输出相同的分组列表

#### Scenario: View specific
- **WHEN** 用户执行 `/memory view go_expert` 且条目存在
- **THEN** 显示条目所属作用域、字段与正文(未指定作用域时先工作区后全局)

#### Scenario: Forget removes both file and index line
- **WHEN** 用户执行 `/memory forget build_steps`
- **THEN** 条目文件与索引行被删除,并记录该名字的墓碑

#### Scenario: Reload picks up external edits
- **WHEN** 用户在 ACECode 之外编辑了条目文件,然后执行 `/memory reload`
- **THEN** 之后的 `/memory list` 与 `memory_read` 返回新内容

#### Scenario: 未开启记忆摘要时 flush
- **WHEN** 记忆摘要关闭时执行 `/memory flush`
- **THEN** 返回说明文本(如何在个性化设置中开启),不发起模型调用

#### Scenario: 本会话关闭记忆
- **WHEN** 用户执行 `/memory off` 后继续对话,并在之后恢复该会话
- **THEN** 此后的请求都不含记忆上下文、不含记忆工具,该会话不会被记忆摘要处理;恢复会话后仍保持关闭,直到执行 `/memory on`

### Requirement: MemoryRegistry Thread Safety
记忆的所有读写 SHALL 在多线程与多进程(TUI、多个 daemon、headless 共用同一数据目录)下保持一致:条目写入原子;重新生成 `MEMORY.md` 时以写入时刻磁盘上的条目为准,MUST NOT 丢掉其他进程写入的条目;后台作业通过共享状态库的租约协调,同一作业同一时刻最多在一个进程中运行;持有租约的进程崩溃后,租约过期即可被其他进程重新执行。

#### Scenario: Concurrent read and write
- **WHEN** 工作线程调用 `memory_write` 的同时 UI 线程执行 `/memory list`
- **THEN** 两个调用都无数据竞争地完成;列表一致地反映写入前或写入后的状态

#### Scenario: 两个进程各写一条
- **WHEN** 两个进程都在启动时扫描过空目录,之后先后写入条目 `a`、`b`
- **THEN** `MEMORY.md` 同时包含 `a` 与 `b` 两行

#### Scenario: 同一作用域并发整合
- **WHEN** 两个进程同时尝试整合同一作用域的收件箱
- **THEN** 只有一个进程执行,另一个跳过

#### Scenario: 崩溃后接续
- **WHEN** 执行整合的进程在中途被杀掉
- **THEN** 租约过期后,该批次由任一进程重新执行,已整合的内容不被重复写入

## ADDED Requirements

### Requirement: Memory Surface Parity
同一配置下,TUI、daemon(桌面 / 网页)与 headless 会话 SHALL 具有相同的记忆行为:相同的记忆工具、相同的记忆上下文注入规则、相同的作用域解析。子会话 MUST 使用父会话的工作区作用域。headless 会话 MUST 提供记忆工具(可被 `--disable-tools` 移除)并注入记忆上下文,但不参与记忆摘要。

#### Scenario: 桌面会话具备记忆
- **WHEN** 在桌面版中新建会话并发出第一次请求,且存在全局记忆条目
- **THEN** 请求中包含记忆上下文,工具表中包含 `memory_read` 与 `memory_write`

#### Scenario: headless 工具清单
- **WHEN** 执行 `acecode -p --list-tools`
- **THEN** 清单中包含 `memory_read` 与 `memory_write`,且二者可被 `--disable-tools` 接受

#### Scenario: 子会话共享父会话工作区
- **WHEN** 工作区 W 的会话派生子会话,子会话调用 `memory_read({})`
- **THEN** 返回全局与 W 的工作区条目

### Requirement: Memory Secret Redaction
任何内容写入记忆之前(模型显式写入、记忆摘要产生的观察、整合产生的条目、用户经网页编辑的条目),系统 SHALL 把可识别的敏感信息替换为 `[REDACTED]`,至少覆盖:常见前缀的 API 密钥与令牌(如 `sk-`、`ghp_`、`xai-`、`AKIA`)、`Bearer` 令牌、私钥块、URL 中携带的用户名密码,以及形如 `password=`、`passwd:`、`token=`、`api_key=`、`密码:` / `密码：` 的赋值中的值。

#### Scenario: 私钥块
- **WHEN** 待写入内容包含 `-----BEGIN PRIVATE KEY-----` 到对应结束行之间的内容
- **THEN** 整个私钥块被替换为 `[REDACTED]`

#### Scenario: 中文密码赋值
- **WHEN** 待写入内容包含「测试账号 tester01 / 密码：abc#2026」
- **THEN** 密码值被替换为 `[REDACTED]`,其余文字保留

#### Scenario: 带凭据的 URL
- **WHEN** 待写入内容包含 `https://user:pass@example.com/path`
- **THEN** 凭据部分被替换为 `[REDACTED]`,主机与路径保留

### Requirement: Memory Management Interface
daemon SHALL 提供经鉴权的接口,用于列出(按作用域)、读取、更新、删除条目,重置某个作用域的全部记忆,以及读取与修改记忆设置;网页「设置 > 个性化」SHALL 提供「记忆」区,包含:使用记忆开关、记忆摘要开关与摘要模型(见 `memory-summarization`)、按「全局 / 当前工作区」浏览条目、查看与编辑条目(保存时脱敏并更新 `updated_at`)、删除条目(记录墓碑)、带二次确认的重置。TUI 设置中心的「个性化」页 SHALL 提供同样的使用记忆开关与记忆摘要设置,条目的浏览与删除在 TUI 中通过 `/memory` 完成。

#### Scenario: 编辑条目
- **WHEN** 用户在个性化页修改某条工作区记忆的正文并保存
- **THEN** 条目文件原子更新,`updated_at` 刷新,`MEMORY.md` 对应行更新

#### Scenario: 删除条目
- **WHEN** 用户在个性化页删除一条记忆
- **THEN** 条目与索引行被删除,并记录墓碑;TUI 执行 `/memory list` 不再显示该条

#### Scenario: 重置作用域
- **WHEN** 用户确认重置当前工作区记忆
- **THEN** 该作用域的条目、索引、收件箱与归档全部清空,全局作用域不受影响

#### Scenario: 关闭使用记忆
- **WHEN** 用户关闭「使用记忆」
- **THEN** 之后新开始的会话不注入记忆上下文、不提供记忆工具;已有条目保留在磁盘上
