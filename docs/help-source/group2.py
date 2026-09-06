from common import page, section, code, table, note, figure

PAGES = {
"desktop": page("在一个窗口中组织任务、查看文件和验证修改。先熟悉工作区、聊天区和面板，再按自己的习惯安排工作。", [
    section("navigation", "界面与导航",
        """<p>打开桌面端后，左侧提供<strong>新建任务</strong>、<strong>定时任务</strong>和<strong>搜索任务</strong>。下方按置顶任务、独立任务和工作区组织已有记录。选择工作区中的任务后，中间显示该任务的对话和输入框。</p>
        <ol><li>第一次使用，进入<strong>设置 &gt; 模型</strong>，保存一个可用模型。</li><li>选择新建任务，再选择工作区；已有项目也可以从工作区旁的新建入口开始。</li><li>检查输入区下方的模型和权限模式，输入目标后发送。</li><li>任务执行时阅读工具过程；出现权限请求或提问时，在对应控件中处理。</li><li>离开后从侧边栏回到同一个任务，继续补充目标。</li></ol>
        <p>侧边栏中的扩展入口包含技能、MCP 服务器和专家组件。设置中的<strong>常规</strong>、<strong>外观</strong>、<strong>配置</strong>、<strong>个性化</strong>和<strong>模型</strong>分别管理不同选项。需要重新熟悉界面时，可以在常规页选择<strong>重新查看新手指引</strong>。</p>""",
        figure("UI-01", "桌面端导航与任务区域", "在已有任务中截取完整窗口，用编号标出新建任务、搜索任务、工作区、扩展入口、模型与权限控件，以及设置入口。")),
    section("panels", "文件预览、终端与面板",
        """<p>在文件树中点击文件打开详情标签页；点击变更文件则进入差异预览。多个文件可以同时保留在标签栏中，右上角隐藏详情面板会保留当前标签和草稿，再次打开可继续查看。</p>
        <p>工作区内的文本和代码直接进入可编辑预览，Markdown 使用相应编辑器。修改后按 <kbd>Ctrl+S</kbd> 保存；未保存标签会显示标记。关闭含草稿的标签时，选择保存并关闭、不保存或取消。磁盘文件已被其他程序改动时，先处理冲突提示，避免覆盖新内容。工作区外的绝对文件保持只读预览。</p>
        <p>详情标签栏的<strong>打开工具</strong>菜单提供文件、浏览器和侧边聊天。终端位于独立的控制台区域，可用 <kbd>Ctrl+`</kbd> 展开或收起，再通过加号新建终端或选择可用 Shell。终端中的命令由你直接执行，运行前检查当前路径。</p>
        <p>文件或目录的右键菜单可<strong>添加到会话</strong>，把路径插入聊天输入框；选中的文本可引用到聊天。网页操作见<a href="agent-browser.html">Agent 浏览器</a>，任务中的旁支问题见<a href="conversation.html#side-chat">侧边栏对话</a>。</p>""",
        figure("UI-02", "文件编辑、差异预览与控制台", "展示文件树、含未保存标记的文本标签、变更标签和底部控制台。标明隐藏面板与关闭标签是两个不同操作。"))
], ["README_CN.md", "web/src/lib/sidebarNavigation.js", "web/src/components/PreviewDetailsPanel.jsx", "web/src/components/ConsoleDock.jsx", "docs/user-manual.md"]),

"tui": page("在项目终端中直接使用 ACECode，通过键盘浏览回复、管理模型和处理工具确认。", [
    section("start", "进入项目并启动",
        '''<p>先安装终端版并运行配置向导。每次工作时进入项目目录，再启动 ACECode；当前目录决定本次项目上下文和历史记录范围。通过 SSH 使用时，文件、命令和依赖都位于远端计算机。</p>''',
        code("acecode configure\ncd /path/to/project\nacecode"),
        '''<p>输入任务后按 <kbd>Enter</kbd> 发送。上方显示对话，底部显示输入和当前模型、用量等状态。遇到路径含空格的项目，先用终端正常切换到该目录，再启动程序。</p>''',
        figure("UI-03", "TUI 对话与工具确认", "使用正常宽度的终端，展示项目路径、模型状态、一次工具调用摘要和权限确认选项；不显示密钥或私有路径。")),
    section("interact", "阅读、引用与中断",
        '''<p>输入 <code>@</code> 选择文件或目录；方向键选择，<kbd>Enter</kbd> 引用，<kbd>Tab</kbd> 或右方向键进入目录。空输入框中输入 <code>!</code> 可进入 Shell 直通模式，<kbd>Esc</kbd> 退出。Shell 命令仍受当前工具权限规则约束。</p>''',
        table(["操作", "按键或入口"], [["发送消息或确认选项", "Enter"], ["停止当前回复或拒绝确认", "Esc"], ["展开单条工具结果", "焦点位于工具结果时按 Ctrl+E"], ["全局展开或折叠工具输出", "Ctrl+O"], ["回看历史输入", "输入框中使用上、下方向键"], ["退出", "输入 /exit，或按提示连续两次 Ctrl+C"]]),
        '''<p>模型正在运行时仍可输入下一条消息，发送后进入队列。查看历史时使用滚动键；回到底部后继续跟随最新输出。工具摘要不足以判断结果时，展开原始输出再决定下一步。</p>'''),
    section("manage", "设置与恢复任务",
        '''<p><code>/model</code> 查看保存的模型，<code>/config</code> 打开设置中心。<code>/skills</code>、<code>/mcp</code>、<code>/tools</code>、<code>/hooks</code>、<code>/connectors</code> 打开能力中心对应页签。按底部提示操作，<kbd>Esc</kbd> 返回聊天。</p>''',
        code("acecode -r\nacecode --resume\nacecode --resume SESSION_ID", "终端 · 三种恢复方式，按需选择一条"),
        '''<p><code>-r</code> 打开选择器，裸 <code>--resume</code> 恢复最近会话，带 ID 则恢复指定会话。程序内也可使用 <code>/resume</code>。完整的键位与命令分类见<a href="reference.html">速查手册</a>。</p>''')
], ["docs/user-manual.md", "src/commands/command_registry.cpp", "src/cli/interactive_options.cpp"]),

"cli": page("使用非交互的 print 模式执行一次任务，把结果交给脚本或流水线，同时保留可继续的会话。", [
    section("print", "执行一次任务",
        '''<p><code>acecode -p</code> 不打开 TUI。它读取命令参数或标准输入，执行一个智能体回合，将最终回复写入标准输出后退出。它仍可能调用工具；运行目录、模型和权限同样需要事先确定。</p>''',
        code('acecode -p --permission-mode plan "阅读当前项目，说明入口和测试命令，不修改文件。"'),
        '''<p>也可以把已有文本通过管道传入；同时提供标准输入和提示词时，两者会合并。下面的命令请求审查当前差异：</p>''',
        code('git diff | acecode -p --permission-mode plan "审查这些变更，指出可复现的问题。"'),
        note("无交互环境中的权限", "需要确认的操作不会像桌面端那样等待你点击按钮。分析任务可使用 <code>plan</code>；允许自动文件编辑但不放开命令时，使用 <code>accept-edits</code>。具体权限边界见<a href=\"permissions.html\">权限模式</a>。")),
    section("output", "选择输出格式与限制",
        table(["参数", "作用"], [["<code>--output-format text</code>", "默认，输出最终文本回复。"], ["<code>--output-format json</code>", "输出单个结果对象，包含 session_id、is_error 和 result。"], ["<code>--output-format stream-json</code>", "逐行输出已完成的文本、工具、错误等记录，不是逐 token 文本流。"], ["<code>--thinking</code>", "在 stream-json 中包含完成的推理记录。"], ["<code>--model NAME</code>", "选择已保存的模型预设名称。"], ["<code>--max-turns 10</code>", "限制智能体循环次数；这不是 token 预算。"]]),
        code('acecode -p --output-format json --max-turns 10 "说明项目的目录结构。"'),
        '''<p>脚本应同时检查退出码和结果中的错误字段。退出码：<code>0</code> 成功，<code>1</code> 回合失败，<code>64</code> 参数错误，<code>130</code> 被中断。不要把标准输出中出现部分文本当作整项任务成功。</p>'''),
    section("continue", "继续会话与启用扩展",
        code('acecode -p --session-id docs-review "阅读 README 并列出缺失信息。"\nacecode -p --resume docs-review "只为刚才列出的缺失信息提出补充建议。"'),
        '''<p>print 模式的 <code>--resume</code> 必须带 ID；继续当前目录最近会话使用 <code>-c</code> 或 <code>--continue</code>。自定义新会话 ID 最多 64 个字符，只包含字母、数字、短横线和下划线。</p>''',
        code("acecode -p --list-tools --list-skills --list-mcp", "终端 · 只列出可用能力，不执行模型任务"),
        '''<p>print 模式默认启用可用的内置工具，技能和 MCP 需用 <code>--enable-skills</code>、<code>--enable-mcp</code> 点名开启；内置工具可用 <code>--disable-tools</code> 点名禁用。名称以列出结果为准，多个名称用逗号分隔。查看完整参数使用 <code>acecode -p --help</code>。</p>''')
], ["src/headless/headless_options.cpp", "src/headless/headless_options.hpp", "src/headless/headless_runner.cpp"]),

"web": page("浏览器连接 ACECode 后台服务后，可以继续管理任务和使用智能体。项目文件与命令运行在服务所在的计算机上。", [
    section("local", "在本机打开 Web",
        '''<p>安装终端版后，在希望作为起点的项目目录启动后台服务。启动成功时会输出实际的 Web 地址；复制该地址到浏览器。</p>''',
        code("acecode daemon\nacecode daemon status"),
        '''<p>未覆盖配置时，本机地址为 <code>http://127.0.0.1:12399/</code>。显式端口设置或桌面端托管服务可能使用其他端口，以启动输出为准。后台服务已运行时，先检查状态，无需反复启动。</p>''',
        '''<p>打开页面后，按<a href=\"quick-start.html\">快速开始</a>配置模型、选择任务并发送消息。关闭浏览器标签页不会自动停止独立启动的后台服务。需要结束后台服务时运行 <code>acecode daemon stop</code>。</p>''',
        figure("UI-04", "本机 Web 地址与任务界面", "展示终端启动输出的 Web UI 地址和浏览器打开后的任务页面，确保地址与本次实际端口对应。")),
    section("remote", "从其他设备连接",
        '''<ol><li>先在服务所在计算机确认本机 Web 可访问。</li><li>进入<strong>设置 &gt; 常规 &gt; 远程 Web 模式</strong>并开启。</li><li>在连接地址下拉框中选择其他设备可到达的主机名或网卡地址。</li><li>点击<strong>复制连接</strong>，在目标设备的浏览器打开。</li></ol>''',
        '''<p>远程模式启动独立代理，本机服务仍监听回环地址。远程连接使用代理的端口与访问 Token，不能直接把本机地址中的 127.0.0.1 替换后就假定可用。开关不会替你修改防火墙、路由器或云安全组。</p>''',
        note("连接中包含访问凭据", "复制的完整连接包含 Token，请只交给可信设备使用。跨公网访问应通过可信 VPN 或配置了 HTTPS 的入口；不要公开发布这个连接。")),
    section("differences", "直接 Web 与桌面端的差别",
        '''<p>浏览器上传的是客户端文件的附件快照；桌面端原生文件选择可直接引用本机路径。选择远端工作区时，路径必须存在于后台服务所在的计算机。浏览器自身看到的本地路径不能直接当作服务端路径使用。</p>''',
        '''<p>原生文件窗口、桌面通知授权和可共享的 Agent 浏览器属于桌面集成能力。直接 Web 中缺少相应入口或显示不可用时，使用该页面提供的浏览器流程；需要原生功能则使用支持的平台桌面端。</p><p>连接失败时按<a href=\"troubleshoot-desktop.html\">桌面、终端与远程连接问题</a>依次检查服务、地址、代理和鉴权。</p>''')
], ["src/daemon/cli.cpp", "docs/user-manual.md", "docs/daemon-api.md", "web/src/components/SettingsPage.jsx"]),

"conversation": page("给出目标和必要上下文，在同一个任务里补充要求；需要旁支问答时使用独立的侧边聊天。", [
    section("references", "发送消息与引用文件",
        '''<p>在输入框写明目标、相关范围和验证方式，<kbd>Enter</kbd> 发送，<kbd>Shift+Enter</kbd> 换行。输入 <code>@</code> 会列出当前目录的直接子项；继续输入路径可以进入目录并过滤候选。</p><p>用方向键选择后，<kbd>Enter</kbd> 引用文件或文件夹，<kbd>Tab</kbd> 或右方向键进入文件夹。目录引用保留末尾的 <code>/</code>，含空格的路径会加引号。选择目录不会递归附上所有文件。</p>''',
        code('检查 @src/ 和 @tests/ 中与登录相关的实现。\n修复空密码仍能提交的问题，保留现有样式，并运行相关测试。', "示例请求"),
        '''<p>桌面端可在文件树中右键选择<strong>添加到会话</strong>，或把预览中选中的文本<strong>引用到聊天</strong>。路径引用让智能体按需读取文件，发送前可直接编辑或删除输入中的引用。</p>'''),
    section("attachments", "添加图片与附件",
        table(["添加方式", "实际交给任务的内容"], [["桌面端：添加上下文 &gt; 文件或文件夹", "本地路径引用，包括图片、PDF、归档和大文件；选择时不预读整个文件。"], ["桌面端：选择当前文件夹", "当前目录的相对或绝对路径引用。"], ["粘贴没有本地路径的剪贴板图片", "图片附件快照。"], ["浏览器直接上传文件", "上传到服务端任务中的附件快照，受格式和大小校验约束。"]]),
        '''<p>添加后检查输入框中的路径或附件卡片，再发送要求。例如说明截图中的错误位置，或指定 PDF 的页码。视觉能力取决于所选模型和可用工具；仅添加一张图片不代表文本模型能直接看懂它。上传被拒时查看提示，按需减小文件、拆分内容或改用桌面路径引用。</p>''',
        figure("UI-05", "路径引用与图片附件", "分别展示桌面选择文件后形成的 @ 路径，以及粘贴图片后出现的附件预览和移除入口；用图注明确两种内容的区别。")),
    section("queue", "排队、补充指令与中断",
        '''<p>任务运行时，发送按钮显示<strong>排队</strong>。按 Enter 后，消息进入输入框上方的排队卡片，等当前回合结束再发送。卡片提供编辑、取消和失败后的重试操作。</p><p>想让补充信息影响当前回合时，点击排队卡片上的<strong>插话</strong>。它会在当前回合下一次模型调用前加入消息，不会瞬间撤销已经执行的工具。需要立即结束当前工作时，使用<strong>中断</strong>或停止控件，等状态结束后再发送新的任务要求。</p>''',
        note("停止后仍需检查文件", "中断停止后续执行，已经完成的文件写入仍在磁盘上。先查看变更，再决定继续、修正或恢复；不要把停止操作当作撤销。"),
        figure("UI-06", "排队消息与插话", "在任务运行时展示一张待发送卡片，标出编辑、插话、取消以及主输入区的中断控件。")),
    section("side-chat", "侧边栏对话",
        '''<p>界面中的入口名是<strong>侧边聊天</strong>，位于详情栏的打开工具菜单。它有独立输入草稿，适合询问当前实现的理由、解释报错或补充知识，不会改写主输入草稿。</p>''',
        code("/side 刚才为什么选择这个实现？\n/btw 这个报错中的术语是什么意思？", "示例 · 两个命令分别发送"),
        '''<p><code>/side</code> 与 <code>/btw</code> 是同类旁支问答入口。回答基于当前任务的上下文快照，独立执行一轮，不调用工具，不把这条问题当作主任务的后续修改指令。需要实际读取新文件或执行修改时，把要求发到主对话。</p><p>主任务继续工作后，侧边回答仍对应提问时的上下文；需要最新进展时重新询问。每次问题都应包含清楚的指代，避免只写“它”或“这个”。</p>''',
        figure("UI-07", "侧边聊天与主输入草稿", "同时展示主输入区中未发送的草稿和侧边聊天的问题、回答，强调二者独立，旁支问答不会中断主任务。"))
], ["docs/user-manual.md", "web/src/components/QueueCardList.jsx", "web/src/lib/inputBarState.js", "web/src/components/SideQuestionComposer.jsx", "web/src/components/ChatView.jsx", "docs/daemon-api.md"]),

"workspaces": page("工作区确定要操作的项目位置，任务保存一次持续工作的上下文。先选对范围，再组织和恢复任务。", [
    section("manage", "添加与管理工作区",
        '''<p>在侧边栏<strong>工作区</strong>旁点击添加入口，选择本地项目目录。项目显示在列表中后，点击其新建任务按钮即可在该目录开始工作。一个工作区可以保存多个任务。</p><p>工作区菜单支持重命名项目、复制项目路径和从项目列表移除。移除列表项不会删除磁盘上的项目文件。远程 Web 的工作区路径属于后台计算机，不能填写另一台设备才有的目录。</p>''',
        figure("UI-08", "工作区列表与项目菜单", "展示添加工作区按钮、同一项目下的多个任务和项目右键菜单，标出新建任务与从项目列表移除。")),
    section("standalone", "脱离工作区的任务",
        '''<p>在新建任务时选择<strong>不使用工作区</strong>，适合通用问答、文字处理或先讨论方案。此类记录集中在侧边栏<strong>任务</strong>分组，仍可置顶和归档。</p><p>没有工作区时，不会自动获得某个项目的文件树和 Git 上下文。桌面端可以显式引用文件或目录，路径在没有项目根目录时通常显示为绝对路径。需要持续修改某个代码库时，新建对应工作区中的任务更容易确认操作范围。</p>'''),
    section("organize", "恢复、搜索、置顶与归档",
        '''<ol><li><strong>恢复：</strong>点击侧边栏中的任务，阅读最近结果后继续发送；TUI 使用 <code>/resume</code> 或启动参数。</li><li><strong>搜索：</strong>点击搜索任务，按任务标题或消息线索查找，选择结果回到相应记录。</li><li><strong>置顶：</strong>使用任务旁的置顶按钮或右键菜单；不使用工作区的任务也可以置顶。</li><li><strong>归档：</strong>将暂时不用的任务从日常列表收起。到设置中的已归档会话查找并恢复。</li></ol><p>归档保留记录。已归档页面中的彻底删除会移除本地会话数据，操作前先核对对象；长期保存或分享阅读内容可先导出。</p>'''),
    section("transfer", "分叉、导入与导出会话",
        '''<p>在支持的消息右键菜单选择<strong>从这里分叉</strong>，会复制截至该消息的对话前缀并打开新任务。新任务不会自动开始执行；输入下一条消息后才继续。原任务保留，磁盘文件不会因为分叉回到过去。</p><p>桌面端任务菜单中的<strong>导出</strong>会保存可见对话为 Markdown。导出文件适合阅读和分享，不等于可直接导入恢复的完整任务备份。</p><p>项目菜单提供<strong>从opencode导入会话</strong>：先预览识别出的记录，勾选需要的会话，再启动导入并查看结果。导入入口针对识别到的 OpenCode 历史，不能把任意 Markdown 文档当作会话包导入。</p>''',
        figure("UI-09", "会话分叉与导入预览", "用两张截图展示消息菜单中的从这里分叉，以及项目菜单打开的 OpenCode 会话预览、选择与导入进度；不展示真实私有对话。"))
], ["web/src/components/Sidebar.jsx", "web/src/lib/sidebarNavigation.js", "web/src/components/DesktopContextMenu.jsx", "web/src/components/ChatView.jsx", "docs/daemon-api.md"]),

"git": page("在执行前确认工作位置，执行后核对差异。会话历史、工作树和磁盘文件是三个需要分别管理的对象。", [
    section("changes", "查看变更与差异",
        '''<p>在 Git 项目任务中打开<strong>变更</strong>面板，查看相对比较基线发生变化的文件和增删行数。点击文件会打开详情栏中的差异预览；修改完成后可手动刷新列表。</p><p>面板的基线下拉框决定比较对象，改变它不会切换当前分支。Git 变更可能包含你、其他程序和其他任务的修改，不能仅凭出现在列表中就认定都由当前任务产生。非 Git 项目缺少同样的仓库级比较，应结合任务中的文件编辑记录检查。</p>''',
        code("git status --short\ngit diff --stat\ngit diff -- path/to/file", "终端 · 核对仓库与指定文件"),
        figure("UI-10", "Git 变更与差异预览", "展示基线选择、文件列表、增删行数和一个文件的差异；图注明确基线选择用于比较。")),
    section("worktrees", "Git 分支与工作树",
        '''<p>在尚未开始的新 Git 项目任务中，可以勾选 <strong>worktree</strong>，然后选择创建工作树的基线分支。实际工作树在第一条消息发送时创建；勾选本身不会立刻新建目录。</p><p>不勾选 worktree 时，任务在当前工作区执行，此处的分支选项不可用。基线选择用于新工作树，不是主仓分支切换器。任务已有消息后，也不能靠这个新任务控件迁移正在执行的任务。</p><p>独立工作树适合尝试另一种实现，但它仍使用同一 Git 仓库的历史与对象。完成后检查工作树中的变更，再根据项目流程提交、合并或清理。需要当前未提交内容时，先明确希望从哪份文件状态开始，不要默认所有未提交内容都会复制过去。</p>''',
        figure("UI-11", "新任务中的 worktree 设置", "展示新任务发送前的 worktree 复选框和已启用的基线分支选择，配图说明创建发生在首次发送时。")),
    section("restore", "回退与恢复",
        '''<p>先停止仍在写文件的任务，再查看具体差异。需要撤销 ACECode 刚做的一部分修改时，在同一任务里明确文件和改动范围，要求保留原有未提交内容：</p>''',
        code("只撤销你本次对登录按钮文案的修改。\n保留校验逻辑以及我原先未提交的其他变更，完成后给出差异。", "示例请求"),
        '''<p>若要把某个受 Git 跟踪的文件恢复到提交版本，先备份仍需保留的修改，再使用 Git 工具恢复该文件。恢复前的 <code>git diff -- path/to/file</code> 能帮助判断会失去哪些未提交内容。恢复后重新检查状态并运行相关验证。</p>''',
        '''<h3>TUI 的检查点回退</h3><p>任务空闲时提交 <code>/rewind</code>（别名 <code>/checkpoint</code>），选择要回到的用户回合，再选择恢复代码、对话或两者。只有存在可用文件检查点的回合才提供代码恢复；旧记录可能只能恢复对话。</p><p>恢复对话会以选中消息之前的上下文创建新会话，并把该条请求放回输入框，原完整会话仍可从 /resume 打开。代码恢复以检查点记录的文件范围和返回结果为准，不能替代所有 Shell 命令或外部系统操作的撤销。恢复后检查实际文件差异，再决定重新发送。</p>''',
        note("区分会话恢复与代码回退", "普通恢复历史任务、桌面消息分叉不会自动撤销后来写入磁盘的内容。TUI 只有明确选择可用的代码恢复才会处理文件。恢复后向智能体说明新的文件状态，必要时让它重新读取。"))
], ["web/src/components/GitChangesPanel.jsx", "web/src/components/GitChangeReview.jsx", "web/src/components/GitSessionPill.jsx", "web/src/lib/gitSessionPill.js", "docs/daemon-api.md", "src/commands/builtin_commands.cpp"]),

"context-usage": page("查看当前上下文与实际用量，必要时压缩长对话，保留继续工作的关键信息。", [
    section("read", "理解用量数字",
        '''<p>当前会话的上下文占用描述下一次模型请求需要携带多少信息；输入、输出和缓存 token 记录描述已经发生的请求用量。两者不是同一个统计。长工具输出、图片和附加指令也可能增加上下文开销。</p><p>TUI 使用 <code>/tokens</code> 查看会话用量。桌面端在模型与状态区域查看当前信息，在<strong>设置 &gt; 使用情况</strong>查看汇总、记录和会话统计。不同服务商上报的字段可能不同，缺失的统计不应当作零消耗。</p>''',
        figure("UI-12", "上下文状态与使用情况", "展示任务中的上下文提示和使用情况页面中的输入、输出、缓存统计，标明当前请求范围与累计统计范围。")),
    section("compact", "手动与自动压缩",
        '''<p>输入并提交 <code>/compact</code> 可主动压缩已有上下文。压缩需要模型生成接续摘要，因此仍会发起模型请求。界面会展示压缩过程，成功后收起为 <code>Context compacted</code> 一类完成记录，可以展开查看。</p><p>当前自动压缩在活动上下文达到模型标称窗口的 90% 时触发。窗口值来自当前模型配置，因此应该按服务商的实际限制设置，不能靠随意增大窗口数字解决服务端超限。</p><p>压缩保留继续工作需要的摘要和最近用户信息，人类可见的聊天记录不会因此被截短。恢复会话时会继续使用有效压缩记录后的上下文。如果压缩失败，阅读错误原因再处理，不要把失败的压缩当作已经完成。</p>'''),
    section("reduce", "让后续请求更清楚",
        '''<ul><li>引用明确的文件和范围，避免反复粘贴整个大型日志。</li><li>保留目标、已经验证的结论、未完成事项和重要约束。</li><li>任务主题已经改变时，考虑新建任务并附上必要交接信息。</li><li>超限时先确认实际模型窗口、输出限制与错误内容，再压缩或拆分输入。</li></ul><p>压缩减少后续上下文负担，不会退回已经消耗的 token。连接中断或服务端限制导致的错误，按<a href="troubleshoot-context.html">上下文超限与请求失败</a>继续排查。</p>''')
], ["docs/user-manual.md", "web/src/components/SettingsPage.jsx", "src/session/session_manager.cpp", "docs/daemon-api.md"])
}
