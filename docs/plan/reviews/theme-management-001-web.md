---
task: theme-management-001
scope: 本轮自定义主题管理 Web 行为与 UI 源码独立复核
date: 2026-09-12
reviewer: impeccable_finish_reviewer
result: pass
validation: code-and-headless-behavior
visual: unverified
---

# 范围与结论

结论：**pass，限本轮代码、无浏览器行为和限定协议复核**。首次审查确认两项 P2 blocking，W1/W2 均已由作者修复并经独立定向回归关闭；原型中的状态行差异 N1 已修正。没有遗留的 confirmed blocking。完整 Web 测试和构建证据已核对，**visual unverified**，不据此宣称整项视觉验收完成。

比较基线为 `build/theme-management-web-baseline-20260912/manifest.json` 及对应 16 个文件，独立核验 SHA-256 全部一致。仅审查基线后的主题管理增量及新 `themeExports` 控制器，未把原有 AI 主题创建、专家预填和其它工作区脏改动纳入本轮。未修改实现、切换分支、提交或运行共享构建。

已阅读任务文件、OpenSpec design/spec、AGENTS.md、docs-sprint verify 流程、impeccable polish/craft floor，并查看用户批准的 `C:/Users/shao/Documents/Codex/2026-09-09/bang/outputs/acecode-theme-export-prototype-v2.png`。评审以既有 Settings、120px 卡片、现有字体及主题 tokens 为准，结合用户后续增加删除和移除压缩包图标的要求，未要求重画设置页。

# Confirmed findings

## W1 — P2 / blocking / 已复核关闭：迟到启动偏好能恢复已删除主题

- 契约：OpenSpec design 的 Ownership 中“旧请求完成也不能恢复已删资源”，以及 spec“删除当前主题”“删除其它主题或失败”的迟到响应边界。
- 首轮代码：`web/src/lib/appearancePreferences.js` 的 `restore()` 原本仅检查 `revision !== 0`；新增 `removeColorTheme()` 成功删除并未增加 revision，也未让 restore 检查已删集合。实际入口为 `web/src/App.jsx:787` 的 `getUiPreferences()` 启动请求。
- 独立复现：构造真实 appearance controller，初始为 `ai-current`；成功删除并返回持久化 `blue`；再交付删除前取得的 bootstrap `ai-current`。原结果为 `restore(...) === true`，可见状态从 `blue` 回到 `ai-current`。无需浏览器或模拟实现即可触发。
- 影响：删除已生效，但主题 ID 和本地首帧偏好缓存重新引用已删资源；后续刷新资源失败也不能正确保留删除后的蓝色状态。
- 修复复核：`web/src/lib/appearancePreferences.js:142` 增加 `deletedColorThemes.size > 0` 的 restore guard。作者在 `appearancePreferences.test.js:245` 添加真实 controller 的成功删除、cleanup_pending 和迟到 bootstrap 用例；独立运行该文件 13 项通过。该因果链已消除。

## W2 — P2 / blocking / 已复核关闭：旧取消快照覆盖更晚已保存终态

- 契约：OpenSpec design 的原生保存、可取消真实进度与 `native_saved` 语义；取消和成功反馈必须与实际写入一致。
- 首轮代码：`web/src/lib/themeExports.js:118` 缓存第一次 `cancelThemeExport()` Promise；`finishCancelled()` 在约 122–128 行只按该结果决定 completed/cancelled，没有尊重随后轮询取得的已保存终态。
- 独立复现：真实 export controller 以原生模式启动并进入 compressing；让 GET job 暂停；取消接口返回 `saving, native_saved:false`；随后 GET 返回 `completed, native_saved:true`。原结果为 `start()` 返回 false、最终 `status === 'cancelled'`，但 `state.job.native_saved === true`，且没有成功通知。
- 后端接口依据：`ThemeStore::cancel_export()` 可返回仍在工作的快照；原生路径在发布目标文件时先设置 `published`，之后才经临时文件清理和事务锁发布 info 终态。该窗口内取消可以取得 saving 快照，因此上述接口顺序符合真实实现，并非只由不可能的 mock 状态造成。
- 影响：实际已保存甚至已覆盖目标文件，界面仍按取消结束。修复应确认可靠终态，保留后来确认的 `completed/native_saved:true`，不得让缓存的中间态覆盖它。相同边界还须检查 Web `writable.close()` 提交期间取消被拒绝后仍报告成功。
- 修复复核：`themeExports.js:122` 的 `finishNativeSave()` 统一报告成功并清除旧取消错误；`finishCancelled()` 在等待取消之前和取消异常之后都优先接受当前已确认的 native_saved，非终态取消快照则继续轮询。`cancel()` 的错误反馈也不能覆盖已经确认保存的 job。该修复覆盖首轮 cancelled 误报，以及同一边界中“取消请求 reject、GET 随后证实已保存”的 failed 误报。
- 定向回归：`themeExports.test.js:181` 起包含中间取消快照被较晚已保存结果覆盖、取消后继续轮询实际终态、Web close 已进入提交时 cancel 返回 false 并最终 saved、旧取消网络错误被已保存终态清除。独立运行最终 18 项全部通过。已关闭，不要求新的存储修改。

