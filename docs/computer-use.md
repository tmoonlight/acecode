# 电脑操控（Computer Use）

设置 → 工具 → 内置工具中的“电脑操控（实验性）”默认关闭。Windows 和 macOS 14 及以上版本的用户开启后，Agent 可以观察并操作本机交互桌面的应用窗口。macOS 还需授予辅助功能和屏幕录制权限。关闭后，新请求不再暴露这组工具，正在运行的执行器终止，历史工具调用也会被拒绝。配置保存在 `computer_use.enabled`，不需要重启 daemon。

点击右侧“配置”可展开鼠标指针样式：默认是右下角带 ACE 标识的主题色指针，也可以选择纯主题色指针。两种样式均跟随当前主题的强调色，默认蓝色主题显示蓝色。选择自动保存，关闭工具时也可配置，修改样式不会开启操控。预览跟随主题即时变色，样式选择以保存成功为准；后续实际操作和截图采用已保存的样式与主题色。

样式保存在 `computer_use.pointer_style`（`ace` / `plain`），解析后的主题色保存在 `computer_use.pointer_color`（`#rrggbb`）。前端在整个应用期间同步实际主题色，支持内置、下载及自定义主题；TUI 使用最近保存的颜色。外观更新不会使当前观察失效，模型参数不能覆盖 broker 注入的指针配置。

本次参考本地 Codex 的 Windows 工具能力与接入方式，自主实现 C++ 原生后端。Codex 仓库中的 MCP 桥接代码不包含其专有 `@oai/sky` 运行时，因此这里不依赖该插件或 JavaScript REPL。

### 与 Codex macOS 实现的关系

2026-09-24 核对了本机 Codex 应用包内 `cua_node/lib/node_modules/@oai/cua/dist/project/cua/sky_js/src/targets/mac/` 的 `client.js`、`get_app_state.js`、`window_result.js` 和 `native-pipe.js`。这些文件是 JavaScript 客户端，使用 `CodexComputerUseIPC-5` 调用独立的 `Codex Computer Use.app` 原生服务；它们不包含原生指针绘制代码。本机 CLI 源码和检查到的应用资源中均未找到该服务的原生实现。

| 核对项 | Codex 客户端中可确认的行为 | ACECode 当前实现 |
|---|---|---|
| 观察对象 | 按应用身份请求 skyshot，由原生服务返回截图和文字 | 按窗口请求截图、结构化 AX 控件树和一次性观察标识 |
| 动作入口 | `ComputerUseIPCAppPerformActionRequest`，包含应用身份与动作 | 现有 `computer_*` 工具，经自有 broker/helper 执行 |
| 文字操作 | 独立 `type`、`paste`、`selectText`、`setValue` | Unicode 键盘输入、按键组合和 AX 赋值；尚无专用粘贴和按文字选择接口 |
| 原生指针 | 客户端没有提供渲染、动画或生命周期实现 | ACECode 自行实现 AppKit 悬浮窗和截图合成，视觉反馈以现有 Windows 后端为参照 |

因此当前 macOS 后端属于自主实现，不能称为 Codex 原生代码的移植或完整行为复刻。后续对齐应逐项记录可观察的输入、输出和实际桌面效果；源码不能证明的细节需要对照运行结果验证。

## 功能

| 工具 | 行为 |
|---|---|
| `computer_list_apps` / `computer_launch_app` | 发现、启动应用 |
| `computer_list_windows` / `computer_activate_window` | 查找、激活窗口 |
| `computer_get_window` | 按已返回的窗口标识恢复窗口，可核对应用身份 |
| `computer_get_window_state` | 截图、可访问性控件树、文字与观察标识 |
| `computer_click` / `computer_drag` / `computer_scroll` | 鼠标点击、拖拽、滚动 |
| `computer_type_text` / `computer_press_key` | Unicode 输入与快捷键 |
| `computer_set_value` / `computer_perform_secondary_action` | 原生可访问性控件赋值与辅助动作 |
| `computer_release` | 释放当前会话的桌面控制 |

操作顺序是：列出窗口 → 读取窗口状态 → 根据截图或控件索引执行一个动作 → 再次读取状态。输入动作需要最新的 `observation_id`，旧观察、变化后的窗口几何与失效句柄不能继续使用。截图最长边为 2560 像素，坐标使用返回图片的像素；返回值同时提供原生尺寸、屏幕原点和缩放比例，后端负责换算。

一次观察最多返回主窗口及三个有明确归属的弹层，各图拥有独立的 `screenshot_id`、尺寸和坐标映射。多图或主图缺失时，坐标动作必须指定截图标识；动作的 `window` 仍使用这次观察的主窗口标识。菜单、下拉框关闭或移动后必须重新观察，不能继续沿用旧图。

截图作为工具附件保存，并进入支持视觉的模型请求。OpenAI 兼容接口先发送同批全部工具结果，再发送标记来源的图片消息；Anthropic 将图片放入对应的 `tool_result`。非视觉模型获得控件树和图片不可读提示，不应猜测坐标。

