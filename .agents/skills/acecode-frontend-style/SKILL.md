---
name: acecode-frontend-style
description: ACECode WebUI 的统一视觉规则 — 写设置 / 管理 / 配置 / 状态展示类页面或面板前必读。提炼自 SettingsPage 的「常规」「外观」section,覆盖容器、标题层级、卡片行、单选卡片、小型 input、状态指示、分隔线等。新 section 直接套这套 className,不要重新造视觉风格。
metadata:
  author: acecode
  version: "1.0"
---

# ACECode 前端统一样式

ACECode WebUI 的设置 / 管理 / 配置类面板共用一套 Tailwind v4 className 约定。新写 section / 抽屉 / 表单时按这里的规则套 className,**不要重新发明视觉风格**。

技术栈:React 18 + Tailwind v4 + 自定义 CSS 变量(`--ace-bg` / `--ace-surface` / `--ace-border` / `--ace-fg` / `--ace-fg-mute` / `--ace-accent` / `--ace-ok` 等,定义在 `web/src/styles/globals.css`)。亮 / 暗双主题靠这些变量切。**所有色值必须走 token,不要硬编码 hex。**

## 0. 字体规范

ACECode 只使用两套字体栈,统一由 `web/src/styles/globals.css` 的 Tailwind theme 变量管理:

```css
--font-sans: -apple-system, BlinkMacSystemFont, "PingFang SC", "Segoe UI", sans-serif;
--font-mono: "SF Mono", "Cascadia Code", "Fira Code", Menlo, Consolas, monospace;
```

- 所有常规 UI、中文正文、标题、按钮、标签、输入框、文件路径、版本号、模型名、ID、数字和状态信息均使用主字体 `font-sans`。应用根节点已统一设置时应直接继承,不要到处重复添加 `font-sans`。
- 只有真实代码内容和 diff 内容使用 `font-mono`,包括代码块、inline code、源码预览、终端内容、命令文本、结构化配置源码和 diff 行。
- 不要因为内容看起来“技术化”就使用等宽字体。路径、版本、统计数字、快捷命令名称、模型 ID、配置字段值本身仍是普通 UI 文本。
- 不要在组件中另写第三套 `font-family`,也不要使用 `system-ui`、`ui-monospace` 或 Tailwind 默认字体栈。图标字体等专用资产字体不受此规则影响。

---

## 1. 页面外壳

外层是 SettingsPage 类容器,真正写 section 时只关心**内容滚动区**这一层:

```jsx
<div className="flex-1 overflow-y-auto px-12 py-6">
  {/* 你的 section 在这里 */}
</div>
```

- 横向 padding `px-12`(48px),纵向 `py-6`(24px)
- 滚动归外壳,内容自由长

## 2. Section 标题

设置界面只允许两级标题使用加粗字重：页面主标题（如“配置”）和一级分组标题（如“升级服务”）。字段名、选项、卡片名称、导航文字、列表项和更深层标题一律使用 `font-normal`（400），不要使用 `font-medium`、`font-semibold` 或 `font-bold`。需要强调时使用 `text-fg`，说明文字使用 `text-fg-mute`；选中状态也不增加字重。独立弹窗的主标题可保留加粗，弹窗内部仍遵守同一层级规则。

每个 section 顶上一行 H2,然后空 5(`mb-5`)。

```jsx
<h2 className="text-xl font-bold mb-5">常规</h2>
```

## 3. 一级分组(同 section 里的分组标题 + 描述)

**Avoid unnecessary secondary labels and descriptions. Let the layout do the explaining.**

默认只显示分组标题，以间距、对齐和控件本身解释结构。只有影响决策、非显而易见的行为或错误恢复才补充一句简短说明，不复述标题、开关或预览，不改为默认 tooltip。必要说明使用 `text-[12px] text-fg-mute mb-3`。

```jsx
<div className="text-[14px] font-semibold mb-3">权限模式</div>
{/* 行卡片 / 卡片网格 */}
```

## 4. 行卡片(单条配置项)

最常用的容器 — 一行一项配置。**左侧文本块 + 右侧操作元素**,用 `justify-between` 撑开。

```jsx
<div className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
  <div>
    <div className="text-[13px] font-normal text-fg">最大轮次</div>
  </div>
  {/* 右侧:Toggle / 小 input / status pill / 下拉 / 按钮 */}
</div>
```