# Non-blocking

## N1 — P3 / non-blocking / 已静态复核关闭：操作出现时隐藏状态行

首轮 CSS 在 hover、focus-within 和无 hover 设备上把 `.ace-theme-card-state` 设为透明，与 V2 和 design 图中的“使用中”及操作并存有差异。作者已移除隐藏规则，仅在 `.ace-local-theme` 内收紧色块、标题与状态间距；120px 卡片仍保留。状态和按钮现在在源码布局中同时存在，实际窄屏、缩放和字体渲染仍属于未验证的视觉范围。

# 已检查的行为边界

- 自定义管理入口限定于合法 `ai-*`、`source: local`、`installed: true`。blue/orange/EVA 没有管理入口。选择按钮和操作按钮是兄弟节点，操作显式停止传播；hover/focus-within 和 `(hover: none)` 提供入口。
- 浏览器支持的 ZIP Save As 在点击调用链的第一个 await 之前调用；取消选择不创建导出作业。桌面请求设置 `native_save:true` 并使用无超时 API，能力缺失时仅按明确错误回退到鉴权 Blob 下载。
- 没有格式选项；下载来自正常鉴权 API Blob，无带 token 的直接资源链接。复用已有 ZIP 不制造压缩状态；确定百分比只来自后端数值，null 对应不确定 progress；打包 UI 没有压缩包图标。
- 导出错误保留可读原因和路径；打包 Modal 关闭走取消；删除、导出状态和错误 Modal 使用 `z-[400]`，高于 Settings 的现有层级并复用共享 Modal。
- appearance 队列在旧完整快照后执行删除，对已删除 ID 的后续快照回退为 blue；保留后来选择的其它主题；删除失败不毒化队列；cleanup_pending 显示成功及待清理提示。
- 删除先从本地卡片列表剔除并释放资源，再异步刷新目录；移除集合过滤迟到目录响应，selection/created 完成检查阻止已删主题重新应用。ThemeProvider 使用 cache Promise identity 防止迟到加载重新入状态；forget 释放 pending Blob；刷新失败且删除发生时也释放旧 Blob。

# 验证证据与限制

本次独立执行最终 58 项聚焦检查并通过：

- `node src/lib/themeExports.test.js`：18 项，包含 W2 最终修复回归。
- `node src/lib/appearancePreferences.test.js`：13 项，包含 W1 修复回归。
- `node src/lib/themePackages.test.js`：22 项，包含本地删除、离线、迟到选择/创建和资源释放。
- `node src/lib/themeManagementUi.test.js`：5 项，执行从生产源转换的 React 静态渲染、实际 onClick callback 和 ThemeProvider cache callbacks，覆盖操作按钮语义/传播、真假进度、无图片图标、删除确认层级、迟到加载与刷新失败时资源释放。它没有运行真实浏览器或完整 DOM 事件系统。
- 两个独立真实 controller 故障复现，分别得到 W1 的旧 ID 恢复和 W2 的 cancelled/native_saved 冲突。
- 本轮 baseline 16 个 SHA-256 校验，0 不一致。
- 正常仓库配置下 `git diff --check -- web` 退出 0。

作者执行证据已独立核对，未重复共享构建：

- `build/theme-management-web-tests-20260912.log`：2233 个 pass、0 fail。
- `build/theme-management-web-build-20260912.log`：Vite 完成 3033 模块构建，15.47 秒；后置检查扫描 4427 个正则且兼容检查通过。
- `build/theme-management-api-web-test.log`：65 个 pass、0 fail；新增两项 API 测试调用真实 createApi/request，验证五个管理方法的鉴权头、编码 URL、ZIP Blob、原生无超时和下载 AbortSignal。测试的 fetch 为模拟网络边界，真实服务由下段 HTTP 测试覆盖。
- i18n 新文案经 overrides 与生成 catalog 接入；完整 Web 测试/build 包含该产物。
- `build/theme-management-impeccable-detect-20260912.json`：作者按要求仅执行一次 detector，共 12 条，全部在 globals.css。独立核对本轮 CSS diff 仅新增 31 行，并将 12 条报告行映射回 baseline，12 条原规则均完全一致；ThemeCards 无报告项。未以机械检测结果替代视觉验证。

C++ 存储已有独立审查，本报告只读取必要的接口状态依据并执行下段限定协议对照。

