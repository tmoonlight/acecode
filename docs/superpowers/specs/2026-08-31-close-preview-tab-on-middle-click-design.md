# 预览标签页「中键按下关闭」需求与设计

- 日期：2026-08-31
- 状态：需求澄清完成，已实现
- 分类：Bounded（前端单点改动，复用现有关闭链路）
- 决策轮次：经两轮澄清，最终决策见下（第二轮推翻第一轮的 mousedown/开关选择）

## 1. 背景与动机

ACECode 桌面端的「工作区」里，文档、浏览器、变更等预览内容以**标签页（preview tab）**形式呈现在预览面板（PreviewDetailsPanel）的标签条上。当前关闭一个标签页只有两种方式：

1. 点击标签右侧的小 ✕ 关闭按钮；
2. 右键标签 → 上下文菜单（关闭 / 关闭其他 / 关闭右侧 / 关闭全部）。

用户希望增加一种更快的关闭手势：**把鼠标指针移到某个标签页上，按下鼠标滚轮（中键 / middle button）即关闭该标签页**——与 Chrome、VS Code 的中键关页行为一致。中键无需瞄准小 ✕，能显著提升多标签浏览时的关闭效率。

## 2. 目标

在工作区的预览面板标签条上，支持「中键按下即关闭对应标签页」，并满足以下约束：

- 仅在预览面板标签（文档 / 浏览器 / 变更类）生效；
- 遇到有未保存草稿的文档标签时，走现有的「确认 / 保存后关闭」流程，不静默丢失内容；
- 功能默认开启，**不提供设置开关**（对齐 VS Code / Chrome / 终端模拟器默认行为）；
- 改动集中在前端，不重写已有的纯函数关闭逻辑。

## 3. 范围

### In Scope

- 预览面板标签条上的四类标签：
  - `FILE`（文档）
  - `BROWSER`（浏览器）
  - `SESSION_CHANGES`（会话变更）
  - `GIT_CHANGES`（git 变更）
- 中键（鼠标滚轮按下，`button === 1`）在标签元素上的关闭手势，通过 `auxclick` 事件触发。
- 中键 `mousedown` 时的自动滚动光标抑制（`preventDefault`）。

### Out of Scope

- 顶部会话标签、侧边栏等非预览面板的标签条（本期不做）。
- 滚轮「滚动」手势（上下滚动触发关闭）或「长按滚轮」——本期只做「按下」即关。
- 非预览区域的全局中键行为（如中键在新标签打开链接）。
- 设置开关：明确不做，功能始终开启。

## 4. 功能需求（FR）

- **FR-1** 当鼠标指针位于某个预览标签页元素上，且按下中键（`event.button === 1`）并在抬起后产生 `auxclick` 事件时，关闭该标签页。
- **FR-2** 中键关闭必须复用现有的关闭入口 `onCloseTab`（ChatView 中即 `closePreview`），从而自动继承其「未保存草稿 → 弹确认框」逻辑。
- **FR-3** 中键 `mousedown` 时调用 `event.preventDefault()`，阻止浏览器默认的中键「自动滚动」四向箭头光标出现（该抑制仅挡默认行为，不影响后续 `auxclick` 派发）。
- **FR-4** 功能始终开启，不提供设置开关。
- **FR-5** 关闭行为无额外视觉动画，与点击 ✕ 一致（标签从列表瞬间移除）。

## 5. 设计与实现要点

### 5.1 改动文件

唯一改动文件：`web/src/components/PreviewDetailsPanel.jsx`

标签条渲染位于该组件的 `tabs.map(...)` 内，每个标签是一个
`<button className="ace-preview-details-tab" onPointerDown={...} onMouseDown={handleTabMouseDown} onClick={...}>`。

### 5.2 事件处理（auxclick 方案）

**两处改动，职责分离：**

**(a) 自动滚动抑制（在 mousedown 层）**——`handleTabMouseDown` 顶部守卫扩展：

```js
const handleTabMouseDown = useCallback((event, tabKey) => {
  if (event.button === 1) {            // 中键 mousedown：仅抑制 autoscroll，不在此关闭
    event.preventDefault();
    return;
  }
  if (event.button !== 0) return;      // 其余非左键忽略（原拖拽逻辑不变）
  // ... 原有左键拖拽逻辑 ...
}, [/* 原依赖不变 */]);
```

注意：`preventDefault` 在 `mousedown` 上只阻止默认行为（autoscroll），**不会阻止**后续 `mouseup` / `auxclick` 事件的派发。

**(b) 关闭触发（在 auxclick 层）**——tab `<button>` 新增 `onAuxClick`：

