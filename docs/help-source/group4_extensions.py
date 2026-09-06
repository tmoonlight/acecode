from common import page, section, code, table, note, figure

PAGES = {
"tools": page("工具让模型读取项目、修改文件和执行命令。先了解工具的作用，再根据任务选择合适的权限范围。", [
    section("inventory", "查看当前可用工具",
        '''<p>TUI 使用 <code>/tools</code> 打开能力中心查看工具。脚本环境可执行 <code>acecode -p --list-tools</code> 获取当前可发现的工具清单。工具是否出现受构建平台、功能开关、外部依赖和当前运行环境影响。</p><p>桌面/Web 的<strong>设置 &gt; 工具</strong>目前主要展示 Agent 浏览器相关状态，并不是所有内置工具的完整开关列表。查看一次任务真正使用了哪些工具，应展开该任务中的工具调用和结果。</p>''',
        table(["类别", "典型用途"], [["文件与搜索", "读取、写入、精确编辑文件；按路径模式或文本搜索项目。"], ["Shell", "执行构建、测试和其他本机命令。"], ["任务组织", "维护待办、规划工作、派遣子代理和汇总完成结果。"], ["扩展能力", "按环境提供网页搜索、图片理解、LSP、MCP 或 Agent 浏览器工具。"]]),
        figure("CF-06", "工具调用与执行结果", "展示一次文件读取、一次编辑和一次测试命令，标出工具参数、运行状态与结果展开入口。")),
    section("use", "让工具服务于明确的目标",
        code("先阅读登录相关代码和现有测试，定位失败原因。只修改相关文件，运行对应测试，并说明每项改动的目的。", "发送给 ACECode 的任务示例"),
        '''<p>通常直接描述目标即可，模型会选择工具，不必在聊天里手写工具 JSON。读取文件与运行命令是不同动作；默认权限下出现确认时，应核对实际路径、命令及影响范围。</p><p>文件编辑通常要求目标片段与当前文件精确匹配。出现找不到旧文本或匹配不唯一的结果时，让 ACECode 重新读取文件并缩小修改范围。不要用覆盖整个文件来掩盖没有理解原内容的问题。</p>'''),
    section("results", "理解失败与验证结果",
        '''<p>工具执行失败会将错误反馈给模型。可执行文件缺失、路径不正确、权限拒绝和测试失败应分别处理；“工具成功执行”只说明调用完成，不等于功能已经正确。</p><p>内置文件工具支持处理多种常见文本编码并保留适用的编码、BOM 和换行信息。遇到二进制、特殊格式或乱码时，先确认文件类型与编码，不要反复尝试文本替换。</p><p>任务结束后结合<a href="git.html">文件差异</a>和测试结果检查修改。需要为外部系统增加能力时，参见 <a href="mcp.html">MCP 服务器</a>；需要复用操作流程时，参见<a href="skills.html">技能</a>。</p>''')
], ["src/tool/builtin_tool_registry.hpp", "src/headless/headless_options.cpp", "web/src/components/SettingsPage.jsx", "docs/user-manual.md"]),

"skills": page("技能把一类任务的说明、流程和辅助资源放在一起。安装后先确认能被发现，再在合适的任务中使用。", [
    section("install", "安装与使用技能",
        '''<p>桌面/Web 打开<strong>技能</strong>页面查看当前发现的技能、所属范围和启用状态；TUI 使用 <code>/skills</code>。关闭的技能不会作为普通可用技能参与使用。手动添加文件后刷新或重新加载技能，再检查清单。</p><ol><li>从可信来源取得完整技能目录，确认目录里有 SKILL.md。</li><li>将整个目录放入全局或项目技能根目录，保留 scripts、references 等相对路径。</li><li>重新加载并确认名称、描述和启用状态。</li><li>在任务中明确要求使用该技能，检查执行过程及所需依赖。</li></ol><p>如果当前安装提供技能安装相关的内置技能，也可以让 ACECode 帮你安装，并说明来源和安装范围。桌面技能列表不是一个保证包含所有第三方技能的在线商店。</p>''',
        code("/skills\n/skills reload", "TUI · 查看与重新加载"),
        '''<p>发现后的技能可由模型按任务匹配，也可以通过技能对应的斜杠入口显式选择。选择只确定要使用的流程，不代表脚本、网络或文件修改已经获得授权。无界面 CLI 需要额外通过 <code>--enable-skills</code> 启用相应技能。</p>''',
        figure("CF-07", "技能列表与使用入口", "展示全局和项目技能分组、名称、描述、启用状态，以及在输入框选择一个技能的过程。")),
    section("scope", "全局技能与项目技能",
        table(["范围", "原生目录", "适合的内容"], [["当前用户", "<code>~/.acecode/skills/</code>、<code>~/.agent/skills/</code>", "跨项目复用的工作流程。"], ["项目目录", "<code>.acecode/skills/</code>、<code>.agent/skills/</code>", "只与当前项目有关的约定和操作。"]]),
        '''<p>项目扫描会考虑工作目录向上到用户目录范围内的相关技能根目录。兼容读取开关 <code>skills.reuse_opencode</code> 开启时，还会扫描支持的 OpenCode、.agents 和 .claude 等兼容位置。额外目录可通过 <code>skills.external_dirs</code> 配置。</p><p>不要因为磁盘上存在某个工具的技能目录，就假定 ACECode 一定会读取它。以当前技能页实际显示的路径和范围为准。重复名称可能被去重，维护时最好避免不同来源使用相同名称。</p><p>项目技能可以与仓库一起维护；个人技能留在用户目录。引用脚本时使用技能目录内的相对路径，避免把一台计算机的绝对路径写进共享技能。</p>'''),
    section("author", "编写自己的技能",
        '''<p>先创建一个用途明确的目录，例如 <code>.acecode/skills/review-checklist/</code>，再编写 SKILL.md。文件开头的 YAML 元数据必须包含名称与描述，正文写清触发场景、操作步骤和验收方式。</p>''',
        code("---\nname: review-checklist\ndescription: 按当前项目约定审查代码变更并报告可复现的问题。\n---\n\n先阅读项目规则和当前差异。\n逐项检查行为、错误处理和已有测试。\n只报告能指出具体文件位置与影响的问题。\n未经要求不要修改代码；说明未能验证的部分。", "SKILL.md"),
        '''<p>复杂流程可以拆到 references 中，脚本放入 scripts，并在正文中明确何时读取或运行。把使用条件写在描述里，把操作细节留给正文。完成后用一个小任务验证它确实被发现、引用路径可用、结果符合预期。</p><p>技能不是独立安装包管理器。需要 Python、Node.js、浏览器或其他命令时，应在说明中列出依赖及验证方法。涉及外部账号时，不把密钥写入 SKILL.md。</p>''')
], ["docs/skills.md", "src/skills/skill_init.cpp", "src/config/config.hpp", "web/src/components/SettingsPage.jsx"]),

"mcp": page("通过 MCP 连接外部工具服务。先在 ACECode 所在计算机上准备依赖，再配置连接并检查工具是否真正加载。", [
    section("configure", "添加与配置",
        '''<p>打开<strong>设置 &gt; MCP 服务器 &gt; 服务器配置</strong>，编辑 JSON。此处直接填写以服务器名称为键的对象；手动编辑主配置文件时，则将这个对象放在 <code>mcp_servers</code> 字段中。</p><p>本地进程使用 <code>stdio</code>，通过 command、args 和 env 启动。远端服务按对方提供的协议使用 <code>sse</code> 或 <code>http</code>；后者用于 Streamable HTTP。不要只凭 URL 看起来像网页就猜测协议。</p>''',
        code('{\n  "local-tools": {\n    "transport": "stdio",\n    "command": "python",\n    "args": ["/absolute/path/to/mcp_server.py"],\n    "env": {}\n  }\n}', "设置页中的 JSON · 替换为已安装服务器的真实命令"),
        '''<p>上例仅说明结构，不会自动下载或创建 MCP 服务器。Windows 路径可使用正斜杠，或在 JSON 中正确转义反斜杠。启动命令必须能在运行 daemon 的用户与环境中找到。</p><p>离开编辑区后等待保存反馈；<strong>格式化</strong>用于整理有效 JSON。认证字段、HTTP 地址和超时应采用服务器提供的配置，不把模型 API Key 无条件复用给 MCP。</p>''',
        figure("CF-08", "MCP 配置与服务器开关", "展示有效 JSON、已保存状态、启用服务器列表以及实际连接或错误状态，遮挡 env、headers 或认证字段中的密钥。")),
    section("manage", "启停、重连与查看工具",
        '''<p>在<strong>启用服务器</strong>列表开关目标服务器。支持运行时应用时会立即切换；出现“重启 daemon 后生效”提示时，按提示重启后台。关闭某个服务器会影响依赖它的后续调用。</p><p><strong>重新加载</strong>重新读取配置文本，<strong>Reload</strong>用于提交并重新加载 MCP 运行状态。保存配置与建立连接是两个步骤：JSON 合法并不代表进程已经启动或远端已经完成初始化。</p>''',
        code("/mcp\n/mcp list\n/mcp enable local-tools\n/mcp disable local-tools\n/mcp reconnect local-tools\n/mcp help", "TUI · 使用自己的服务器名称"),
        '''<p>先检查服务器连接状态，再查看它实际暴露的工具。工具通常以带服务器前缀的名称注册，模型才能在合适的任务中调用。修改配置后仍看到旧工具时，重连并再次检查清单。</p><p>stdio 连接失败先验证可执行文件、参数、工作环境和依赖；网络连接失败再检查 URL、协议、认证与代理。无界面 CLI 默认不启用 MCP，需要通过 <code>--enable-mcp</code> 选择服务器。更多处理方法见<a href="troubleshoot-tools.html#mcp">MCP 连接或工具加载失败</a>。</p>''')
], ["web/src/components/SettingsPage.jsx", "src/commands/builtin_commands.cpp", "src/config/config.hpp", "docs/user-manual.md"]),

"lsp": page("LSP 为编辑过程补充语言服务器信息。它依赖项目本身的语言环境，帮助尽早发现代码问题。", [
    section("prepare", "准备语言服务器",
        '''<p>ACECode 会根据文件语言和项目标记选择语言服务器，通常在首次需要时启动。语言服务器及项目依赖需要在后台所在计算机上可用；安装 ACECode 不等于所有语言服务器已经安装。</p>''',
        table(["语言", "服务器命令", "常见项目条件"], [["C / C++", "<code>clangd</code>", "编译数据库或相应 clangd 配置。"], ["JavaScript / TypeScript", "<code>typescript-language-server --stdio</code>", "项目 TypeScript 与包管理依赖可用；Deno 项目有不同要求。"], ["Python", "<code>pyright-langserver --stdio</code>", "正确的 Python 环境与项目依赖。"], ["Go", "<code>gopls</code>", "go.mod 或 go.work 等项目标记。"], ["Rust", "<code>rust-analyzer</code>", "Cargo.toml 与 Rust 工具链。"]]),
        '''<p>从项目正常构建的环境启动 ACECode，有助于继承正确 PATH。Python 项目先确认虚拟环境；C/C++ 项目先生成正确的编译数据库。语言服务器安装命令和版本应遵循所用工具的官方说明。</p>'''),
    section("status", "查看连接与诊断",
        '''<p>TUI 使用 <code>/lsp</code> 或 <code>/lsp status</code> 查看状态。区分尚未启动、未安装和启动后损坏：尚未处理匹配文件时没有连接，不一定是故障。</p><p>文件编辑后，ACECode 可把语言服务器返回的 ERROR 级别诊断加入工具结果，让模型继续修复。普通警告不会全部当作错误注入。没有诊断可能表示代码没有错误，也可能是该文件尚未得到有效的语言服务。</p>''',
        figure("CF-09", "LSP 状态与编辑后的错误", "展示 /lsp status 的服务器状态和一次编辑结果中的 ERROR 诊断，标出文件、行号及错误信息。")),
    section("verify", "确认修复真正生效",
        '''<p>先检查报错文件是否属于正确项目、依赖是否完整、语言服务器能否从当前环境启动。修复安装或启动故障后重新运行 ACECode，再处理目标文件并观察状态。</p><p>LSP 诊断用于编辑反馈，不代替编译器、单元测试和实际运行。完成修改后仍应运行项目要求的验证命令。<code>lsp.enabled</code> 控制此能力；关闭后不能依赖自动诊断反馈判断代码正确性。</p>''')
], ["src/lsp/lsp_server_registry.cpp", "src/config/config.hpp", "docs/user-manual.md"]),

"hooks": page("Hooks 在会话和工具执行的特定时机运行本地命令，适合检查规则或补充上下文。启用前先审查实际执行内容。", [
    section("events", "选择事件与配置位置",
        '''<p>支持的时机包括会话开始、提交提示、工具调用前后、权限处理、上下文压缩前后与停止。配置中的事件名称如 <code>SessionStart</code>、<code>PreToolUse</code> 和 <code>PostToolUse</code> 区分大小写。</p><p>全局配置读取 <code>~/.acecode/hooks.json</code> 与 <code>~/.codex/hooks.json</code>；项目配置读取当前受信任工作区中的 <code>.acecode/hooks.json</code> 与 <code>.codex/hooks.json</code>。各个已加载来源会合并，而非只保留一个文件。</p><p>目前支持 command 处理器。prompt、agent 与 async 类型不会按其他产品的实现自动运行，而会显示跳过或诊断信息。总开关 <code>features.hooks</code> 可以关闭这套兼容生命周期 Hooks；旧版启动钩子另有兼容行为。</p>'''),
    section("example", "配置一个工具调用检查",
        '''<p>下面以 Bash 工具调用前运行本地检查脚本为例。先在项目中准备并自行验证脚本，然后把定义合并到项目 Hooks 文件。</p>''',
        code('{\n  "hooks": {\n    "PreToolUse": [{\n      "matcher": "Bash",\n      "hooks": [{\n        "type": "command",\n        "command": "python ./hooks/check_tool.py",\n        "commandWindows": "py ./hooks/check_tool.py",\n        "timeout": 30\n      }]\n    }]\n  }\n}', ".acecode/hooks.json"),
        code("import json\nimport sys\n\nrequest = json.load(sys.stdin)\n# 在这里检查 request 中的事件和工具参数。\n# 退出码 0 且不输出内容，表示检查通过且不附加行为。\nsys.exit(0)", "hooks/check_tool.py · 最小接入示例"),
        '''<p>命令从标准输入接收一条 UTF-8 JSON 事件。timeout 的单位是秒；Windows 可以单独配置 commandWindows。需要阻止操作或添加上下文时，应使用该事件支持的结构化输出，不能给所有事件套用同一种返回对象。</p>'''),
    section("trust", "审核、启停与排错",
        '''<ol><li>进入<strong>设置 &gt; 钩子</strong>并刷新发现结果，TUI 也可用 <code>/hooks</code> 查看。</li><li>检查来源路径、命令、匹配条件和诊断。</li><li>对确认可信的待审核项执行信任操作。</li><li>触发一次对应的小操作，核对实际结果。</li></ol><p>新的非托管 Hook 处于待审核状态时会被跳过。定义改变后需要重新审核；受管理策略控制的 Hook 会显示相应限制。可以禁用或重新启用普通 Hook，但重新启用不能替代尚未完成的信任审核。</p>''',
        figure("CF-10", "Hooks 来源与信任审核", "展示待审核 Hook 的来源、命令和信任入口，以及已启用和已禁用状态；不包含真实凭据。"),
        '''<p>Hook 没有执行时，先看开关、项目是否受信任、事件和 matcher 是否匹配。脚本失败时检查解释器、工作目录、退出码及超时。不要通过反复放宽工具权限来解决脚本本身的错误。</p>''')
], ["docs/hooks.md", "web/src/components/SettingsPage.jsx", "src/config/config.hpp"]),

"connectors": page("连接器用于管理安装环境中已经配置的外部集成。先查看当前列表和集成说明，再处理对应的启用或认证问题。", [
    section("list", "查看已配置连接器",
        '''<p>进入<strong>设置 &gt; 连接器</strong>查看名称、描述和启用状态；TUI 使用 <code>/connectors</code>。列表来自主配置中的 connectors 定义，具体项目取决于当前安装和配置。</p><p>显示“暂无已配置连接器”时，表示本地没有相应定义。当前页面提供已配置项的刷新和启停，不提供一个通用的连接器商店，也不会仅凭名称自动安装外部服务。</p>''',
        figure("CF-11", "已配置连接器列表", "展示一个实际配置的连接器名称、用途、开关和刷新入口；若环境为空，截取真实空状态并说明需要先完成集成配置。")),
    section("auth", "启用与认证的关系",
        '''<p>连接器可由安装配置提供首次启动认证命令。当前自动流程只在该数据目录的首次 daemon 启动阶段，对符合条件的启用项执行一次。修改开关、重复启动后台或收到 401 错误，不会自动反复拉起认证助手。</p><p>需要重新认证时，按对应集成提供的说明重新完成授权，并确认外部进程或服务已经恢复。旧配置中的 on_enable、on_auth_error 字段仅保留兼容读取，不代表切换开关或报错时会再次执行这些命令。</p>'''),
    section("distinguish", "确定应当配置哪种扩展",
        table(["目标", "对应功能"], [["调用远端工具或本地工具服务", "<a href=\"mcp.html\">MCP 服务器</a>"], ["安装可复用的任务流程", "<a href=\"skills.html\">技能</a>"], ["在生命周期事件中运行命令", "<a href=\"hooks.html\">Hooks</a>"], ["通过消息平台收发任务消息", "<a href=\"channels.html\">消息渠道与远程控制</a>"], ["管理安装环境提供的外部集成", "当前连接器列表及该集成的配置说明。"]]),
        '''<p>排错时记录连接器名称、触发动作和错误信息，再检查它依赖的程序或服务。启用状态只能说明本地配置允许使用，不能代替对外部服务连通性和认证的确认。</p>''')
], ["src/config/config.hpp", "web/src/components/SettingsPage.jsx", "docs/user-manual.md"]),
}