大型控件树进入工具结果文件时，预览开头仍保留窗口、观察标识、截图标识及坐标信息。控件索引点击会核对真实可点击点和命中元素；同一窗口内的浮层遮挡也会导致拒绝。控件树包含可用的焦点动作、选择项及文档内容。

Windows 组合键支持 Codex 文档中的 keysym 名称和常用别名，例如 `Control_L+Shift_L+period`、`Ctrl+/`、`Numpad_Add` 和 `KP_Enter`。标点快捷键使用 Windows OEM 键码，受当前键盘布局影响；需要 Shift 时应显式传入，直接输入文字应使用 `computer_type_text`。

## Windows 实现与边界

- UI Automation 用于控件信息与支持的控件动作；SendInput 用于鼠标和键盘。
- 操作时显示独立的桌面指针，包含移动和点击反馈；UIA 控件操作也显示目标位置。指针层不会抢焦点或挡住点击，回合结束、取消、关闭工具或释放执行器时清理。
- 窗口截图包含属于当前窗口的操控指针；没有操控指针时读取系统光标。各图的 `cursor` 提供 `visible`、`source`、热点坐标 `x/y`、`hotspot_x/hotspot_y` 与指针尺寸，均使用该图像素。原生合成后整体缩放，避免放大界面时光标错位；被其他窗口遮住的指针不会叠到目标截图。
- Windows.Graphics.Capture 用于窗口捕获，支持被其他窗口遮挡的目标。若当前系统或构建缺少该能力，仅在确认目标可见且无遮挡时回退到屏幕捕获，结果明确标记来源；不能可靠捕获时返回错误。
- 系统安全桌面、UAC 界面、受保护画面、应用完整性级别限制仍由 Windows 决定。某些自绘界面没有完整控件树，可以使用截图坐标。
- 每个交互桌面同一时间只允许一个 ACECode 会话控制。执行器在回合结束、取消或关闭开关时退出；主动启动的应用保留运行。
- 原生调用有 20 秒硬截止时间，输入序列包含按键或鼠标释放，超时后必须重新观察。
- 电脑操作使用现有工具审批模式，Plan 模式拒绝写操作。开关开启表示工具可用，具体动作仍遵守当前会话权限。
- Linux 暂不实现原生控制。

## macOS 实现与边界

- 原生 helper 使用 AppKit 主循环、Accessibility API、ScreenCaptureKit 和 CGEvent。Computer Use 最低要求 macOS 14；普通 ACECode Desktop 的最低系统版本保持不变，旧系统不会启动 helper。
- 设置中的权限区显示“辅助功能”和“屏幕录制”的独立状态。“去授权”会请求对应系统权限并打开系统设置；必须由用户在系统显示的应用条目下授权，再点击“刷新权限”。读取配置、刷新状态和打开工具开关都不会主动弹授权框。不同启动方式可能由系统归属到不同应用；以 macOS 显示的名称为准，权限检查始终在实际 helper 进程内进行。
- `enabled` 表示用户开启意图，`availability.ready` 表示两项权限均就绪。开关不会伪装成系统授权。没有屏幕录制权限时可返回明确警告及可用的 AX 控件信息，不能生成可用于坐标动作的截图标识；没有辅助功能权限时拒绝观察和输入。
- 截图只捕获指定窗口，关闭系统光标和窗口阴影；合成 ACE/plain 指针。图片坐标换算到屏幕点坐标，保留 Retina、非整数缩放和负屏幕原点。没有可见操控指针时 `cursor.visible` 为 `false`，不伪造系统光标。
- 鼠标和 AX 控件动作显示目标指针；按键和文字输入在可见、已验证的焦点控件处显示指针。点击反馈保留 250 ms，不延长真实鼠标按下时间。指针不抢焦点、不拦截输入，释放、取消或回合结束后清除。列举、启动和观察应用本身不会移动系统鼠标或生成点击反馈。
- 窗口绑定同时核对进程启动身份、CG 窗口几何与 AX 窗口；不能唯一匹配时拒绝输入。弹层必须从窗口的 AX 层级或已验证的前台窗口菜单层级发现，最多返回三个；不以“同一进程”推断无关浮动窗口归属。
- 输入前检查前台应用、焦点窗口、观察有效期、控件几何、实际窗口遮挡及 AX 命中。键盘还检查具体焦点控件，避免输入到同一应用的另一个窗口。观察标识有效期为 90 秒且只能用于一次输入；失败后也需重新观察。
- `computer_press_key` 支持 `Cmd+A`、`Option+Left`、`Ctrl+Shift+Tab`、F1–F20、方向键和 Numpad 键等。字符快捷键按当前键盘布局解析，修饰键需显式指定；文字使用 Unicode 输入。滚动量使用像素，`scrollX` 正值向右、`scrollY` 正值向下；Windows 的 120 单位滚轮语义保持不变。
- 受保护字段的 AX 值和选区不会返回，也不能通过 AX 赋值。自绘控件可能缺少 AX 信息，需使用截图。系统登录/锁屏、安全输入和应用自身权限仍受 macOS 约束。
- 同一用户桌面只有一个 helper 能获得控制租约。取消、超时、关闭开关或父进程退出会回收 helper、释放已按下的键和鼠标按钮，并清除指针。私有 socket 不对网络开放；单次调用仍有 20 秒硬截止时间。