真实浏览器验证受环境阻塞：已安装的 26.825.41651 browser-client 初始化时，其受信 worker 引用缺失的 `openai-bundled/browser/26.903.71938/scripts/browser-service.mjs`，报 `Cannot find module`。主代理已记录并停止此路径。本审查没有重试被策略拒绝的 shell 浏览器启动，没有绕过工具环境，也没有把原型、静态标记或无效截图当作当前产品视觉证据。桌面/窄屏触达、真实焦点圈、重叠、对比度、长名称、120px 内实际排版及原生系统选择器操作均不能据此宣称视觉通过。

# 限定 HTTP / 偏好 / 原生类型协议对照

主代理追加授权的本段仅检查 `routes_themes.cpp`、`routes_misc.cpp` 本轮偏好验证增量和原生保存类型接入，对比 `build/theme-export-baseline-20260912/`，未重审已有存储报告关闭的四项。结论：**pass，限源码及既有执行证据，未新增 blocking**。

- `src/web/routes/routes_themes.cpp:91` 起五条新增管理路由均调用正常鉴权与迁移保护。export body 只接受可选 boolean `native_save`；路径等额外字段、非对象和错误类型会在开启保存前拒绝。缺少原生能力返回 501/THEME_NATIVE_SAVE_UNAVAILABLE；选择器取消返回无 job ID 的 cancelled，主题验证及内置保护由同一 store 入口执行。
- ZIP 下载在 `routes_themes.cpp:126` 起复用经过校验的作业资源，添加 `application/zip`、UTF-8 百分号编码 attachment 文件名、`private, no-store` 和 `nosniff`，成功后才响应文件。无任意目标路径参数，也不把鉴权凭据放入下载 URL。
- 删除在 `routes_themes.cpp:146` 起先持有 `app_config_mu`，确认该时刻当前主题，仅在需要回退时持久化 blue。save_config 抛出时恢复 `before.web_ui`，把失败交回 store 的隔离回滚；返回的 ui_preferences 取自同一锁下的实际配置。删除非当前主题不替换选择。
- `src/web/routes/routes_misc.cpp:2291` 把 ai/EVA 安装检查放入 config 锁内，再检查 store，和删除采用同样的锁顺序。因此删除后排队到达的旧 ID 写入不能重新持久化已删主题；与 Web 队列的旧快照过滤相互衔接。
- `src/platform/native_ui/folder_picker_win.cpp:247` 起按建议文件名 `.zip` 切换单一 ZIP filter/default extension；mac `folder_picker_mac.mm:89` 起切换到 zip，且保留 `allowsOtherFileTypes = NO`。两者其它导出保留原有 Markdown 分支。新增中英文类型/标题/错误枚举与 strings catalog 顺序对应，未改变共享保存接口的路径来源。
- `cleanup_pending:true` 和新的 `cleanup_message` 描述提交后的隔离缓存清理延迟。Web 以 deleted 为成功依据并显示本地化待清理提示，不会因该附加字段恢复卡片。

独立读取 `build/theme-export-tests-20260912.xml` 和 `.log`：60 项全部通过，0 failure/error/skipped/disabled；其中 4 个新增真实 HTTP 用例覆盖内置保护、注入 destination 拒绝、原生能力缺失、鉴权 ZIP 头、取消选择不打包、原生保存、删除当前主题持久化 blue、旧偏好写入拒绝、配置写入失败后的文件/偏好回滚。另有 DesktopStrings 的完整 catalog 和 ZIP 文案断言通过。本复核未重复运行这 60 项。

这些 HTTP 用例使用真实服务和存储，但原生选择器回调由 fixture 提供；不能当作 Windows/macOS 系统对话框的实机验证。Windows/macOS 原生保存类型及用户交互仅静态核对。

# 最终复核快照

| 文件 | SHA-256 |
| --- | --- |
| `web/src/lib/themeExports.js` | `983C44C3915565FAAE42CEAD2CF3872846F136D47F3F592D5B9645230B6A8B3A` |
| `web/src/lib/themeExports.test.js` | `37DDF15875B88B4EDFC9F09E0F696814F5251ACA6A355E490A7E913724848542` |
| `web/src/lib/appearancePreferences.js` | `C4D642F4ADAEBCB59A2E266EC8985E51DB2C990EF0D97C5FD32B882131BD17EB` |
| `web/src/lib/themePackages.js` | `E894B5A447524339010CA245E803597F7CAC607854A064CDF95F358FD64A01BE` |
| `web/src/theme.jsx` | `BF2A3569F3579E65817C678D1A85447137803D103DABFF33BD2EA68007256DE5` |
| `web/src/components/ThemeCards.jsx` | `0EB256C5DA0FCF08F93775D0F3E75B03408499CF686FD17E6108A815283AB76B` |
| `web/src/lib/themeManagementUi.test.js` | `906392D2A190A32175D30B5B6DE9916B000815276FA139351FD08D7AF023E7D2` |
