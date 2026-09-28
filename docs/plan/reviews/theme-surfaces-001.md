---
task: theme-surfaces-001
scope: 主题外观参数、本轮后台与 Web 新增量独立复核
date: 2026-09-12
reviewer: theme_surface_review
result: pass
validation: code-and-headless-behavior
visual: unverified
---

# 范围与结论

结论：**pass，限本轮源码、无浏览器行为和已有构建证据复核**。未发现 confirmed blocking，也没有需要另行归档的 non-blocking 代码问题。**visual unverified**；本报告不把源码静态渲染、fake GL 或构建成功当作当前产品的视觉验收。

仅审查 `docs/plan/tasks/theme-surfaces-001.md` 和 `openspec/changes/configure-theme-surfaces/` 的三个外观参数及其传递、渲染、兼容边界。比较基线为 `build/theme-surface-backend-baseline-20260912/` 与 `build/theme-surface-web-baseline-20260912/`；后台基线 22 项、Web 基线 16 项 SHA-256 均独立核验一致。后台 15 个和 Web 12 个最终新增量文件分别与作者最终哈希清单一致。没有把当前 master 的其它脏改动纳入本轮，没有重审前轮已关闭的导出、删除存储事务。

已阅读 AGENTS.md、文档索引、任务及 OpenSpec design/spec、docs-sprint verify/contract 流程、impeccable craft floor 和代码审查指导。沿用既有 ACECode 布局、品牌 SVG、动态 shader、标题栏与侧栏结构；未修改实现、分支、运行中的 daemon，也未重复共享原生或 Web 构建。

# Findings

- **Blocking：无。** 本轮没有可复现的设计契约偏离或残留 stub/mock 实现。
- **Non-blocking：无新增代码事项。** 下面的视觉与执行边界属于证据限制，不作为虚构的代码缺陷。

# 后台契约与集成

| 契约 | 独立检查依据与结论 |
| --- | --- |
| schema 1 与恰好 28 色保持 | `src/adapters/themes/theme_store.cpp:271` 增加独立 `valid_theme_appearance`，原颜色集合不变。两个颜色只接受 7 字符的 `#RRGGBB`，通顶只接受 JSON boolean；对象为 null、错误类型、未知键及非法字段值均拒绝；空对象允许。 |
| 旧草稿确认兼容 | `src/adapters/themes/theme_drafts.cpp:48` 仅在原草稿显式包含 appearance 时把它加入 palette digest；未添加默认字段，因此缺省旧草稿的 palette/prototype 摘要算法不变。 |
| 完整方案替换与确认失效 | `theme_drafts.cpp:178` 的 palette 先校验再改草稿；省略 appearance 会删除旧覆盖。修改方案清除两阶段确认及图片摘要。读取损坏的持久化 appearance 返回 `THEME_INVALID_APPEARANCE`。 |
| status、安装及导出传递 | `theme_drafts.cpp:65` 保留 appearance；`:263` 的安装重试比较同一对象，`:273` 写入完整定义。原有 ThemeStore 继续保存和导出完整 theme.json，ZIP 仍只有三个根文件；新增 round-trip 回归读取实际 ZIP 并重新安装。 |
| 安装不能追加参数 | `theme_drafts.cpp:118` 只有 palette 允许 appearance；prototype/install/status 不接受追加外观配置。工具 schema、工具描述与这些限制一致。 |
| AI 技能与种子升级 | 色系步骤同时展示三个参数及缺省值；原型提示写明同一参数、连续壁纸、深色白色按钮、浅色主题前景和关闭红色反馈。修改外观返回两阶段确认。示例仍有 28 色，另有三个 appearance 键。seed.version 与 MANIFEST 为 `2026-09-12.2`，SKILL.md 的 canonical-LF SHA 与 manifest 一致；新增回归覆盖已管理旧版本升级及用户自定义技能保留。 |

# Web 消费与界面边界

