# 主题与 AI 主题创建

## 创建 AI 主题

在「设置 → 外观」点击「AI主题」，ACECode 会打开新任务，并带入 `/ai-theme` 技能和可编辑的开场白：

> 我想生成关于 XXX 的主题，X 色是它的主色调。

替换主题对象和主色后再发送。点击卡片只准备草稿，不会发送消息或开始生成图片。

创建分为以下步骤：

1. **选择方案**：回答定制程度（快速：主页和样式；高级：主页、会话页和样式；深度：主页、会话页、聊天卡片定制）、主题模式（位图、矢量图、使用自带贴图）、风格基座（白天、黑夜、不拘泥于白天还是黑夜）。自带贴图指用户提供的本地图片，矢量图和自带贴图不调用生图工具。
2. **确认细节和色系**：设置首页文本框透明度、首页图片主色调和透明度；高级、深度定制还设置会话图片主色调和透明度。每项提供三档建议及其他输入。透明度 0% 表示完全不透明，100% 表示完全透明。图标色、首页标题色和背景通顶设置一并保留并确认。
3. **确认原型**：查看独立背景和套入 ACECode 布局的预览；无生图模型时，用深浅 HTML 模板在 ACECode Browser 展示主页在上、会话页在下的预览。选择「确认原型并生成」，或者继续修改。
4. **生成主题**：把已经确认的素材制作成本地主题。当前 Web/Desktop 任务收到生成完成事件后应用主题，并在外观页保留新卡片；原有主题仍可切换。

修改色系会使后续原型确认失效；取消、超时或无人值守运行不会代替用户确认。中断后可在同一任务通过技能查看草稿状态并继续。缺少图片生成工具时仍可用矢量图或本地图片制作主题；位图方案尚无素材时先保留 HTML 原型草稿，不能将占位框图当作最终壁纸安装。

主页、会话页、用户消息卡片的背景分别配置。聊天卡片图片只影响用户发送的消息正文气泡，不影响 AI 回复、附件或输入框。图片透明度只改变背景，文字和控件保持清晰。

每个主题交付独立背景、缩略图、28 项 UI 配色、可选外观参数和可分享的 ZIP 包。工具返回实际文件位置。主题保存在当前配置目录旁的 `themes/` 中；已安装的本地主题可离线使用。读取历史聊天记录不会重新应用过去生成的主题。

## 图标、首页标题与背景通顶

主题可以分别指定首页和侧栏的 ACECode 图标主色，以及“我们该做什么？”和带项目名的首页标题文字颜色。图标保持原有形状、字形和动态效果。

开启背景通顶后，首页主内容列的壁纸连续延伸到标题栏；深色主题的右侧操作按钮变为白色，关闭按钮仍保留红色悬停反馈。关闭通顶时标题栏保留独立底色。侧栏使用自己的底色，窗口拖拽和各按钮操作继续有效。

这些设置在 AI 主题制作时一并确认，随主题包保存和导出。旧主题包无需修改，继续使用原来的图标、标题颜色和通顶行为。

## 导出与删除自定义主题

在外观页悬停或键盘聚焦自定义主题，卡片左下角显示“导出主题”和“删除主题”；触屏直接显示。内置蓝色、橙色和 EVA 不提供这两个操作。点击操作不切换主题。

导出固定为完整 ZIP 主题包。桌面端点击后打开系统另存为；Web 优先使用可用的保存窗口，否则通过普通浏览器下载，是否询问保存位置遵循浏览器设置。已有与当前主题一致的 ZIP 直接保存，否则显示文件名、真实压缩进度和取消入口。进度面板不显示压缩包图标，压缩和浏览器下载是两个不同阶段。

删除需确认。删除正在使用的主题后，当前外观与已保存设置一同回到蓝色；删除其它主题不影响当前选择。删除失败保留原主题，只清理 ACECode 保存的该主题及派生包，保留任务草稿、原始素材和用户自己导出的文件。

交互布局、并发与接口边界见 [主题管理设计](../openspec/changes/manage-custom-theme-packages/design.md)。

## 本地导入与主题工坊

在「设置 → 外观」的主题标题旁，使用「本地导入」和「主题工坊」。本地导入接受最大 16 MiB 的完整自定义主题 ZIP，先校验并展示名称、色系、缩略图、图标色、首页标题色和通顶参数；确认后才安装，是否立即应用由复选框决定。同一主题 ID/版本的不同内容不会覆盖已安装主题。

