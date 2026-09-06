from common import page, section, code, table, note, figure

PAGES = {
"model-basics": page("先区分服务商、模型和已保存预设，才能正确选择连接方式并在任务中切换模型。", [
    section("concepts", "三个名称分别表示什么",
        table(["名称", "含义", "例子"], [["服务商 / Provider", "决定请求协议、连接地址、认证方式及可配置字段。", "ACEModel、GitHub Copilot、自定义 OpenAI 兼容接口。"], ["Model ID", "服务端识别的模型名称，必须与接口实际支持的 ID 一致。", "从目录或探测结果选择，也可以手动填写。"], ["预设名称", "你保存这组连接配置时使用的名称。", "日常编程、项目审查等便于自己辨认的名字。"]]),
        '''<p>一个服务商可以提供多个模型，同一个 Model ID 也可以保存为不同预设，分别使用不同连接或参数。输入框中的模型选择器使用<strong>已保存模型</strong>；在目录中看到一个模型，不代表它已经加入任务可选列表。</p>'''),
    section("setup", "从目录到任务模型",
        '''<ol><li>进入<strong>设置 &gt; 模型</strong>，点击<strong>新增模型</strong>。</li><li>选择 Provider。自定义接口、需要 API Key 的服务商、设备登录服务商的表单不同。</li><li>填写连接信息，选择或输入 Model ID，按需设置预设名称。</li><li>保存模型，回到任务，在输入框下方选中这个预设。</li><li>发送简短请求验证连接，再开始正式代码任务。</li></ol>''',
        note("目录信息不是连接测试", "本地目录提供名称和能力元数据，保存预设记录配置。实际请求是否成功还取决于凭据、账号权限、服务可用性和网络。"),
        figure("MD-01", "服务商、模型 ID 与预设", "展示新增模型表单和保存后的模型列表，分别标出 Provider、Model ID、预设名称以及任务模型选择器中的对应项。")),
    section("choose", "怎样选择一个合适的预设",
        '''<p>先确定任务需要的能力：代码修改通常需要工具调用，截图分析需要视觉支持，长项目分析需要足够的上下文。再确认服务商实际开放的模型和限制。界面中的能力图标是配置依据，不能代替服务端实际支持。</p><p>配置过程见<a href="providers.html">接入其他服务商</a>，已有配置的日常维护见<a href="manage-models.html">添加与管理模型</a>。</p>''')
], ["web/src/components/model-settings/ModelSettingsSection.jsx", "web/src/components/model-settings/ModelProfileDialog.jsx", "web/src/components/model-settings/SavedModelList.jsx", "web/src/lib/modelManager.js"]),

"acemodel": page("ACEModel 是 ACECode 内置目录中的服务商入口。选定模型并保存连接后，就可以在任务中使用。", [
    section("catalog", "内置模型目录",
        '''<p>在新增模型窗口中选择<strong>ACEModel</strong>。当前内置目录提供以下模型；保存时以界面显示的 Model ID 为准，显示名称的大小写不等于接口 ID 的大小写。</p>''',
        table(["显示名称", "Model ID"], [["Moonlight", "<code>moonlight</code>"], ["Starrylight", "<code>starrylight</code>"], ["Aurora", "<code>aurora</code>"]]),
        '''<p>内置目录是随 ACECode 提供的配置起点，独立于其他外部服务商目录。若使用同名模型的其他兼容服务，请选择对应的自定义 Provider 并填写那个服务的连接信息，避免把不同服务的名称与凭据混用。</p>'''),
    section("connect", "保存并开始使用",
        '''<ol><li>进入设置的模型页并点击新增模型。</li><li>选择 ACEModel，确认表单中的连接地址。</li><li>按账号获得的连接信息填写 API Key，选择需要的模型。</li><li>填写便于辨认的预设名称，或使用表单自动生成的名称。</li><li>保存后在任务模型选择器中选中它，发送一次简短请求。</li></ol><p>不清楚某项限制时，先保留界面提供的参数，再核对服务实际支持的配置。不要为了消除超限报错随意增大上下文窗口。</p>''',
        figure("MD-02", "ACEModel 的模型选择", "展示 ACEModel Provider 和三个内置 Model ID，以及保存后的预设。API Key 必须隐藏，避免截入账号信息。")),
    section("verify", "验证能力与连接状态",
        '''<p>检查已保存模型的视觉、工具等能力标识，然后用真实的小任务验证所需功能。</p><p>认证失败时检查密钥和实际地址；模型不存在时核对 Model ID；有回答但不能完成工具任务时检查工具能力与权限。排查步骤见<a href="troubleshoot-models.html">模型连接与认证问题</a>。</p>''')
], ["src/provider/builtin_model_catalog.cpp", "src/config/saved_models.cpp", "web/src/components/model-settings/ModelProfileDialog.jsx", "web/src/components/model-settings/ProviderCatalogPicker.jsx"]),

"providers": page("按实际接口协议选择 Provider，再填写服务给出的连接信息。不同服务商的认证方式和可选参数分别配置。", [
    section("openai", "OpenAI 兼容接口",
        '''<ol><li>进入<strong>设置 &gt; 模型 &gt; 新增模型</strong>，选择自定义 OpenAI 兼容入口，或目录中对应的服务商。</li><li>填写 <strong>Base URL</strong> 和 <strong>API Key</strong>，输入服务实际支持的 <strong>Model ID</strong>。</li><li>需要完整请求地址时，在高级设置中切换<strong>端点模式</strong>。</li><li>保存后选择这个预设，发送简短请求验证。</li></ol>''',
        table(["端点模式", "该填写什么"], [["Base URL", "API 基础地址，例如 <code>https://api.example.com/v1</code>；由客户端构造对应请求端点。"], ["完整端点", "服务要求的完整请求 URL；不要再按基础地址的方式重复拼接路径。"]]),
        '''<p>这里的示例域名不是可用服务。请复制服务商给出的实际地址；同一平台可能有不同协议的入口。需要额外认证头时，在高级设置填写<strong>自定义请求头 JSON</strong>，保持合法 JSON 对象。</p><p>支持探测时可以点击<strong>探测模型</strong>读取服务返回的模型 ID。无法列出模型的兼容服务仍可手填 Model ID，随后用真实请求验证。</p>''',
        figure("MD-03", "自定义兼容接口填写示例", "标出 Base URL、Model ID、隐藏的 API Key 和高级设置中的端点模式。使用 example.com 等占位域名，不展示真实密钥。")),
    section("anthropic", "Anthropic",
        '''<p>选择 Anthropic 协议的 Provider，填写对应 Messages API 的地址、密钥和模型 ID。不要仅因模型名称中包含 Claude 就选择 Anthropic；若服务提供的是 OpenAI 兼容代理，应按它实际提供的协议选择。</p><ol><li>确认账号或代理提供哪种协议和认证方式。</li><li>选择匹配的 Provider，再填入该接口地址与凭据。</li><li>从可用目录选择模型，或填写准确的模型 ID。</li><li>仅在表单支持时配置推理开关、强度或预算。</li><li>保存后先进行一次普通文本请求，再验证工具任务。</li></ol><p>推理字段因模型而异，必需推理的模型不能随意关闭。遇到无效参数错误，先核对该模型的高级设置，而不是不断更换同一地址下的名称。</p>'''),
    section("copilot", "GitHub Copilot",
        '''<p>GitHub Copilot 使用设备登录流程。模型页的<strong>模型连接</strong>卡片提供<strong>连接 GitHub</strong>，认证由该入口管理，模型弹窗不要求手动填写受管端点和密钥。</p><ol><li>点击连接 GitHub，查看设备验证码。</li><li>在打开的系统浏览器中完成 GitHub 授权。</li><li>回到 ACECode，按需要点击<strong>我已完成授权</strong>，确认状态为已连接。</li><li>新增一个 GitHub Copilot 模型预设并保存。</li><li>在任务中选择预设，验证账号可以访问该模型。</li></ol><p>验证码过期或授权未完成时重新发起登录。已连接只表示认证流程成功，实际模型访问仍由账号权限和服务状态决定。需要切换账号时使用连接卡片中的退出连接。</p>''',
        figure("MD-04", "GitHub 设备登录", "展示模型连接卡片中的连接状态、验证码位置和我已完成授权按钮；验证码使用失效的示例值。"))
], ["web/src/components/model-settings/ModelProfileDialog.jsx", "web/src/components/model-settings/ModelConnectionCard.jsx", "web/src/components/model-settings/ModelSettingsSection.jsx", "src/provider/openai_provider.cpp", "src/provider/anthropic_provider.cpp"]),

"manage-models": page("管理可供任务使用的模型预设，并区分新任务默认值与当前任务的模型选择。", [
    section("crud", "添加、编辑与删除",
        '''<p>模型页的<strong>已保存模型</strong>列表支持搜索、刷新和新增。每条预设显示名称、Provider、Model ID 与能力标识，可编辑、设为默认或删除。</p><ol><li><strong>添加：</strong>点击新增模型，选择 Provider 和 Model ID，填写连接信息，保存模型。</li><li><strong>批量添加：</strong>新增时，支持多选的模型目录或探测窗口可选择多个 ID，确认后保存对应预设。</li><li><strong>编辑：</strong>打开已有预设，修改需要的字段，再点击保存修改。编辑时保留原有 Provider 身份。</li><li><strong>删除：</strong>核对弹窗中的名称后删除预设；会话记录仍保留。</li></ol>''',
        '''<p>已保存密钥不会明文回填。编辑时不填写新密钥通常保留原凭据；需要删除凭据时勾选<strong>清除已保存密钥</strong>。新增时出现<strong>复用已有凭据</strong>可选择来源预设，密钥复制由后台完成。</p><p>名称冲突时，界面提供覆盖已有预设、另存为或取消。不要为绕过冲突随意覆盖还在使用的连接。运行中的会话使用该预设时，删除可能显示<strong>会话使用中</strong>并被阻止，先结束或切换相关任务。</p>''',
        figure("MD-05", "已保存模型与编辑操作", "展示默认标识、编辑与删除按钮，以及名称冲突弹窗的覆盖、另存为和取消选项。")),
    section("defaults", "默认模型与会话模型",
        '''<p>在已保存模型列表中<strong>设为默认</strong>，决定新任务使用的默认预设。已经打开的任务有自己的模型选择，可在输入框下方切换。不要把修改默认值理解成自动改写每条历史任务选择的预设名称。</p><p>编辑一个预设则是在更新该连接的地址、认证和能力参数。历史任务继续使用这个预设时，应使用更新后的配置；若页面仍显示旧信息，刷新模型列表并检查实际选中的预设。正在执行的请求已发出，不会把新参数倒回已经完成的请求。</p>''',
        table(["TUI 命令", "生效范围"], [["<code>/model NAME</code>", "切换当前会话。"], ["<code>/model --cwd NAME</code>", "切换并保存当前工作目录的模型覆盖。"], ["<code>/model --default NAME</code>", "切换并更新全局默认模型名称。"]]),
        '''<p>这些 NAME 使用<strong>预设名称</strong>。脚本中的 <code>acecode -p --model NAME</code> 同样如此；print 模式恢复会话而没有显式传模型时，会保留会话已保存的模型选择。</p>''')
], ["web/src/components/model-settings/SavedModelList.jsx", "web/src/components/model-settings/ModelSettingsSection.jsx", "web/src/components/model-settings/ModelProfileDialog.jsx", "web/src/components/ChatView.jsx", "src/config/saved_models.cpp", "docs/user-manual.md"]),

"model-capabilities": page("让模型参数与实际接口相符。能力标识告诉 ACECode 如何组织请求，不会替服务端增加原本没有的能力。", [
    section("limits", "上下文窗口与输出限制",
        '''<p>在新增或编辑模型的<strong>高级设置</strong>中查看<strong>上下文窗口 Token</strong>和<strong>最大输出 Token</strong>。窗口影响上下文预算与自动压缩判断；输出限制约束单次生成的规模，两个字段不能互相替代。</p><p>已有目录值时先保留目录提供的参数；需要覆盖时使用服务实际支持的值。接口限制可能比模型家族的宣传窗口更小。服务返回上下文超限时，先压缩历史或减小输入，不要只把本地数字改大。</p>'''),
    section("abilities", "思考、视觉与工具调用",
        table(["能力", "实际用途", "验证方法"], [["推理 / 思考", "按 Provider 支持的字段发送推理开关、强度或预算。", "使用支持推理的模型，检查请求是否正常完成。"], ["视觉", "处理图片内容；可与可用视觉工具配合。", "提交一张内容明确的示例图片并问具体问题。"], ["工具调用", "允许模型发出结构化工具请求来读写文件或执行命令。", "先要求读取一个示例文件，再检查工具记录。"]]),
        '''<p>只有当前 Provider 支持的配置才会出现在表单中。有些模型要求推理始终启用；有些提供强度选项，有些使用 token 预算。勾选视觉或工具能力只是声明请求方式，不能让不支持该协议的模型自动具备能力。</p><p>权限模式和模型能力分别管理：模型能提出工具调用，不代表所有工具都已安装或所有操作都已获准。</p>''',
        figure("MD-06", "模型高级设置", "展示上下文窗口、输出限制、能力标识和推理设置；选取确实支持这些字段的 Provider，避免画出实际不存在的选项。")),
    section("probe", "能力探测",
        '''<p>模型表单中的<strong>探测模型</strong>主要获取当前 Provider 返回的模型 ID 与可用元数据。有已保存的本地结果时，<strong>查看探测结果</strong>会打开缓存；需要重新访问服务时点击<strong>重新探测</strong>。</p><ol><li>先填写可用的地址、凭据和必要请求头。</li><li>主动点击探测模型，等待结果。</li><li>用搜索过滤 Model ID；新增时可多选，编辑时选择一个替换当前 ID。</li><li>确认回填后检查参数并保存预设。</li><li>用小请求验证实际需要的文字、视觉或工具功能。</li></ol>''',
        note("探测结果的边界", "能列出模型不等于已经实测每项能力。目录和缓存中的标识可能不包含服务的全部限制；正式任务仍以实际请求结果为准。"),
        figure("MD-07", "查看本地探测结果与重新探测", "展示探测结果搜索框、模型选择、重新探测按钮和添加所选模型按钮，图注区分读取缓存与主动请求服务。"))
], ["web/src/components/model-settings/ModelProfileDialog.jsx", "web/src/components/model-settings/ModelProbeDialog.jsx", "web/src/components/model-settings/ProviderCatalogPicker.jsx", "src/provider/model_context_resolver.cpp"])
}
