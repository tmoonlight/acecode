from common import page, section, code, table, note, figure

PAGES = {
"build": page("从源码构建终端、后台与可选桌面程序。先准备工具链和前端产物，再配置 CMake。", [
    section("prepare", "准备源码与依赖",
        '''<p>需要 Git、支持 C++17 的编译器、CMake 3.20 及以上、Ninja 和 vcpkg。Web 前端还需要可用的 Node.js 与 pnpm。Windows 使用正确架构的开发者终端；macOS 准备 Xcode 命令行工具；Linux 准备编译工具与目标平台依赖。</p>''',
        code("git clone --recurse-submodules https://github.com/tmoonlight/acecode.git\ncd acecode\ngit submodule update --init --recursive"),
        '''<p>仓库中的 vcpkg 清单管理 C++ 依赖，测试依赖由 tests feature 提供。不要把一个平台的 build 目录直接拿到另一平台继续使用；切换架构或工具链时使用新的构建目录。</p>'''),
    section("frontend", "先构建嵌入式 Web 资源",
        code("cd web\npnpm install\npnpm test\npnpm build\ncd .."),
        '''<p>CMake 会读取 web/dist 并把前端产物嵌入程序。修改前端后，需要重新生成这份产物，再重新配置并构建 C++。没有完整前端产物时，后台 API 可能仍能启动，但不会得到完整应用界面。</p><p>本帮助文档包与应用前端构建是两套产物：阅读 docs/help 不需要 pnpm，构建 ACECode 应用则需要准备其实际前端。</p>'''),
    section("compile", "配置、编译与测试",
        '''<p>下面是一条配置命令。把 vcpkg 路径和 triplet 替换为本机实际值；示例使用 Linux x64。Windows 路径可以使用正斜杠并加引号。</p>''',
        code('cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-linux -DVCPKG_OVERLAY_PORTS=ports -DVCPKG_MANIFEST_FEATURES=tests -DBUILD_TESTING=ON\ncmake --build build --target acecode acecode_unit_tests\nctest --test-dir build --output-on-failure'),
        table(["目标平台", "常见 triplet"], [["Windows x64", "<code>x64-windows-static</code>"], ["Windows ARM64", "<code>arm64-windows-static</code>"], ["Linux x64 / ARM64", "<code>x64-linux</code> / <code>arm64-linux</code>"], ["macOS Intel / Apple silicon", "<code>x64-osx</code> / <code>arm64-osx</code>"]]),
        '''<p>交叉编译还需要相应工具链，不能只改 triplet 就假定可用。构建成功后运行产出的 acecode 显示版本，并用一个小任务或本地健康检查验证目标入口。</p>'''),
    section("desktop", "可选桌面构建与开发服务器",
        '''<p>在前面的配置命令中加入 <code>-DACECODE_BUILD_DESKTOP=ON</code>，再构建 <code>acecode-desktop</code> 与 <code>acecode</code>。桌面程序运行时需要配套后台及平台资源；Windows 依赖 WebView2，Linux 桌面还需要相应 WebKitGTK 环境。详细平台条件以仓库 CMake 和打包工作流为准。</p>''',
        code("cmake --build build --target acecode acecode-desktop"),
        '''<p>开发 Web 时，当前 Vite 配置将 /api 与 /ws 代理到 127.0.0.1:28080。因此为这个开发流程显式启动 28080 的独立后台，再在另一个终端运行 pnpm dev；它与未配置后台默认端口 12399 不同。</p>''',
        code("acecode daemon --foreground --port=28080", "终端一 · 使用刚构建的程序路径"),
        code("cd web\npnpm dev", "终端二 · 打开 Vite 输出的开发地址"),
        '''<p>开发服务器用于本地调试；发布包还需按项目打包流程收集资源并验证，不等同于复制一个编译出的可执行文件。</p>''')
], ["CMakeLists.txt", "vcpkg.json", "cmake/acecode_desktop.cmake", "web/vite.config.js", "web/README.md", ".github/workflows/test.yml", ".github/workflows/package.yml"]),

"architecture": page("ACECode 由共享智能体核心和多个使用入口组成。理解状态归属，有助于把修改放在正确模块。", [
    section("surfaces", "入口与核心职责",
        table(["模块", "当前源码位置", "职责"], [["终端 TUI", "<code>src/main.cpp</code>、<code>src/tui/</code>", "终端输入、渲染、交互确认和本地会话。"], ["无界面 CLI", "<code>src/headless/</code>", "参数、单轮执行、输出与退出状态。"], ["后台", "<code>src/daemon/</code>", "进程生命周期、会话托管与运行身份。"], ["HTTP / WebSocket", "<code>src/web/</code>", "路由、请求解析、响应与事件传输。"], ["Web 前端", "<code>web/src/</code>", "任务界面、设置、消息与面板状态。"], ["桌面壳", "<code>src/desktop/</code>", "原生 WebView、后台托管和平台桥接。"], ["智能体循环", "<code>src/agent_loop.cpp</code>", "模型请求、工具回合、进度与终态。"], ["共享能力", "<code>src/session/</code>、<code>src/provider/</code>、<code>src/tool/</code>", "会话持久化、模型适配和工具执行。"]]),
        '''<p>TUI 可以直接使用共享核心；桌面与 Web 通过后台管理会话。桌面原生浏览器和文件窗口通过桥接接入，不意味着所有浏览器客户端都具备同样能力。</p>''',
        figure("DV-01", "ACECode 运行架构图", "后续绘制真实架构图：TUI 与 CLI 连接共享核心，桌面/Web 连接 daemon，daemon 连接 SessionRegistry、AgentLoop、Provider、Tools 与持久化；标出桌面原生桥接。")),
    section("turn", "一次任务如何流转",
        '''<ol><li>入口接收用户输入与上下文，并定位目标会话和工作目录。</li><li>保存用户消息，按当前模型绑定构造请求。</li><li>Provider 将统一消息转换为对应服务协议，并返回文本、推理或工具调用。</li><li>工具经过权限与运行条件检查后执行，结果追加到会话。</li><li>智能体根据结果继续下一次模型调用，或生成最终回复。</li><li>终态、用量与消息通过事件更新前端，持久记录用于后续恢复。</li></ol><p>用户界面中的“排队成功”只是输入被接受，任务完成还需要观察对应回合的终态。模型重试和上下文压缩也会产生状态变化，不能把每一次 busy 变化都视为成功完成。</p>'''),
    section("boundaries", "修改时保持的边界",
        '''<p>可复用的解析、校验和状态机放在对应共享模块，不把终端渲染对象带入后台接口。HTTP 处理应复用路由解析与错误响应辅助函数，模型变更应经过当前会话模型绑定路径。</p><p>Web 源码在 web/src，构建结果由工具生成。后台会话状态与前端临时草稿分开维护；文件修改又有独立的磁盘副作用，不能只更新界面就视作状态持久化。</p><p>新增行为需要检查 TUI、CLI、Web 和桌面是否使用了不同入口。协议变化同步维护 API 说明，关键持久字段补兼容性验证。</p>''')
], ["ARCHITECTURE.md", "CMakeLists.txt", "src/main.cpp", "src/agent_loop.cpp", "src/session/session_registry.cpp", "src/tool/tool_executor.hpp", "src/provider/session_model_binding.hpp"]),

"daemon": page("独立 daemon 提供 HTTP 与 WebSocket 服务。根据个人后台、调试或系统服务场景选择生命周期。", [
    section("lifecycle", "启动、查看与停止",
        code("acecode daemon\nacecode daemon status\nacecode daemon stop", "独立后台的常规生命周期"),
        '''<p>不带子命令表示后台启动，start 是兼容别名。启动成功时输出实际 Web UI 地址。未指定 cwd 时使用执行命令的目录；需要其他起点时传入 <code>--cwd=路径</code>。</p><p>前台调试使用 <code>acecode daemon --foreground</code>，日志同时输出到终端。不要在仍有需要保留的活动任务时重复启动或停止后台；先检查状态和当前使用者。</p>'''),
    section("ports", "端口、运行目录与远程访问",
        '''<p>后台监听固定回环地址 127.0.0.1。默认端口为 12399，主配置中的 web.port 可覆盖默认值，命令行 --port 的优先级更高。端口冲突会拒绝启动，不会自动寻找其他端口。</p>''',
        table(["参数", "作用"], [["<code>--cwd=PATH</code>", "选择起始工作目录。"], ["<code>--port=N</code>", "覆盖本次 Web 监听端口。"], ["<code>--run-dir=PATH</code>", "隔离 PID、心跳、端口与 Token 等运行文件。"], ["<code>--static-dir=PATH</code>", "从指定目录提供前端静态资源。"], ["<code>--question-policy=ask</code>", "需要提问时等待用户回答。"], ["<code>--question-policy=deny</code>", "不等待问题回答，返回自动处理指引。"], ["<code>--question-policy=timeout:60</code>", "提问等待指定秒数，超时后按策略选择。"]]),
        '''<p>自定义运行目录的后台，执行 status 和 stop 时也应使用同一运行目录。隔离运行目录不会自动隔离全部全局配置和用户数据。桌面保留的 desktop-shared 目录由桌面生命周期管理，不应用作任意独立进程的目录。</p><p>远程 Web 使用独立受管理代理，按<a href="web.html#remote">远程 Web 流程</a>开启。完全访问权限不取消所有提问；无人值守服务应同时明确工具权限与问题策略。</p>'''),
    section("identity", "运行身份与日志",
        '''<p>普通运行文件位于数据目录的 run 下，包含 daemon.pid、daemon.port、daemon.guid、heartbeat 和 token。检查进程时联合看 PID、GUID、心跳时间与健康响应，不能只凭某个文件存在就认定后台健康。</p><p>日志按日期写入 logs/daemon-日期.log。共享问题描述时只提供必要片段并遮挡凭据。Token 是访问凭据，不能放到公开仓库或监控截图。</p><p>桌面托管后台另有协议与身份校验，兼容实例才会复用。它与独立命令行后台的回收范围不同，参见<a href="troubleshoot-desktop.html#desktop">桌面后台排错</a>。</p>'''),
    section("service", "Windows 服务模式",
        '''<p>需要开机后在用户登录前运行时，可在管理员 PowerShell 中安装服务。服务以 LocalSystem 身份运行，使用 ProgramData 下的数据，不自动沿用个人用户的模型配置。</p>''',
        code("acecode service install\nacecode service start\nacecode service status", "管理员 PowerShell · 安装与启动"),
        '''<p>先为服务身份准备正确配置和可访问目录，再验证健康与模型请求。个人映射盘、环境变量和凭据可能不适用于 LocalSystem。维护时先停止服务，确认不再需要该服务后卸载：</p>''',
        code("acecode service stop\nacecode service uninstall", "管理员 PowerShell · 停止与卸载服务"),
        '''<p>移除服务注册不等于删除所有项目与用户数据。需要清理时按明确的数据目录逐项备份和处理。</p>''')
], ["src/daemon/cli.cpp", "src/daemon/service_win.cpp", "src/daemon/worker.cpp", "docs/daemon-api.md", "src/utils/paths.cpp"]),

"api": page("先通过 HTTP 创建会话，再订阅事件并提交消息。响应码、回合终态与重连游标需要分别处理。", [
    section("connect", "连接与鉴权",
        '''<p>基础地址采用 daemon 实际输出的地址。本机同源回环请求可不带 Token；远程或代理来源必须带认证。HTTP 使用 <code>X-ACECode-Token</code> 请求头，浏览器 WebSocket 使用 URL 的 token 参数。</p><p>先调用 <code>GET /api/health</code> 确认进程 GUID、版本、工作目录和能力。健康接口可以访问只证明该后台响应，不证明模型服务和外部工具均已可用。</p>''',
        table(["HTTP 状态", "处理方向"], [["400", "JSON、字段、参数或状态不满足要求。"], ["401", "缺少 Token 或 Token 不匹配。"], ["404", "会话、工作区、资源或路由不存在。"], ["409", "工作区不可用、正在运行或其他状态冲突。"], ["503", "对应后端能力或运行组件不可用。"]]),
        '''<p>错误正文通常包含 error，并可能提供 message。客户端同时保留状态码和可读错误，不将不同错误统一丢弃为一个连接失败提示。</p>'''),
    section("http", "创建会话与提交消息",
        table(["顺序", "请求", "关键内容"], [["1", "<code>POST /api/workspaces</code>", "<code>{\"cwd\":\"C:/repo\"}</code>；返回工作区 hash。"], ["2", "<code>POST /api/workspaces/:hash/sessions</code>", "可选 model 和 permission_mode；返回 session_id。"], ["3", "订阅目标会话的 WebSocket", "取得 subscribe_ack 后监听事件。"], ["4", "<code>POST /api/sessions/:id/messages</code>", "<code>{\"text\":\"说明项目入口\"}</code>；返回 202 queued。"], ["5", "<code>GET /api/sessions/:id/messages</code>", "按接口约定获取持久消息快照，配合实时事件显示。"]]),
        '''<p>model 使用已保存的预设名称。创建时 auto_start 默认关闭，只有提供 initial_user_message 并显式开启时才自动执行。下面的 Python 示例只创建一个默认权限会话，不发送模型请求。</p>''',
        code('import json\nimport os\nfrom urllib.request import Request, urlopen\n\nbase = os.environ.get("ACECODE_BASE_URL", "http://127.0.0.1:12399").rstrip("/")\ntoken = os.environ.get("ACECODE_TOKEN", "")\nproject = os.environ["ACECODE_PROJECT"]  # 后台所在机器中的绝对路径\n\ndef post(path, value):\n    headers = {"Content-Type": "application/json"}\n    if token:\n        headers["X-ACECode-Token"] = token\n    request = Request(base + path,\n                      data=json.dumps(value).encode("utf-8"),\n                      headers=headers, method="POST")\n    with urlopen(request, timeout=30) as response:\n        return json.load(response)\n\nworkspace = post("/api/workspaces", {"cwd": project})\nsession = post("/api/workspaces/" + workspace["hash"] + "/sessions",\n               {"permission_mode": "default"})\nprint(session["session_id"])', "Python · 先设置 ACECODE_PROJECT，远程连接还需设置地址与 Token"),
        '''<p>202 queued 只表示接受输入。可选 client_message_id 用于前端乐观消息与持久记录对齐，<strong>不提供服务端执行去重</strong>；网络超时后不要无条件重发同一个有副作用的请求。</p>'''),
    section("websocket", "订阅、重连与终态",
        '''<p>当前前端使用 <code>/ws/sessions/_multiplex?token=...</code>。路由末尾不是自动绑定的会话 ID，连接打开后仍需发送订阅消息：</p>''',
        code('{"type":"subscribe","payload":{"session_id":"sid","since":0}}', "WebSocket 客户端消息 · sid 替换为实际 ID"),
        '''<p>服务端事件包含 type、session_id、payload，通常还有每会话独立递增的 seq 和 timestamp_ms。文本、工具、用量、问题及回合状态都通过不同事件传输，不能只监听 token。</p><p>客户端保存已经处理的序号，重连后用 since 请求续传。订阅确认后补发的待处理权限与问题快照可能没有 seq，应按 request_id 去重，不推进序号游标。处理关闭事件后保留该轮已解决标记，防止迟到快照重新弹出旧请求。</p><p>普通回合用 turn_id 标识。busy_changed 的结束状态和后续 done 会带 outcome，只有 completed 表示成功；error 与 aborted 需要分别显示。重试产生的 transcript_replace 应按替换语义处理，避免保留失败尝试的临时输出。</p>'''),
    section("interactions", "回答问题与取消",
        code('{"type":"decision","payload":{"session_id":"sid","request_id":"request-id","choice":"allow"}}\n{"type":"abort","payload":{"session_id":"sid"}}', "独立消息示例 · 仅在用户作出相应决定时发送"),
        '''<p>权限选择字段是 choice，可为 allow、deny 或 allow_session。提问使用 question_answer，携带 request_id、cancelled 与 answers；每项回答使用 question_id、selected 和 custom_text。多会话连接必须带正确的 session_id。</p><p>收到 permission_closed 或 question_closed 时关闭对应交互。不要为了让集成看起来流畅而默默自动批准。完整路由、消息字段和 PTY 协议见仓库中的<a href="https://github.com/tmoonlight/acecode/blob/master/docs/daemon-api.md" target="_blank" rel="noopener noreferrer">Daemon API 协议说明</a>，接入时对照当前版本源码。</p>''')
], ["docs/daemon-api.md", "src/web/routes/routes_workspaces.cpp", "web/src/lib/api.js", "web/src/lib/connection.js", "src/web/server_helpers.cpp"]),

"extension-development": page("优先在现有模块边界内扩展能力，复用工具、命令和渠道协议，补齐相应的错误与生命周期处理。", [
    section("tools-commands", "添加工具与命令",
        '''<p>内置工具通常在 src/tool 下定义工厂函数，返回 ToolImpl：definition 描述名称、用途和 JSON 参数，execute 接收参数文本与 ToolContext，返回 ToolResult。is_read_only 必须符合真实副作用，不能为绕过确认把写操作标为只读。</p><ol><li>参考已有小工具，提取参数校验和核心逻辑。</li><li>复用 ToolArgsParser、错误、路径与会话辅助函数，限制作用范围。</li><li>注册到适用的工具清单；有环境依赖时只在能力可用时暴露。</li><li>提供可读结果与结构化摘要，必要时附带差异和元数据。</li><li>测试合法输入、缺参、错误路径、取消和权限边界。</li></ol>''',
        code("// 结构示意：具体定义与处理函数需在对应模块实现。\nToolImpl create_example_tool() {\n    ToolImpl impl;\n    impl.definition = make_example_definition();\n    impl.execute = execute_example;\n    impl.is_read_only = true; // 仅适用于确实没有写入副作用的实现\n    impl.source = ToolSource::Builtin;\n    return impl;\n}", "C++ · 工具工厂结构示意，不是可直接编译的完整工具"),
        '''<p>斜杠命令通过 CommandRegistry 注册 SlashCommand，处理函数接收 CommandContext 与参数。需要复用模型输入时使用已有提交辅助函数；界面专属操作通过对应回调进入 TUI 管理面板。</p><p>桌面/Web 的内置命令还经过独立的支持清单、请求解析和前端路由。只在 TUI 注册命令不会自动得到所有前端行为，修改时同时检查 builtin_command_handler、commands_handler 和 builtinCommandRouting。涉及持久数据或 API 的变动应同步协议与兼容性测试。</p>'''),
    section("channel", "渠道插件协议",
        '''<p>Channel v1 插件是独立可执行程序，用 manifest 描述启动方式。ACECode 每次生命周期请求启动一次该程序，向 stdin 写入一条以换行结束的 JSON，读取 stdout 上的 channel.status；标准输出不要混入调试日志。</p>''',
        code('{\n  "schema": "acecode.channel-plugin.v1",\n  "name": "example-channel",\n  "transport": "stdio",\n  "command": "/absolute/path/to/channel-helper",\n  "args": [],\n  "cwd": "/absolute/path/to/plugin",\n  "timeout_ms": 10000\n}', "channel-plugin.json · 使用实际可执行文件与工作目录"),
        '''<p>激活消息 channel.activate 包含 protocol_version、session_id、入站 URL、认证头与 Token、出站偏好和 settings。插件完成自身准备后返回连接状态及 Webhook 地址：</p>''',
        code('{\n  "type": "channel.status",\n  "state": "connected",\n  "already_running": false,\n  "binding_token": "opaque-current-binding",\n  "outbound": {\n    "mode": "webhook",\n    "url": "http://127.0.0.1:39001/messages"\n  }\n}', "激活结果示例 · 地址必须由插件实际提供"),
        '''<p>激活应当幂等。可选 binding_token 必须为非空字符串，ACECode 会在对应 channel.deactivate 中原样回传。插件解除绑定时同时核对 session_id 与 binding_token，防止延迟清理误断开新连接。</p><p>旧插件可以不返回 binding_token；返回空串或非字符串会被视为非法状态。daemon 正常关闭会保留托管绑定以便恢复，显式 /rc off 才走当前绑定的解除流程。不能把一次辅助进程退出等同于渠道服务已经永久关闭。</p><p>测试至少覆盖重复激活、失败返回、超时、同会话重新绑定、迟到解除和认证错误。协议细节见<a href="https://github.com/tmoonlight/acecode/blob/master/docs/channel-plugin-protocol.md" target="_blank" rel="noopener noreferrer">渠道插件协议</a>；用户配置入口见<a href="channels.html">消息渠道与远程控制</a>。</p>''')
], ["src/tool/tool_executor.hpp", "src/tool/task_complete_tool.cpp", "src/tool/builtin_tool_registry.hpp", "src/commands/command_registry.hpp", "src/web/handlers/builtin_command_handler.cpp", "web/src/lib/builtinCommandRouting.js", "src/remote_control/channel_plugin.cpp", "docs/channel-plugin-protocol.md"]),

"contributing": page("围绕一个明确问题提交聚焦的改动，保留现有行为边界，并提供能让维护者复现的验证结果。", [
    section("scope", "开始前明确范围",
        '''<p>阅读仓库 README、AGENTS.md、架构和相关模块文档，先复现问题并查找已有实现。使用独立分支处理一个主题，确认工作区中哪些是自己已有的修改，不覆盖其他未提交工作。</p><p>非简单行为变化按仓库要求先建立或继续 OpenSpec change，写清使用场景、行为与验证任务，再实施。小型文档或明确文案修正保持范围，不为了修一个词改动整套界面。</p><p>不要修改无关的第三方或子模块内容。根目录只保留规范文档，临时日志、截图和构建产物放到合适位置，不加入一次性的根目录报告。</p>'''),
    section("implement", "遵循模块与代码约定",
        '''<p>C++ 使用 C++17，遵守 .editorconfig 的编码、换行与缩进。共享逻辑放入对应 src 子系统，TUI 渲染留在终端模块，React/Vite/Tailwind 源码留在 web/src。</p><p>新增用户可见字符串检查国际化映射与目录，避免一个入口显示中文而另一个出现未翻译字符串。新文件操作复用路径、编码和错误处理辅助函数，新增会话字段检查序列化、恢复与旧记录兼容。</p><p>API 行为变化同步 docs/daemon-api.md；用户体验变化同步本帮助包的内容源并重新生成静态 HTML。不要直接编辑应用的生成产物来代替源码修改。</p>'''),
    section("verify", "按改动运行验证",
        table(["改动类型", "验证"], [["C++ 纯逻辑、解析与存储", "构建 acecode_unit_tests，运行对应测试与必要的完整测试。"], ["Web 源码", "在 web 运行 pnpm test 与 pnpm build。"], ["可见 TUI / 桌面交互", "在真实目标入口验证，并提供相关截图或终端记录。"], ["平台桥接与打包", "验证对应平台运行条件、资源和配套后台。"], ["静态帮助文档", "生成后检查目录、链接、搜索、图片占位和离线窄屏阅读。"]]),
        code("cmake --build build --target acecode_unit_tests\nctest --test-dir build --output-on-failure", "已完成测试构建配置时"),
        '''<p>测试应覆盖真实行为与错误边界。没有条件执行的平台检查在提交说明中明确标注，不把静态代码检查称为已经实际运行成功。</p>'''),
    section("submit", "提交变更与参与讨论",
        '''<p>提交信息简短描述具体动作。PR 说明首先交代问题与修改后的行为，再列验证结果、关联问题或 OpenSpec change。可见交互变化附相应画面，涉及 API 提供必要请求示例。</p><p>提交前检查差异，排除密钥、运行 Token、个人模型配置、会话记录、反馈包和无关产物。只提交当前任务需要的文件。</p><p>通过<a href="https://github.com/tmoonlight/acecode" target="_blank" rel="noopener noreferrer">ACECode 仓库</a>查看参与方式，通过<a href="https://github.com/tmoonlight/acecode/issues" target="_blank" rel="noopener noreferrer">Issues</a>报告可复现问题。仓库当前使用 MIT License；发布或复用时保留相应许可说明。</p>''')
], ["AGENTS.md", "ARCHITECTURE.md", ".editorconfig", "tests/CMakeLists.txt", "web/package.json", "docs/daemon-api.md", "LICENSE"]),
}
