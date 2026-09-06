from common import page, section, code, table, note, figure

PAGES = {
"configuration": page("优先通过界面修改对应设置；需要手动编辑时，先确认数据目录、字段范围和保存方式。", [
    section("locations", "配置保存在哪里",
        r'''<p>个人安装的主配置是 <code>~/.acecode/config.json</code>，Windows 对应 <code>%USERPROFILE%\.acecode\config.json</code>。模型预设、默认模型以及网络、技能、MCP 等全局选项保存在这套用户配置中。</p><p>Windows 服务模式使用 <code>%PROGRAMDATA%\acecode</code>，与个人安装的数据目录分开。连接远端后台时，配置属于远端用户或服务身份；编辑本机文件不会自动修改远端配置。</p>''',
        table(["配置层", "适合保存的内容"], [["全局配置", "服务商连接、默认值、扩展连接与运行选项。"], ["工作目录覆盖", "例如 TUI 用 /model --cwd 保存的项目模型选择。"], ["项目文件", "项目规则、项目技能和项目 Hooks。"], ["当前任务", "任务选择的模型、权限与对话上下文。"]]),
        '''<p>具体位置见<a href="reference.html#data">本地配置和数据</a>。复制配置到其他计算机前，检查其中的绝对路径、可执行文件位置和认证信息。</p>'''),
    section("save", "修改与生效",
        '''<p>模型页通过保存模型或保存修改提交；个性化文本和 MCP JSON 等控件会在离开编辑区时保存，并显示保存状态。TUI 设置中心的 General、Appearance 开关通常即时保存，配置与模型表单按底部提示使用 <kbd>Ctrl+S</kbd>。</p><p>不同配置有不同生效边界。任务模型和权限使用专门的切换入口；后台连接与运行服务以界面的应用结果为准。出现“重启 daemon 后生效”时，保存正在进行的工作后重启。手动改动任意 JSON 文件，并不等于所有运行中的模块都已经重新加载。</p>''',
        note("配置片段需要合并", "文档中的 JSON 示例只展示相关字段。请合并到现有对象中，不要用片段替换整份配置，否则可能丢失已经保存的模型和其他设置。")),
    section("recovery", "手动编辑与错误恢复",
        '''<ol><li>先备份当前有效配置，使用支持 UTF-8 的编辑器打开。</li><li>只修改目标字段，保持正确的 JSON 类型，不加入注释或尾随逗号。</li><li>重新加载相关功能，或按该功能要求重启。</li><li>检查界面实际值和一次小操作，确认修改已生效。</li></ol><p>当前版本会保存有效配置快照。配置损坏且存在有效快照时，会备份错误文件并尝试自动恢复；Web/Desktop 会显示一次<strong>配置已自动回滚</strong>提示。没有可用快照时仍会报告配置错误。按提示查看备份位置并修复目标字段，备份可能含密钥，不要直接公开。</p>''')
], ["src/config/config.cpp", "src/config/config.hpp", "docs/user-manual.md", "web/src/components/SettingsPage.jsx"]),

"permissions": page("权限模式决定文件修改和工具执行何时需要你确认。发送任务前确认模式，遇到请求时核对实际操作。", [
    section("modes", "选择权限模式",
        table(["桌面端 / Web 模式", "常规行为"], [["默认权限", "读取通常自动通过；文件写入和命令执行按规则确认。"], ["自动接收编辑", "文件写入、编辑自动通过，命令执行仍需确认。"], ["完全访问权限", "常规工具权限自动通过，应明确任务范围和允许的副作用。"]]),
        '''<p>当前任务从输入框下方切换模式。设置的常规页可设置新会话默认模式；界面提示存在活动任务时，修改也会同步当前任务。TUI 可用设置中心或空闲状态下的 <kbd>Ctrl+P</kbd> 切换。</p><p>CLI 另支持 <code>--permission-mode plan</code>，用于限制写入的分析任务。它不是当前桌面模式菜单中的第四个选项。模式不能替代操作系统权限、工具可用性或其他安全限制。</p>''',
        figure("CF-01", "权限选择与操作确认", "展示任务的权限下拉菜单，以及一次文件写入或命令请求；标出工具名、路径、参数和允许或拒绝入口。")),
    section("requests", "处理操作确认和提问",
        '''<p>请求出现时，先看工具要做什么、作用于哪个文件或目录、命令是否符合当前目标，再决定允许或拒绝。来自子代理的请求会带来源信息，处理前确认是哪一个后台任务。</p><p>TUI 的确认选项支持允许本次、在当前会话内持续允许该工具和拒绝；<kbd>Esc</kbd> 可拒绝。不要把当前会话内的持续允许理解成永久修改系统安全设置。</p><p>智能体也可能提出需要你选择的业务问题。按问题给出的选项回答，或填写自己的说明；提问不是工具执行权限。默认模式下任务停在等待确认状态时，处理请求后才能继续。</p>'''),
    section("boundary", "中断与危险启动参数",
        '''<p>停止任务会阻止后续工作，但已经完成的写入和命令副作用不会自动回滚。拒绝一个工具后，可以说明原因并要求智能体采用更小范围的方案。</p>''',
        note("危险启动模式", "<code>--yolo</code> / <code>--dangerous</code> 启动参数会绕过权限和路径安全检查，适合明确受控的本地环境，不应作为日常排错的通用办法。危险模式不能开启远程 Web 访问。", True),
        '''<p>只想减少文件编辑的确认时，先考虑自动接收编辑。需要了解如何检查已经发生的修改，参见<a href="git.html#restore">回退与恢复</a>。</p>''')
], ["web/src/lib/permissionMode.js", "web/src/components/SettingsPage.jsx", "src/permissions.hpp", "src/headless/headless_options.cpp", "docs/user-manual.md"]),

"network": page("分别配置访问模型服务的出站代理，以及让其他设备连接 ACECode 的远程 Web 入口。", [
    section("outbound", "模型请求使用的代理",
        '''<p>出站代理由 <code>config.json</code> 的 <code>network</code> 配置控制。默认 <code>auto</code> 会尝试读取系统或环境中的代理；Windows 使用系统代理信息，macOS/Linux 使用相关代理环境变量。</p>''',
        table(["proxy_mode", "含义"], [["auto", "自动选择可用的系统或环境代理。"], ["off", "强制直接连接，忽略代理来源。"], ["manual", "使用 proxy_url 指定的代理。"]]),
        code('{\n  "network": {\n    "proxy_mode": "manual",\n    "proxy_url": "http://127.0.0.1:7890",\n    "proxy_no_proxy": "localhost,127.0.0.1"\n  }\n}', "JSON · 合并到主配置，端口替换为实际代理端口"),
        '''<p>先确认代理程序已经运行且端口正确。需要直连的主机可加入 <code>proxy_no_proxy</code>，它会与环境中的 NO_PROXY 合并。这里的 7890 只是示例，不是 ACECode 自带的代理服务。</p>'''),
    section("inspect", "检查与临时调整",
        '''<p>TUI 中使用 <code>/proxy</code> 查看实际代理、来源和可达性。<code>/proxy refresh</code> 重新解析并探测，<code>/proxy off</code> 临时直连，<code>/proxy set URL</code> 临时指定，<code>/proxy reset</code> 回到配置来源。这些临时操作与手动保存全局配置要区分。</p><p>自动模式检测到代理不可达时，可能进入 <code>auto-fallback</code> 直连状态。若服务只能通过代理访问，先恢复代理再 refresh；不要因为浏览器能打开网页，就假定 ACECode 进程使用了同一代理。</p><p>模型 URL、MCP URL 和代理 URL 分别填写。代理不是模型的 Base URL；证书、鉴权、DNS 或接口参数错误也不能一概通过换代理解决。</p>'''),
    section("inbound", "远程 Web 的访问代理",
        '''<p><strong>设置 &gt; 常规 &gt; 远程 Web 模式</strong>用于其他设备访问当前 ACECode。开启后出现连接地址和复制连接入口，本机后台仍监听回环地址。它与上面的模型出站代理是两套配置。</p><p>连接失败时依次检查后台服务、远程模式状态、实际代理端口、主机名解析和防火墙。复制连接包含 Token，服务重启后旧连接可能失效，需要重新复制。</p>''',
        figure("CF-02", "远程 Web 连接设置", "展示远程 Web 开关、主机或网卡选择、实际端口和复制连接按钮；将 Token 完整遮挡。"),
        '''<p>完整连接流程见<a href="web.html#remote">从其他设备连接</a>。跨公网访问使用可信 VPN 或 HTTPS 入口，不直接公开包含 Token 的地址。</p>''')
], ["src/config/config.hpp", "src/config/config.cpp", "src/commands/proxy_command.cpp", "web/src/components/SettingsPage.jsx", "docs/user-manual.md"]),

"appearance": page("根据阅读习惯调整界面外观，选择语言，并设置任务完成后是否接收系统通知。", [
    section("visual", "外观与工作模式",
        '''<p>进入<strong>设置 &gt; 外观</strong>选择主题、字号和主色。当前提供小、中、大字号及蓝色、橙色主色选项。调整后回到任务，检查正文、输入区和工具输出的阅读效果。</p><p><strong>设置 &gt; 常规 &gt; 工作模式</strong>提供<strong>用于编程</strong>与<strong>适合日常工作</strong>，用于调整智能体显示的技术细节。它不是模型切换或权限开关，需要换模型和权限时仍使用对应控件。</p>''',
        figure("CF-03", "外观与工作模式", "展示外观页实际提供的主题、字号、主色选项，以及常规页两种工作模式。不要将本文档主题按钮误画成应用设置。")),
    section("language", "界面语言",
        '''<p>在常规页的语言选项中选择自动、简体中文或英语。自动模式跟随当前环境可识别的语言设置。TUI 设置中心目前使用自身的英文页签和键位提示，不能把 Web 的每个中文标签原样套用到 TUI。</p><p>界面语言与回复语言是不同需求。希望智能体始终用某种语言回答时，可以在请求中说明，或把稳定偏好写入<a href="memory.html">个性化指令</a>。</p>'''),
    section("notifications", "任务完成通知",
        '''<p>在常规页开启<strong>打开任务完成通知</strong>。桌面端只在 ACECode 窗口失去焦点且主任务完成时发送系统通知；盯着前台窗口时没有通知，并不代表任务没有完成。</p><p>macOS 会显示系统通知授权状态。未授权时点击授权；被拒绝后使用<strong>打开系统设置</strong>检查权限。系统专注模式或通知策略也可能影响展示。直接 Web 的通知能力取决于浏览器与当前环境，不能视为所有桌面原生通知功能都可用。</p><p>排查时先让一个小任务在窗口失焦后完成，再查看应用开关和系统权限。不要用子任务结束来验证主任务完成通知。</p>''')
], ["web/src/components/SettingsPage.jsx", "web/src/lib/desktopNotify.js", "docs/localization.md"]),

"project-rules": page("把项目约定写成可随仓库维护的规则，让 ACECode 在理解和修改代码时遵循相同的边界。", [
    section("files", "项目规则文件",
        '''<p>ACECode 默认识别 <code>AGENT.md</code>、<code>AGENTS.md</code> 和 <code>CLAUDE.md</code>。同一目录按配置中的文件名顺序选用第一个匹配文件，默认顺序如上；不会把同目录三个文件无条件全部合并。</p><p>加载先读取用户数据目录中的全局规则，再按项目目录层级从外到内收集。是否读取、文件名顺序及内容长度都受配置控制，CLAUDE.md 还受独立读取开关控制。</p><p>规则适合记录目录职责、构建测试命令、编码风格、禁止修改的区域和交付标准。不要把临时任务状态、长日志和密钥放进去，冗长规则也会占用模型上下文。</p>'''),
    section("init", "使用初始化命令",
        '''<p>在正确的项目中提交 <code>/init</code>，让 ACECode 分析仓库并生成或改进 <code>AGENT.md</code>。它会调用模型，可能需要阅读文件和写入确认；生成后仍应检查其中的目录与命令是否真实可用。</p>''',
        code("/init", "在 ACECode 输入框中提交"),
        '''<p>已有团队维护的 AGENTS.md 时，先明确希望更新哪个文件，避免无意引入优先级更高但内容重复的 AGENT.md。桌面/Web 选择命令只把文本填入输入框，提交后才执行。</p>''',
        figure("CF-04", "初始化后的项目规则检查", "展示提交 /init 的位置以及生成规则的文件预览，标出构建命令、测试命令和禁止修改范围。")),
    section("example", "一份小而明确的规则",
        code("项目使用 Python，业务代码放在 src/，测试放在 tests/。\n修改前先阅读相关模块与测试；不要修改 generated/ 下的生成文件。\n保持现有公开接口，新增行为需要相应测试。\n完成后运行项目 README 中记录的验证命令，并报告未执行的检查。", "AGENT.md 内容示例"),
        '''<p>把示例中的语言、路径和验证命令替换成自己的项目约定。稳定的个人偏好放在个性化或记忆中，可复用的操作流程整理为<a href="skills.html">技能</a>；本项目特有的约束留在项目规则里。</p>''')
], ["src/project_instructions/instructions_loader.cpp", "src/config/config.hpp", "src/commands/builtin_commands.cpp", "docs/user-manual.md"]),

"memory": page("个性化指令用于持续表达偏好，记忆用于保存可复用的信息。让保存的内容简短、明确，并随实际情况更新。", [
    section("instructions", "设置个性化指令",
        '''<p>进入<strong>设置 &gt; 个性化 &gt; 自定义指令</strong>，填写稳定的工作习惯、输出语言或协作要求。离开编辑区后等待“已保存”提示；保存失败时先处理错误再离开。</p><p>自定义指令保存在当前用户配置中，会参与模型请求。项目特有的约定更适合写进项目规则，避免让其他项目继承不适合的目录或技术栈要求。</p>''',
        code("用简体中文解释结果。\n修改代码前先阅读相关实现。\n完成后说明改动与验证结果；没有执行的检查请明确写出。", "自定义指令示例"),
        figure("CF-05", "个性化指令与保存反馈", "展示自定义指令编辑区和已保存状态，使用通用示例，不展示个人或公司私密信息。")),
    section("memory", "保存和查看记忆",
        '''<p>需要记住长期信息时，可以在任务中明确提出要求，例如“记住：我希望解释结果时先给结论，再给验证依据”。阅读执行结果，确认实际保存了什么，避免把一次聊天中出现的信息都当作已经持久化。</p><p>默认记忆目录是 <code>~/.acecode/memory/</code>，其中 <code>MEMORY.md</code> 为索引，具体条目分别保存为 Markdown 文件。TUI 的 <code>/memory</code> 用于列出记忆，更多操作可查看 <code>/memory help</code>。</p><p>记忆与聊天记录不同：恢复任务读取该任务的上下文，持久记忆用于后续工作重用信息。不要把整个项目源码或完整对话复制为一条记忆。</p>'''),
    section("maintenance", "更新与删除过时信息",
        '''<p>当工作习惯或项目信息变化时，明确告诉 ACECode 要更新哪条记忆以及新内容。删除时同样指定对象，先查看内容再操作；TUI 的记忆命令提供查看、编辑、忘记和重新加载入口。</p><p>适合长期保存的是稳定偏好、反复需要的项目说明和可复用经验。账号密钥、临时验证码、大段日志和未经确认的推测不适合作为普通记忆保存。分享备份前检查记忆与配置中的私密内容。</p><p>如果某条旧信息反复影响回答，先检查个性化指令、项目规则和记忆三个来源，修正对应来源后再开始新任务验证。</p>''')
], ["web/src/components/SettingsPage.jsx", "src/memory/memory_paths.cpp", "src/commands/memory_command.cpp", "src/tool/memory_read_tool.cpp", "src/tool/memory_write_tool.cpp"])
}

from group4_extensions import PAGES as EXTENSIONS
PAGES.update(EXTENSIONS)
