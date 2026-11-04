# Tasks: add-inline-visuals

## 1. 配置与开关接口

- [x] 1.1 `AppConfig::inline_visuals { enabled = true }` 的加载与稀疏保存(默认值不写出,非布尔忽略);验证:新增 `tests/config/config_inline_visuals_test.cpp` 覆盖缺省 / 显式 false / 保存省略默认值并通过
- [x] 1.2 `apply_default_session_preferences_bytes` 从磁盘读回 `inline_visuals.enabled`,让同机其它 daemon 跟上;验证:`tests/config/default_session_preferences_test.cpp` 新增用例,改 config.json 后刷新得到新值
- [x] 1.3 `handlers/inline_visuals_handler.{hpp,cpp}`(快照 + patch 解析,非布尔报错)与 `routes_inline_visuals.cpp`(写锁内 `mutate_config`),在 server.cpp 注册;`docs/daemon-api.md` 增补该路由;验证:`tests/web/inline_visuals_handler_test.cpp` 通过,`check_doc_paths` 通过

## 2. 画图说明与规范技能

- [x] 2.1 `src/engine/prompt/diagram_guidance.{hpp,cpp}`:按运行面(none / terminal / web)、visual 开关、技能可用性、skill_view 模型侧名字生成固定文本,mermaid 部分与前端安全检查的拒绝规则对齐;验证:`tests/prompt/diagram_guidance_test.cpp` 覆盖三种运行面、开关关闭、技能不可用、工具重写映射、同输入逐字节相同
- [x] 2.2 `SessionPromptConfig` 增加运行面与开关;worker 设为 web、TUI 设为 terminal、headless 不设;`RequestContextFactory` 对子会话与群聊代答降为 none;`static_system_prompt` 末尾追加说明;visual 部分不生效时把 `inline-visuals` 并进技能索引的隐藏集合;验证:`tests/agent/` 下新增请求组装用例:web 开启含两部分、关闭只含 mermaid、子会话不含、技能索引按规则隐藏,且 `RequestPrefixIsByteStableAcrossIterationsInATurn` 仍通过
- [x] 2.3 内置技能 `assets/seed/skills/acecode/inline-visuals/SKILL.md`(隔离框变量与类名、SVG 布局与配色、交互与脚本、图表、示例),更新 `MANIFEST.json`、`seed.version` 与 `default_skill_seeds()`;验证:`DefaultSkillSeedRegistryTest.PackagedManifestVersionAndHashesAgree` 通过
- [x] 2.4 新建主题文档 `docs/inline-visuals.md`,写明画图说明的取舍规则与技能;验证:文档里的源码路径通过 `check_doc_paths`

## 3. 看不到图的地方

- [x] 3.1 `src/domain/llm/inline_visual_blocks.{hpp,cpp}`:识别顶层 visual 围栏并替换为占位文字;验证:`tests/llm/inline_visual_blocks_test.cpp` 覆盖多块、未闭合、嵌套、mermaid 不受影响
- [x] 3.2 IM:`classify_session_event` 的助手文本分支替换 visual 块;验证:`tests/remote_control/session_channel_binder_test.cpp` 新增用例
- [x] 3.3 TUI:Markdown 代码块分支遇到 visual 输出一行占位提示;验证:TUI markdown 测试新增用例

## 4. 前端隔离框

- [x] 4.1 `web/src/lib/inlineVisualFrame.js`:srcdoc 生成(CSP、基础样式、主题变量、色调类、预置箭头、运行时)、主题变量映射、消息校验、追问限频与长度判断;验证:`inlineVisualFrame.test.js` 通过
- [x] 4.2 `web/src/lib/inlineVisualRuntime.js`:框内运行时(脚本剥离、差量对齐、定稿执行脚本并补发事件、尺寸上报、追问与链接拦截、存储顶替);验证:`inlineVisualRuntime.test.js` 在假 DOM 上覆盖对齐与脚本剥离
- [x] 4.3 `components/InlineVisualBlock.jsx` 与发送桥上下文:消息排队到 iframe 加载完成、来源校验、高度记忆与夹取、复制按钮、主题变化推送;验证:`inlineVisualArchitecture.test.js` 守住 sandbox 恰为 allow-scripts、srcdoc 以 CSP 开头、只认自己 iframe 的消息
- [x] 4.4 `docs/inline-visuals.md` 补充隔离框的安全边界与消息协议;验证:文档与实现中的变量名、消息名一致

## 5. 前端接入与设置

- [x] 5.1 `markdown.js`:`renderMarkdownBlocks` 对顶层 visual 围栏返回 visual 块(流式与定稿同一 key),嵌套与 `renderMarkdown` 走普通代码块;验证:`markdownBlocks.test.js` 新增用例
- [x] 5.2 `Message.jsx` 渲染 visual 块;`ChatView.jsx` 提供发送桥(空闲发送、忙时填进输入框)与外链打开;`globals.css` 隔离框样式;验证:`pnpm test`、`pnpm build` 通过
- [x] 5.3 设置 > 常规「可视化图表」开关(`lib/inlineVisuals.js`、`api.js`、`settingsSearch.js`、i18n 覆盖与 `pnpm i18n:catalog`);验证:`inlineVisuals.test.js` 与 `sourceCatalog` 测试通过
- [x] 5.4 `CLAUDE.md` 增加一行索引指向 `docs/inline-visuals.md`;验证:`check_doc_paths` 通过
- [x] 5.5 (实现中发现)回合结束后的对话记录自愈替换最新一轮时整轮换新 id,对话行卸载重建,隔离框重新加载、脚本重跑;`transcriptSelfHeal.js` 改为按位置对得上的行沿用原 id;验证:`transcriptSelfHeal.test.js` 新增用例,端到端核对回合结束后仍是同一个隔离框

## 6. 集成验证

- [x] 6.1 构建 `acecode` 与 `acecode_unit_tests`,定向跑本变更涉及的测试套件以及系统提示、技能播种相关用例,全部通过
- [x] 6.2 用 OpenAI 兼容的假模型服务流式回放一段含 visual 与 mermaid 的回复,在真实 daemon 页面里核对:同一个框逐步长出图形、闭合后脚本执行、框内请求本机 daemon 被拦、点击追问发出消息、深浅色切换原地更新、mermaid 正常渲染

## 7. 内置图表库

- [x] 7.1 前端依赖 `chart.js` 4.5.1 与 `d3` 7.9.0(精确版本),`web/src/lib/inlineVisualLibraries.js` 用 `?raw` 动态导入、按名字缓存;验证:`pnpm build` 产物里两个库各在独立 chunk,首屏入口 chunk 不变大
- [x] 7.2 框内运行时按地址认库、向外层要代码(`library` 消息),主版本不同 / 模块脚本 / 取不到时走原地址;`InlineVisualBlock` 只给已知库;验证:`inlineVisualRuntime.test.js`、`inlineVisualFrame.test.js`、`inlineVisualArchitecture.test.js` 新增用例
- [x] 7.3 画图规范技能与系统提示的 visual 部分写明内置库;`docs/inline-visuals.md` 补充;验证:`diagram_guidance_test.cpp`、`DefaultSkillSeedRegistryTest.PackagedManifestVersionAndHashesAgree` 通过
- [x] 7.4 端到端:浏览器拦截所有 CDN 请求后,引用 Chart.js 与 d3(含 d3js.org 地址)的 visual 照常画出,框内请求本机仍被拦;验证:端到端脚本全部通过

## Workflow follow-up

- 用户验收后归档本变更(`openspec archive add-inline-visuals`),并核对归档后的主规格。