## 安装与开发

Windows 构建生成 `acecode-computer-use.exe`，必须与 `acecode.exe` 放在同一目录。CMake `computer_use_runtime` 安装组件负责交付该文件；单独复制 daemon 时也需要复制 helper。执行器只使用父进程创建的私有管道通信，不开放额外网络端口。

当前 MSVC + Windows SDK 构建可使用 WGC；缺少 C++/WinRT 头的工具链使用明确标记的可见屏幕回退。构建与测试方式见根目录开发指南。

macOS 构建生成 `acecode-computer-use`，独立 CLI 包中与 `acecode` 相邻，桌面包中位于 `ACECode.app/Contents/MacOS/`、与 `acecode-daemon` 相邻。CMake 的 `computer_use_runtime` 安装组件、便携包、npm 包和发布签名流程均交付该文件。移动或重新签名应用后，应重新检查系统权限；开发用临时构建路径的授权不等于安装版已授权。

macOS 原生验证目标为 `computer_use_native_mac_smoke`，运行其 `.app/Contents/MacOS/computer_use_native_mac_smoke --run-owned-window`，经真实 broker/helper 操作专用测试窗口；`--observe-owned-window` 只验证观察。缺少权限返回退出码 77，表示未验证。几何和键名解析有独立单测；`tests/computer_use/macos_helper_test.py <helper>` 验证协议上限、互斥租约和父进程退出，不需要输入权限。

macOS 指针检查还独立枚举实际 helper 的 WindowServer 悬浮窗，用 ScreenCaptureKit 单独捕获该窗口，验证颜色、热点位置、焦点保持、点击穿透和释放后的消失；它不以工具返回截图中的合成指针作为桌面可见性的证明。锁屏时返回 77，真实可见性保持未验证。设置 `ACECODE_MAC_FIXTURE_ARTIFACTS` 可保存测试窗口图片和单独捕获的 `native-overlay.png`，不截取其他应用内容。

原生集成验证使用专门的可丢弃窗口：构建 `computer_use_native_smoke`，传入 `--observe-owned-window` 检查窗口发现、应用启动、遮挡截图、缩放、密码字段、弹层归属和控件命中；传入 `--run-owned-window` 继续检查鼠标键盘、控件操作和真实 OLE 拖放。后者需要测试窗口能获得前台焦点，系统搜索菜单等界面可能阻止激活，此时明确失败，不向错误窗口发送输入。可用 `--wait-for-foreground` 等待用户点击测试窗口标题栏后执行，超时自动结束。独立的 `computer_use_broker_smoke` 使用私有假执行器验证超时、取消、并发租约和大响应，不接触桌面。

`--run-owned-window-via-helper` 通过生产 broker、真实独立执行器和私有管道运行同一套完整检查，不回退到进程内调用。它验证中文与补充平面字符输入、辅助控件动作、菜单/下拉框选择、画布对象最终位置、OLE 目标 payload，以及可用显示器和缩放截图的实际坐标点击。测试窗口结束时销毁，不把事件已入队当作动作已完成。主题指针验证覆盖本次可用的一台显示器及 3000 像素窗口缩放；负原点等布局另有坐标单测覆盖。测试同时核对系统光标、UIA/点击后的可见桌面指针、PNG 指针热点及缩放一致性，并检查 ACE / plain 在默认蓝色和指定主题色下的实际 PNG 像素，截图保存到系统临时目录。

## 设置 API

`GET /api/config/computer-use` 返回：

```json
{"enabled":false,"supported":true,"platform":"windows","pointer_style":"ace","pointer_color":"#2563eb"}
```

返回值还包含 `availability`；macOS 的字段为 `supported`、`minimum_macos`、`helper_available`、`helper_path`、`accessibility`、`screen_recording` 和 `ready`，探测失败时包含 `error`。权限状态为 `granted`、`required` 或 `unknown`。非 macOS 权限状态为 `not_required`。

`PUT /api/config/computer-use` 接收 `enabled`、`pointer_style`、`pointer_color` 的任意子集，返回保存后的同结构对象；未传入的字段保持原值。例如 `{"pointer_style":"plain"}` 只修改样式。`enabled` 必须是布尔值，样式只能为 `ace` 或 `plain`，颜色必须为六位 `#RRGGBB` 并以小写保存。需要现有 daemon 身份认证；非法字段返回 `400 BAD_REQUEST`。不支持的平台开启返回 `400 COMPUTER_USE_PLATFORM_UNSUPPORTED`，但可保存外观；保存失败不改变已确认的运行时配置。UI 以服务端确认值显示状态，网络结果不确定时重新读取，避免把失败的关闭或样式保存显示为成功。

`POST /api/config/computer-use/permissions` 需要相同认证，接收且只接收 `{"permission":"accessibility"}` 或 `{"permission":"screen_recording"}`，返回包含最新 `availability` 的配置，不修改开关。只有用户点击授权按钮时调用；无效请求返回 `400 COMPUTER_USE_INVALID_PERMISSION`。HTTP 200 不代表权限已授予，必须检查返回的权限状态。