「主题工坊」使用独立服务：保留当前更新源的协议和主机，将端口加 10000，并打开根路径。例如 `http://2017studio.imwork.net:82/aupdate/` 对应 `http://2017studio.imwork.net:10082/`，端口 1234 对应 11234。省略端口时 HTTP 使用 10080、HTTPS 使用 10443；原端口必须介于 1 和 55535。桌面使用系统浏览器，Web 打开新标签页；更新源保存后或窗口重新获得焦点时刷新入口。工坊可搜索、预览和下载主题。上传先预览，再提交审核。待审核与驳回主题及其图片、ZIP 不公开；管理员在独立站点的 `/admin` 通过后才进入公开列表。旧 `/aupdate/workshop` 页面入口跳转至新站点。

服务使用 C#，与现有 aupdate 反馈上传共用 IIS 项目，参见 [部署和管理方式](../services/iis-feedback-upload/README.md)。AI 主题技能已包含用户指定的 ACECode v0.9.14 深、浅两套首页模板，按已确认色系选择对应底图并保留实际布局。

## 本地主题协议

AI 主题使用 `ai-<slug>` 标识，允许小写 ASCII 字母、数字及短横线，总长度不超过 64；不会覆盖蓝色、橙色或 EVA 内置主题。`theme.json` 沿用 `schema_version: 1`，包含名称、明暗模式、28 项十六进制颜色，以及背景和缩略图的大小与 SHA-256。旧主题包的 `theme.json`、`background.png`、`thumbnail.png` 三个根文件保持不变；可选增加声明过的 `session-background.png` 和 `user-message-background.png`，分别对应 `session_background` 和 `user_message_background` 元数据。主题包不包含可执行 CSS、HTML、SVG 或应用代码；矢量源稿通过 Browser 输出 PNG 后安装。

可选顶层 `appearance` 接受以下参数；颜色只允许 `#RRGGBB`，通顶只允许 boolean，不透明度只允许 0–1 的有限数值，不接受未声明字段或 CSS：

| 参数 | 用途 | 省略时 |
| --- | --- | --- |
| `logo_color` | 首页静态/动态图标及侧栏图标主色 | 保留原图标配色 |
| `home_title_color` | 首页问候标题颜色 | 使用 `colors.fg` |
| `extend_to_titlebar` | 首页主内容列壁纸是否延伸至顶栏 | 自定义主题关闭，旧 EVA 保持通顶 |
| `home_composer_opacity` | 首页文本框底色不透明度 | 原有 0.95 |
| `home_background_opacity` | 首页壁纸不透明度 | 1 |
| `session_background_opacity` | 会话页壁纸不透明度 | 1 |
| `user_message_background_opacity` | 用户消息壁纸不透明度 | 1 |
| `home_background_color` | 首页壁纸下的主色底色 | `colors.bg` |
| `session_background_color` | 会话页壁纸下的主色底色 | `colors.bg` |
| `user_message_background_color` | 用户消息壁纸下的主色底色 | `colors.accent-bg` |

例如 `"appearance":{"logo_color":"#9B6DFF","home_title_color":"#F5F0FF","extend_to_titlebar":true}`。该对象随色系确认；修改、增加或移除参数后必须重新确认色系和原型。完整交互及兼容边界见 [主题外观参数设计](../openspec/changes/configure-theme-surfaces/design.md)。

`theme_create` 工具管理当前会话的草稿、人工确认和原子安装；只有位图模式依赖 `image_generate`，HTML/Browser 预览和矢量/本地素材流程不依赖它。风格基座“不拘泥”最终仍解析为实际 `light` 或 `dark` 模式，保证原有通顶按钮逻辑不变。技能不直接修改外观配置。前端只处理当前任务的新完成事件，通过现有外观保存流程应用主题。