```jsx
<button
  ...
  onPointerDown={(event) => handleTabPointerDown(event, tab.key)}
  onMouseDown={(event) => handleTabMouseDown(event, tab.key)}
  onAuxClick={(event) => {
    if (event.button === 1) {          // 中键抬起后的 auxclick：真正关闭
      event.preventDefault();
      onCloseTab?.(tab.key);           // 复用现有关闭入口（含未保存确认）
    }
  }}
  onClick={...}
>
```

选择 `auxclick` 而非 `mousedown` 作为关闭触发器的原因：`auxclick` 是 W3C 为「非主键点击」定义的语义事件，在按键抬起后才派发，避免与现有 `mousedown`/`pointerdown` 拖拽启动逻辑在同一个事件上竞争分支；macOS 上部分鼠标驱动对中键的 `auxclick` 派发也更稳定。

### 5.3 复用关系（关键，避免重写）

- `PreviewDetailsPanel` 的 prop `onCloseTab` 在 ChatView 中绑定为 `closePreview`
  → `requestPreviewClose`：检测 `previewTabsWithUnsavedDrafts(affected)`，
    若有脏标签则 `setPreviewCloseConfirm(...)` 弹确认框，否则 `performPreviewClose`。
- 因此中键关页与 ✕ 按钮、右键菜单走**同一条关闭链路**，未保存确认免费生效。
- **不改动**：`web/src/lib/previewTabs.js` 的纯函数（`closePreviewTab` 等）、
  ChatView 的关闭链路、右键上下文菜单逻辑、`handleTabPointerDown`。

### 5.4 设置项

不新增任何偏好或设置开关。功能始终开启。

## 6. 边界与已知限制

- **B-1 关闭 ✕ 按钮上的中键**：标签内的 `.ace-preview-details-tab-close` 元素在 `onMouseDown` / `onPointerDown` 上调 `stopPropagation()`，但**没有**对 `onAuxClick` 做 stopPropagation。因此中键正好按在 ✕ 上时，`auxclick` 会冒泡到 tab `<button>` 并触发关闭——即中键点 ✕ 也会关闭该标签。这与旧 mousedown 方案（✕ 上中键不触发）不同，但结果一致（都是关闭），可接受。
- **B-2 标签条空白处**：`onAuxClick` 绑定在标签 `<button>` 上，中键按在标签条间隙/空白不触发关闭。
- **B-3 平台限制（重要）**：macOS 触控板（含 Magic Mouse）**没有中键**，此手势仅在**实体鼠标按下滚轮**时生效。Windows / Linux 实体鼠标为标准支持。因不设开关，无法在 UI 中提示，需在发布说明 / 文档中说明。
- **B-4 与拖拽不冲突**：autoscroll 抑制分支（`button===1`）在 `mousedown` 早返回，不进入左键拖拽流程；关闭触发在独立的 `auxclick` 事件上，与 `button===0` 的拖拽/激活逻辑完全隔离。
- **B-5 ✕ 上的 autoscroll**：因 ✕ 的 `onMouseDown` stopPropagation，中键在 ✕ 上 mousedown 不会到达 tab 的守卫，autoscroll 光标可能短暂出现。边缘情况，可接受。

## 7. 测试要求

- **T-1** 行为测试：在 `PreviewDetailsPanel` 的 tab 上模拟中键 `auxclick`（`button === 1`），断言调用了 `onCloseTab(tabKey)` 且对事件调用了 `preventDefault()`。
- **T-2** autoscroll 抑制测试：模拟中键 `mousedown`（`button === 1`），断言调用了 `preventDefault()` 且**未**调用 `onCloseTab`（关闭只在 auxclick 触发），且未启动拖拽。
- **T-3** 回归：检查现有 `*.architecture.test.js`（基于源码正则断言）是否对 tab 按钮的 JSX / 事件属性有精确匹配；因新增 `onAuxClick` 属性需更新对应断言，确保测试仍通过。
- **T-4**（手动）macOS + 实体鼠标：中键关文档/浏览器/变更标签，验证未保存文档会弹确认框、且中键按下时无 autoscroll 圆圈。

## 8. 验收标准

1. 预览面板的文档 / 浏览器 / 变更标签页，实体鼠标中键按下并抬起后即关闭；
2. 中键按下时**不会**触发浏览器的中键自动滚动圆圈；
3. 中键关闭有未保存草稿的文档标签时，弹出与 ✕ 按钮一致的「保存后关闭 / 不保存关闭 / 取消」确认框；
4. 无设置开关，功能始终启用；
5. 既有左键点击激活、拖拽重排、右键菜单、✕ 按钮关闭行为完全不受影响；
6. 全部相关测试通过。

## 9. 风险与备注

- 风险低：改动集中、复用成熟链路、无新增状态。
- 主要外部依赖是真实硬件（实体鼠标中键）才能端到端验证；CI 仅能验证事件处理逻辑。
- 实现由本 SPEC 经评审批准后，按常规开发流程（含 TDD）落地，无需额外计划文档。