- `web/src/lib/themePackages.js:15` 独立严格校验 appearance；`validThemeHexColor` 同时阻止字符串尾换行等正则边缘情况。`:34` 统一解释缺省与显式值：旧自定义主题不通顶、标题使用 colors.fg、原 logo；旧 EVA 缺省继续通顶并用白色右侧按钮，显式通顶选项优先。
- `web/src/theme.jsx:134` 从当前已安装主题提供统一 appearance。`:141` 在 layout effect 内应用 CSS 变量与 DOM 属性，并由同一 effect 的 cleanup 清除。`themePackages.js:68` 仅在拥有合法应用 Blob URL 时启用壁纸、通顶与白色按钮属性；切换到无资源主题、删除回退或失去已加载资源引用时不会沿用前一主题的派生属性。加载/刷新失败继续遵循已有主题控制器的保留或切换行为。
- `web/src/components/BrandLogo.jsx:6` 为首页静态/降级图标与侧栏提供同一来源。`brandLogoColors.js:23` 只替换内置 SVG 固定颜色，不接受主题自带 SVG/CSS；路径、圆角、裁剪、字形几何保持。缺省继续使用原 PNG；显式颜色使 EVA 的旧 hue-rotate 不再覆盖它。
- `web/src/components/InteractiveHomeLogo.jsx:354` 将相同 RGB 混色规则接入既有 shader，白色字形、高光、阴影、鼠标响应和 idle 动画的结构保留。logoColor 变化使旧 effect 清理并重新绑定；无 GL、失去 context、减少动态效果、关闭动画和 EVA 静态分支仍有落点。黑、白、灰和饱和色的 CPU 颜色数据与 uniform 均为有限数值。
- `web/src/styles/globals.css:2881` 只为 `.ace-home-title` 引入独立变量；实际 ChatView 的普通问候和项目名变体共用同一 h1。没有修改任务正文或其它标题的前景色。
- `globals.css:2727` 的同一背景伪元素仍从侧栏边界开始；关闭通顶时背景顶部从共享 30px 标题栏高度以下开始。原 `.ace-topbar::before` 与侧栏继续使用 shell 背景。App 原 `data-home-wallpaper` 开关排除已有任务、其它视图、设置和反馈。
- `globals.css:2742` 的白色前景规则覆盖右侧控制台、面板及窗口按钮，并以高于原交互规则的选择器优先级保持白色；白色悬停背景明确排除 close，原 close 红色 hover/focus 仍有效。浅色通顶沿用主题前景 tokens。TopBar.jsx、WindowControls.jsx 与基线一致，因此按钮 callback、拖拽排除、双击最大化、面板状态和 macOS 原生交通灯分支未改。

# 验证证据

独立执行的 31 项轻量回归全部通过：

- `node web/src/lib/themeSurfaces.test.js`：7 项。覆盖严格校验、旧/显式缺省规则、属性清理、SVG 几何、实际组件 SSR、实际 renderer 控制流及 CSS/入口关联。
- `node web/src/lib/themePackages.test.js`：22 项。保留主题切换、加载失败、迟到资源、删除回退、离线和资源释放的既有行为。
- `node web/src/lib/topBarWindowDrag.test.js`：2 项。保留标题栏拖拽范围、消费事件排除及面板按钮语义。

已独立读取并核验作者完成的证据，未重跑共享构建：

- `build/theme-surfaces-native-build-20260912.log`：MinSizeRel 测试目标成功产出，编译了本轮五个测试文件。
- `build/theme-surfaces-native-tests-20260912.log` 与 `.xml`：71 项、14 suites，失败/错误/禁用均为 0，10.738 秒；精确过滤器保存在日志第 2 行。包含严格校验、旧摘要、三字段增删改确认失效、安装幂等、实际 ZIP round-trip、seed 升级和相关 HTTP/偏好回归。
- `build/theme-surface-web-baseline-20260912/web-test.log`：2249 个 pass，0 fail。
- 同目录 `web-build.log`：Vite 3037 模块，22.95 秒完成；后置兼容检查扫描 4435 个正则，无 lookbehind。
- 同目录 `i18n-audit.log`：审计完成；本轮没有新增静态中文文案角色，新增验证入口复用已有“主题配色数据无效”及对应英文 override。
- 后台、Web 已修改文件的正常仓库 `git diff --check` 均退出 0；三个新增 Web 文件没有行尾空白。
- 最终清单 `build/theme-surfaces-backend-files-20260912.json` 的 15 项和 Web `final-web-files.json` 的 12 项与当前文件 SHA-256 相符。

impeccable detector 的一次输出位于 Web 基线目录 `impeccable-detect.json`，共 12 条。11 条落在本轮未触及的旧 CSS 规则；第 12 条是已有首页 logo 的双轴网格，本轮保留结构并变量化主色。它们不能证明新增布局问题，也没有据此扩大改造既有界面。没有把该检测替代视觉验收。

# 验证边界

真实浏览器受信 worker 模块在本会话环境缺失，按任务边界未重试或用 shell 绕行。SSR 验证标记与组件分支，fake GL 验证生产 renderer 的调用控制流和传入 uniform；它们不执行 GPU shader 编译/像素输出，也不证明浏览器图片解码、实际 CSS 布局、焦点视觉、对比度、窗口拖拽或原生系统按钮的现场操作。桌面/窄屏外观、实际壁纸连续性和静态/动态最终观感仍为 **visual unverified**。

最终应用与 Desktop 构建由主代理协调的作者在 Web 构建完成后继续执行，属于本次交付的后续总门槛；本报告没有把尚未完成的最终应用构建记为已通过。
