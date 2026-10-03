## 1. 后台同步核心

- [x] 1.1 实现连接分组、有效声明合并、用户覆盖与过期快照保护，并通过定向 C++ 单测。
- [x] 1.2 实现可取消的后台队列及网络边界，以注入探测器验证非阻塞、合并请求、失败保留和退出。

## 2. 触发及界面联动

- [x] 2.1 接入服务启动、模型保存和认证刷新端点，持久化后发布版本与通知；通过路由/集成验证。
- [x] 2.2 刷新按钮接入远端同步，设置及聊天消费通知且静默处理失败；通过定向前端检查。
- [x] 2.3 更新 daemon API 文档，核对事件和接口与实现一致。

## 3. 完整验证

- [x] 3.1 完成相关 C++ 测试、pnpm test、pnpm build、OpenSpec 严格验证及 git diff --check，复核无关改动保留。

## 4. 新对话思考深度修复（2026-10-04）

- [x] 4.1 进入新对话和输入框刷新时非阻塞调度声明同步，通知不形成重复请求，保留现有选择及创建参数链路。
- [x] 4.2 补充缺失声明恢复、静默失败及新会话选择传递回归验证，完成浏览器交互检查。
- [x] 4.3 完成 pnpm test、pnpm build、严格 OpenSpec 验证及差异检查，记录运行中桌面与源代码验证范围。

## 2026-10-04 修复验证

- 当前桌面 daemon 的已保存 Starrylight / Moonlight 列表没有 reasoning，部分已有会话仍保留有效声明。通过既有后台刷新端点后，列表及配置恢复 low / medium / high / xhigh / max，默认 medium；未手工推断或编写档位。
- modelReasoningSync、modelReasoning、sessionModel 定向测试通过，覆盖缺失声明恢复、同步失败、通知不重复发起同步、支持范围及创建参数传递。
- Headless Chromium 加载真实 ChatView，使用隔离 API fixture 验证新对话自动同步、通知恢复控件且不循环请求、宽屏 1331px / 窄屏 390px 档位选择、输入保留、同步失败静默、输入框刷新及新会话请求 reasoning_effort。浏览器无 pageerror；截图在本机临时目录 ace-home-reasoning-desktop.png / ace-home-reasoning-narrow.png。
- pnpm test、pnpm build、openspec validate sync-saved-model-reasoning --strict 和 git diff --check 全部通过；Impeccable 检测无新增问题。
- 本次构建更新 web/dist；当前桌面通过 static-dir 使用该目录，下次页面加载读取新前端。未重启桌面、未替换原生可执行文件；自动补齐链路的交互验收来自当前源码浏览器 fixture，不宣称已在正在显示的旧桌面页面实测。

## 原验证记录

- C++ Release 单测目标构建成功；SavedModelReasoningSync、ModelsHandler、SavedModels、会话模型绑定及相关 HTTP 探测测试共 157 项通过。
- 真实本地 HTTP 集成测试验证启动与保存不等待远端、同连接合并请求、刷新全部模型、档位落盘及失败保留。
- modelReasoningSync、modelReasoning、modelSettings、sessionModel、customCompatibilityModelFormArchitecture 定向前端测试通过。
- pnpm build 通过；OpenSpec 严格验证与 git diff --check 通过。
- pnpm test 已执行，受现有 sidebarAlignmentArchitecture.test.js:22 断言失败阻塞。该断言在本次未修改的已暂存 Sidebar.jsx 上同样不匹配；保留原有侧栏工作。
- 本次验证使用新构建单测中的 WebServer 与临时配置；未替换或重启用户正在运行的桌面实例。