- 左侧主标 13px normal + fg；必要的副描述才使用 11px fg-mute 和 `mt-0.5`，不默认添加。
- 卡片间隙 `mb-2`,padding `px-3.5 py-2.5`
- 圆角 `rounded-md`,描边 `border-border`,底色 `bg-surface`

### 4.1 行卡片可点击(role=button / radio)

行整体可点 → 加 `cursor-pointer hover:bg-surface-hi transition`,语义按 ARIA:

```jsx
<div
  role="radio"
  aria-checked={selected}
  tabIndex={0}
  onClick={...}
  onKeyDown={(e) => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); ...; } }}
  className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2 cursor-pointer hover:bg-surface-hi transition"
>
  ...
</div>
```

## 5. 单选卡片网格(主题选择那种大卡片)

二/三卡平铺挑一个 — 用 `grid grid-cols-N gap-3 max-w-md`。每个卡片是 `<button>`,active 描边加粗 + 浅色底,右上角放一个圆点表示选中:

```jsx
<div className="grid grid-cols-2 gap-3 max-w-md">
  {options.map((opt) => {
    const active = selected === opt.key;
    return (
      <button
        key={opt.key}
        type="button"
        onClick={() => onSelect(opt.key)}
        className={clsx(
          'relative p-3 rounded-lg border text-left transition',
          active ? 'border-accent border-2 bg-accent-bg' : 'border-border bg-surface hover:border-accent/50',
        )}
      >
        {/* 卡片预览(色块/图标/文字) */}
        <div className="flex gap-1 mb-2">
          <span className="w-6 h-6 rounded border border-border" style={{ background: '#xxx' }} />
        </div>
        <div className="text-[13px] font-normal text-fg">{opt.label}</div>
        {active && <span className="absolute top-2 right-2 w-2.5 h-2.5 rounded-full bg-accent" />}
      </button>
    );
  })}
</div>
```

- 默认 `border-border bg-surface`,hover 描边淡 accent(`hover:border-accent/50`)
- active 双倍粗描边 + accent 浅底(`border-accent border-2 bg-accent-bg`)
- 选中圆点固定 `top-2 right-2 w-2.5 h-2.5 rounded-full bg-accent`

## 6. 小型 input(行卡片右侧那种数字 / 文本输入)

放在行卡右侧,**短**,不撑满:

```jsx
<input
  type="number"
  value={...}
  onChange={...}
  className="w-20 h-7 px-2 text-[12px] text-center rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition"
/>
```

- 宽度按内容定:`w-20`(数字)、`w-32` / `w-48`(短文本)
- 高度固定 `h-7`,字号 `text-[12px]`
- `bg-surface-alt`(比卡片底略浅一档),focus 时描边变 accent
- 文本居中(数字)用 `text-center`,普通文本去掉

### 6.1 全宽 input(表单字段)

ModelManager / 其他表单里左侧 label + 右侧 input,input 撑满 flex 容器:

```jsx
<label className="flex items-center gap-2 text-[12px]">
  <span className="w-20 text-right text-fg-mute">字段名</span>
  <span className="flex-1 [&>input]:w-full [&>input]:px-2 [&>input]:py-1 [&>input]:border [&>input]:border-border [&>input]:rounded [&>input]:bg-surface-alt [&>input]:text-fg [&>input]:outline-none [&>select]:px-2 [&>select]:py-1 [&>select]:border [&>select]:border-border [&>select]:rounded [&>select]:bg-surface-alt [&>select]:text-fg">
    {children}
  </span>
</label>
```

label 列固定 `w-20 text-right text-fg-mute`,值列 `flex-1`,input/select 通过孙选择器 `[&>input]:...` 一并样式化。

## 7. 分隔线

section 内不同子分组之间:

```jsx
<div className="h-px bg-border my-5" />
```

## 8. 状态指示(运行中 / 错误 / 警告 等)

绿点 + 文本(用于"运行中""已连接"等正向状态):

```jsx
<span className="flex items-center gap-1.5 text-[12px] text-ok">
  <span className="w-2 h-2 rounded-full bg-ok shadow-[0_0_4px_var(--ace-ok)]" />
  运行中 · 端口 {port}
</span>
```

- 颜色 token:`ok`(绿)/ `danger`(红)/ `warn`(琥珀)/ `accent`(主)/ `fg-mute`(次)
- 圆点 `w-2 h-2`,带柔光阴影 `shadow-[0_0_4px_var(--ace-ok)]`(把 var 换成对应颜色)

## 9. 主操作按钮 / 次按钮

### 设置页即时保存（必须遵守）