草稿存放在 `themes/drafts`；安装后的文件位于 `themes/<id>/<version>/`，通过原子更新 `installed.json` 发布；新 ZIP 位于 `themes/exports/<id>/<version>.zip`。旧版平铺 ZIP 在验证包内主题 ID、版本和内容后仍可复用，删除也按实际归属核验，避免 ID 与版本拼接相似时误删其它主题。确认绑定配色及图片哈希，修改内容后必须重新确认。工具和列表字段详见 [Daemon API](daemon-api.md#ai-theme-workflow-tool)。

## HUD 外观与多风格

主题包可以在顶层（与 `colors`、`appearance` 同级）声明两个可选字段：

```json
"chrome": {"style": "hud"},
"variants": [
  {"id": "red", "name": "荒坂", "colors": {"…": "完整 28 色"}},
  {"id": "cyan", "name": "公共区", "colors": {"…": "完整 28 色"}, "tint": "#22CCF2", "appearance": {"logo_color": "#2BD3F5"}},
  {"id": "yellow", "name": "限制区", "colors": {"…": "完整 28 色"}, "tint": "#F2E022"}
]
```

- `chrome.style` 目前只有 `hud`。切角面板、霓虹描边、侧栏刻度、会话标尺、带凸台的输入框、代码块标题条和语法配色都由 ACECode 自带的样式（`web/src/styles/themeHud.css`）实现，颜色取自当前配色：线条用 `accent`，选中态与发送按钮用 `send-bg`。主题包仍然只含数据，不含 CSS、HTML 或脚本；不认识的 style 直接忽略，按普通主题显示。
- `variants` 为 2–6 项，全部合法才生效，任意一项有问题就整组忽略，避免悄悄改变默认项。第一项是默认风格，应与顶层 `colors` 一致；每项都带完整 28 色。`appearance` 按键覆盖顶层外观，`tint` 让三张背景图按 CSS `color` 混合染色：保留图片明暗，换成 tint 的色相，所以一套图片就能对应全部风格。不带 tint 的风格按原图显示。
- 染色作用于整块背景区域，包括用户消息气泡里图片之外的底色。只要图片顶部纯色与 `user_message_background_color` 亮度一致，长消息就不会出现接缝。
- 兼容性：两个字段都在顶层，旧版 ACECode 和已部署的工坊校验都会忽略它们，旧版显示为默认风格的普通主题；`appearance` 没有新增键。
- 设置 → 外观：当前主题带风格时显示「主题风格」，声明 HUD 时显示「动态效果」开关。选择保存在 `config.json` 的 `web_ui.theme_variant` / `web_ui.theme_effects`（见 [Daemon API](daemon-api.md#get-apiconfigui-preferences)），Desktop 首帧注入，换端口不会丢。
- 动效只在 HUD 主题且「动态效果」开启时运行：
  - 主题生效时的一次开屏扫描；
  - 切换风格时的故障横条和三角警告过场，显示在设置窗口之上；
  - 缓慢下移的扫描光带；
  - 会话水印与首页壁纸的霓虹闪烁；
  - 首页图标外的瞄准环转动；
  - 选中任务的呼吸光；
  - 悬停时的抖动；
  - 输入框聚焦时沿底边的流光，Agent 运行中流光加速。

  动画只用 transform、opacity 或小面积重绘，不跟随系统的「减少动态效果」设置（远程桌面关闭窗口动画时会误报），以设置里的开关为准。

### 主题音效

顶层再加 `"sounds": {"style": "hud"}` 就开启 ACECode 自带的同名音效（`web/src/lib/themeSounds.js`，Web Audio 实时合成，主题包里不放音频文件；旧版与工坊同样当作未知键忽略）。只有声明了音效的主题，标题栏左上角（搜索按钮右侧）才显示静音按钮，开关存 `config.json` 的 `web_ui.theme_sounds`（默认开）。

| 时机 | 音效 |
|---|---|
| 点按按钮、链接、下拉 | 短促数字点击 |
| 选中会话、页签、菜单项、问题导航菱形 | 双音选择 |
| 开关、复选框 | 机械拨动 |
| 发送 / 加入队列 / 停止 | 上扬发射 / 两步落定 / 下沉关机 |
| 回合完成（与桌面通知同一判定，子代理不响） | 三音上行 |
| Agent 提问或请求权限 | 双音提示，响两遍 |
| 回合出错 | 低频故障嗡鸣 |
| 主题生效 / 切换风格 | 开机扫频 / 故障切换，与动效同一时刻 |

浏览器要求先有一次用户操作才能出声，所以刚打开页面时的开机音可能听不到；切回会话时回放的历史事件不出声。音量按峰值约 -18 dBFS（点击）到 -9 dBFS（开机）调好，经压缩器防止叠加时爆音。

### 异形窗口（切角）

顶层再加 `"window": {"shape": "chamfer"}`，Windows 桌面版会把窗口四个角按 45° 切掉 16px，与 HUD 外框画的斜边重合。切角后系统阴影会消失，ACECode 在窗口四周另放几块鼠标可穿透的透明窗口补画阴影，并带一圈当前风格强调色的辉光，失去焦点时变淡。

- 只有声明了这个字段的主题会切角；换成其它主题立即恢复普通矩形窗口和系统阴影。
- 最大化时恢复矩形；还原后重新切角。
- 切角时贴边分屏不会留缝。四边四角照常可以拖动缩放：窗口外侧一圈（与普通窗口看不见的缩放边一样宽）和切掉的角都能按住拖动，角上的拖拽范围沿两条边各延伸 28px。
- 只支持 Windows 桌面版。macOS、Linux 桌面版与浏览器里窗口不变。

## Built-in National Day theme

The built-in `national-day-2026` theme is the approved ACECode-created
Shengshi Huazhang 2026 artwork. Its card is labelled `国庆节` and appears before
EVA Unit-01. The existing background, palette, logo colour and white titlebar
controls are preserved, including the original definition's `mode: dark`.
Only the 86,886-byte preview is bundled; the full package is downloaded from
the configured aupdate server. The original local AI theme remains unchanged.

从下个版本起，Web/Desktop 启动时不再自动下载或应用国庆节主题。新配置使用
蓝色并跟随系统明暗模式；升级后沿用已保存的外观，包括此前保存的国庆节主题。
国庆节主题仍可在「设置 → 外观」中手动下载和选择，沿用下载确认、进度与失败提示。
旧版留下的首次尝试标记不影响新的启动行为，也无需清除。

The approved definition is `assets/themes/national-day-2026/theme.json`. Repackage
the matching artwork without modifying its PNG bytes:

```powershell
./scripts/package_builtin_theme.ps1 -Definition assets/themes/national-day-2026/theme.json -Background '<approved-background.png>' -Thumbnail '<approved-thumbnail.png>' -OutputDirectory build/national-day-theme-package
```

Publish `national-day-2026/1.0.0/{theme.zip,thumbnail.png}` under the aupdate
`themes/` directory, then atomically publish `catalog-v2.json` with National Day
before EVA. Verify public hashes before publishing the catalogue. Keep the
existing `catalog.json` byte-identical: older clients reject a catalogue with
more than one entry. New clients fall back to that legacy catalogue on HTTP
404/410 from the expanded catalogue. Neither publication changes `aceupdate.json`
nor an application release tag. Built-in themes cannot be deleted or exported
through custom-theme management.

## Downloadable EVA theme

EVA Unit-01 is an optional resource pack served independently from the ACECode
application. The application contains the theme ID, renderer, a small preview
thumbnail and three preview swatches. The full wallpaper and complete palette
are external. The card shows its bundled preview immediately, including offline;
only an explicit download confirmation retrieves the full archive.

The thumbnail covers the card at up to 30% opacity, or 60% when selected, with an alpha mask fading
from transparent on the left to the full existing opacity on the right so the
text sits over a quiet background. All cards keep a compact fixed
height of 120px, with square 26px color swatches and tighter internal spacing;
an uninstalled EVA card shows a large white download SVG on hover or keyboard
focus, and progress/cancellation stay inside the card as an overlay. Failed
user actions open a dialog showing the actual failing resource address.
Passive catalogue discovery stays quiet when the server cannot be reached.

EVA uses static purple ACECode logos in the home screen and sidebar. The
ordinary home logo shader is disabled and released while EVA is selected;
switching back restores the existing ordinary-theme animation policy.

On the EVA home screen, the composer, project selector and hint cards keep
95% of the surface colour and let 5% of the wallpaper show through. Text and
icons remain opaque; other themes and pages keep their existing surfaces.
One continuous wallpaper extends behind the home content and title bar, starting
at the left sidebar boundary. The home title bar's right-side controls are white.
Settings, feedback and other pages restore solid title-bar chrome and icons in
the theme colour.

Theme version 1.0.2 uses the built-in blue theme's neutral base (`#F5F5F2`),
white main surfaces and off-white sidebar (`#FBFBF9`) instead of large purple
fills. Body text is dark gray and timestamps are neutral gray. Purple remains
in branding and small action accents; user message bubbles stay pale lavender
(`#F0E7FA`) with subtle lavender borders (`#DFCEF2`). Project selection uses a
soft pale green. This palette-only update reuses the optimized 1.0.1 artwork.

## Package

Use the approved `acecode-eva-background-v2.png` artwork and the palette in
`assets/themes/eva-01/palette.json`. Version 1.0.1 uses perceptual 256-colour
PNG quantization with light dithering (0.4), preserving the original 1433x1098
wallpaper dimensions. The thumbnail is resized from the original to 320x245
before quantization. A 128-colour comparison introduced visible colour bands.
To reproduce the optimized images and package on Windows:

```powershell
python -m venv build/theme-image-env
./build/theme-image-env/Scripts/python.exe -m pip install Pillow==11.3.0 imagequant==1.1.5
./build/theme-image-env/Scripts/python.exe scripts/optimize_eva_theme_images.py --background '<path-to-approved-background.png>' --output-directory build/eva-theme-images
./scripts/package_eva_theme.ps1 -Background build/eva-theme-images/background.png -Thumbnail build/eva-theme-images/thumbnail.png -OutputDirectory build/eva-theme-package-1.0.2 -Version '1.0.2'
```

The output contains `catalog.json` and `eva-01/1.0.2/{theme.zip,thumbnail.png}`.
Supplying `-Thumbnail` preserves the optimized PNG bytes; omitting it retains
the packager's original thumbnail-generation behaviour.
The ZIP has exactly three root entries: `theme.json`, `background.png`, and
`thumbnail.png`. The palette is data, not executable CSS: 28 named hex color
tokens and fixed `mode:"light"`. Background and thumbnail metadata specify
their byte counts and SHA-256. The catalogue specifies the archive and preview
byte counts, SHA-256, versioned relative paths, and the three preview swatches.

Copy only the small thumbnail to `web/public/themes/eva-01-thumbnail.png` for
the bundled card preview. Keep `theme.zip`, the full background and `theme.json`
out of application packaging resources. The archive limit is 16 MiB and preview
limit is 256 KiB.
Use a new version if any approved artwork or palette changes after publication.

## Independent publication

The Appearance theme grid also reads approved themes dynamically from the
independent workshop. Its address uses the update server's effective port plus
10000 (for example, 82 → 10082). Opening Settings, returning from the workshop,
or changing the update source refreshes the list. All public pages are read;
the newest semantic version of each theme ID appears as one card with its local
installation state. Select a card to download, verify, install and apply it;
there is no manual ZIP import step. Workshop updates use the same confirmation
and progress flow as built-in downloads. Installed themes remain usable offline
or after withdrawal. A workshop connection failure shows a retry action.

The current aupdate root is `J:/jenkins_green/aupdate`, publicly served at
`http://2017studio.imwork.net:82/aupdate/`. Copy the versioned ZIP and thumbnail
under `themes/eva-01/1.0.2/`, verify the public downloads against their SHA-256
and byte counts, then publish `themes/catalog.json` last with an atomic rename.
Preserve unrelated catalogue entries if future catalogue versions add themes.
This operation does not update `aceupdate.json`, an application version, or a
Git release tag.

The daemon reads the current `upgrade.base_url` for each catalogue operation, so
changing the update server takes effect without restarting. Catalogue caches
are scoped to their source; a request to a different server cannot silently
reuse the previous server's metadata. An already started download retains its
confirmed resource URL and checksum. A private update mirror can host the
identical `themes/` directory. Local
installations and cached previews live beside the daemon configuration under
`themes/`; switching to another theme retains the downloaded resources.

The catalogue API reports the installed version and whether a newer semantic
version is available. The same card then offers an update with size confirmation
and progress. Successful updates reload the background even when EVA is already
active. Cancelled or failed updates retain the old installation, and an older
mirror catalogue does not offer a downgrade. Published version directories are
immutable and retained when a new catalogue is published.
The confirmation also lets users apply the downloaded version without updating,
so an unavailable update server does not prevent using installed resources.