- 设置页和配置展开区不放独立“保存”“应用”“取消更改”按钮；仅用户明确要求时允许例外。
- 开关、下拉和单选变更后立即自动保存；文本和数字输入在失焦或按 Enter 时自动保存，避免把输入中的半成品逐字提交。
- 收起、切换分组、关闭设置及窗口失焦时补齐有效更改；保存队列归连接级 store 所有，不能随着组件卸载丢失。
- 串行提交并保留请求期间的新编辑；重新打开先等待队列，防止旧 GET 覆盖新设置。输入无效或保存失败时保留草稿，显示必要错误与重试入口，不弹逐字段保存成功提示。
- “测试连接”等按钮保留其业务用途，并在执行前等待当前更改保存成功；不能兼作隐含的唯一保存入口。
- 工具行沿用相邻行的结构；图标框带 `data-settings-surface`。必须在真实 `.ace-settings-content` 中验证布局和自动保存，不能只验孤立卡片。

以下主按钮模板用于创建、执行等显式业务动作，不用于设置页保存：

```jsx
<button
  type="button"
  onClick={...}
  disabled={busy}
  className="px-3 py-1 bg-accent text-white rounded disabled:opacity-60"
>
  新增
</button>
```

次按钮 / 链接按钮(列表行内的「设为默认」「删」):

```jsx
<button
  type="button"
  className="px-1.5 py-0.5 text-[11px] hover:underline"
  onClick={...}
  disabled={busy}
>
  设为默认
</button>
{/* 危险动作:加 text-danger */}
<button className="px-1.5 py-0.5 text-[11px] text-danger hover:underline">删</button>
```

## 10. 颜色 token 速查

| token | 用途 |
|---|---|
| `bg`/`text-fg` | 页面级背景 / 主文字 |
| `surface` | 卡片底(比 bg 更浅一层) |
| `surface-alt` | input 底(比 surface 再浅一档) |
| `surface-hi` | hover 高亮底 |
| `border` | 通用描边 |
| `fg` / `fg-2` / `fg-mute` | 文字 主 / 次 / 提示 |
| `accent` / `accent-bg` | 主色 / 主色浅底 |
| `ok` / `danger` / `warn` | 状态绿 / 红 / 琥珀 |

亮 / 暗值由 `web/src/styles/globals.css` 切,组件层只用 className,不要写 hex。

## 11. 反例(不要这样)

```jsx
{/* ❌ 自定义底色 */}
<div className="bg-gray-100 dark:bg-gray-800 ..." />

{/* ❌ 行卡片描边写 border-gray-200 */}
<div className="border border-gray-200 ..." />

{/* ❌ 字号靠 text-xs / text-sm 估 */}
<div className="text-xs text-gray-500" />

{/* ❌ 手写颜色 */}
<button style={{ background: '#2563eb' }} />
```

正确做法是用 token / `text-[Npx]` 显式像素值(参 § 3-9)。

## 12. 实现新 section 的 checklist

写新 settings section / 类似管理面板时:

- [ ] H2 用 `text-xl font-bold mb-5`
- [ ] 一级分组只保留明确标题与合适间距；必要描述使用 `text-[12px] text-fg-mute mb-3`
- [ ] 仅主标题、一级标题可加粗；字段、卡片、选项、导航及更深层标题均为 400，强调使用文字颜色
- [ ] 配置项用 § 4 行卡片(左 13px normal + fg,右 toggle/input/pill)，不默认添加次级描述
- [ ] 多选一用 § 4.1 row-radio,多选卡用 § 5 grid
- [ ] input 走 § 6.0 / § 6.1 模板
- [ ] 状态指示走 § 8 绿点
- [ ] 设置页遵守 § 9 即时保存，不设独立保存按钮；失焦、Enter、收起、离开及失败重试已验证
- [ ] 显式业务动作的主/次按钮使用 § 9 模板
- [ ] 颜色全部走 token,**不要 hex / `border-gray-X` / `bg-gray-X`**
- [ ] 亮 / 暗主题不要写双 className,token 自带
- [ ] 常规 UI 继承 `font-sans`;仅代码和 diff 使用 `font-mono`;不新增第三套字体栈

## 13. 参考实现

`web/src/components/SettingsPage.jsx` 里 `SectionGeneral` 与 `SectionAppearance` 是最完整的参考。`web/src/components/ModelManager.jsx` 演示行内表单 / 列表 / 主次按钮组合。读这两个文件总比凭记忆更可靠。
