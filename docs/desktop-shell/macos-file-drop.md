# macOS Finder 文件拖放修复备忘

> 状态：已修复并归档。正式行为契约见
> [`../comet/specs/macos-native-file-drop/spec.md`](../comet/specs/macos-native-file-drop/spec.md)，
> 验收记录见
> [`../comet/archive/2026-09-19-fix-macos-native-file-drop/verification.md`](../comet/archive/2026-09-19-fix-macos-native-file-drop/verification.md)。

## 目的与范围

本文记录 Finder 本地文件或文件夹拖入 ACECode Desktop 输入框后被拒绝的问题、
诊断证据、最终修复架构和回归边界，供后续维护和排错使用。

本次修复只改变 macOS Finder file URL 最终释放时的目标判定。拖放悬停样式、
绿色 copy cursor、Mail/Photos file promise 不在本次范围内。输入可靠性高于拖放
反馈，禁止恢复 `object_setClass`、运行时实例子类或输入 responder 替换方案。

## 用户可见症状

修复前，Finder 拖入时可以看到系统 copy 光标，原生层也能取得文件路径，但释放
到 composer 后没有插入任何内容。关键日志为：

```text
[file-drop] macOS performDragOperation count=1
[file-drop] bridge count=1 consoleReceiver=true composerReceiver=true
[file-drop] composer-rejected {"disabled":false,"hover":false,"ageMs":-1,"count":1}
```

这些信息说明失败发生在前端 materialize 之前，而不是文件读取、路径解析或附件
插入阶段。

## 原因与分析结论

原有 native bridge 只传路径，并同时调用 composer 和 terminal 两个接收函数。
两个接收函数各自依赖最近 1.5 秒内的 DOM drag hover 状态判断实际落点。这一规则
适用于原有浏览器/Windows 兼容路径，但 macOS Finder file URL 已被 AppKit 拖放
处理接管，DOM hover 状态不会可靠地到达或保留到原生最终回调。因此 composer
看到 `hover=false`、`ageMs=-1` 后拒绝请求，materialize 根本没有启动。

已确认的证据链如下：

1. AppKit 的 `performDragOperation:` 被调用。
2. pasteboard 中的本地 file URL 被成功转换为路径，数量正确。
3. native-to-web bridge 可见 composer 和 terminal receiver。
4. composer 在 hover gate 处拒绝，且尚未进入 materialize。
5. 改用实际释放坐标后，单文件和多文件都唯一命中 composer，并成功插入。

早期曾尝试通过 `object_setClass` 给 WKWebView 实例安装运行时子类。该方案会改变
输入对象的运行时类型，并在实机上造成输入异常，已完全撤回。最终方案只沿用
WKWebView class-wide 拖放方法 swizzle，不改写实例 class，也不接管键盘、输入法
或粘贴事件。

## 最终修复方案

### 原生坐标采集

macOS 的 `performDragOperation:` 使用 `draggingLocation` 取得窗口坐标，再转换到
WKWebView 本地 bounds。转换显式处理 `isFlipped`，并将 X/Y 归一化为 `[0, 1)`：

```text
Finder drop
  -> AppKit draggingLocation
  -> WKWebView local bounds
  -> flipped Y correction
  -> normalized xRatio/yRatio
  -> native bridge payload
```

归一化坐标避免把 Retina backing pixel 误当 CSS pixel，也避免原生层假设页面缩放
比例。坐标缺失、非有限值或越界时，macOS 的 coordinate-required 请求直接拒绝，
不得降级到 legacy hover 路由。

### 前端唯一目标路由

前端按当前 `window.innerWidth` / `window.innerHeight` 还原 CSS viewport 坐标，使用
`document.elementFromPoint` 查询释放时最上层元素：

- 启用的 `.ace-composer-card`：只调用 composer receiver；
- 可见的 `.ace-console-term`：只调用对应 tab 的 terminal receiver；
- `[data-ace-native-overlay]`、disabled composer、隐藏 terminal 或其他区域：拒绝；
- composer 与 terminal 同时注册时，坐标请求最多交付一个 receiver。

被坐标路由授权的 receiver 不再检查 DOM hover 时间戳，但继续复用既有路径
materialize 和插入逻辑。这样只替换错误的目标判定，不复制文件处理流程。

### 兼容路径与平台影响

本次新增行为只对 macOS 原生 Finder 拖放生效，但实现包含少量共享接口扩展：

- macOS 原生层传 `location`，并设置 `coordinate_required=true`；
- Windows WebView2 仍只传路径，并构造默认 `FileDropContext{}`；
- 默认 context 没有坐标且 `coordinate_required=false`，所以 native bridge 继续把
  路径数组交给原有 composer/terminal legacy receiver；
- Windows receiver 仍使用既有 hover gate，Windows DOM File/WebMessage、
  `file://` fallback 和路径 materialize 语义不变；
- Linux 仍由前端 `text/uri-list` 路径处理，不经过此 native handler；
- 非文件 WebKit 拖放仍调用 WKWebView 原实现。

因此，**用户可见的目标判定变化限定在 macOS Finder file URL 拖放**。共享的 C++
callback 签名和前端 receiver 虽有兼容扩展，但 Windows 不会进入坐标路由。现有
architecture test 也固定了 Windows 必须传空 context、继续走 legacy 分支。
这说明设计和自动回归均要求 Windows 行为不变；不过本轮实机验证是在 macOS
完成的，不能把它表述为所有 Windows/WebView2 版本都做过实机复测。

## 修复流程

1. **恢复安全基线**：撤回 runtime subclass / `object_setClass`，先确认普通输入恢复。
2. **增加无隐私诊断**：确认 AppKit 收到 drop、路径提取成功、bridge receiver 存在，
   并定位到 composer hover gate 拒绝。
3. **限定修复边界**：不全局删除 hover gate，不同时投递两个 receiver，不改输入路径。
4. **传递实际落点**：在 AppKit 最终 drop 时采集、翻转并归一化 WebView 本地坐标。
5. **实现前端 hit test**：用释放时最上层 DOM 元素选择唯一 eligible target，拒绝
   overlay、disabled、隐藏和区域外目标。
6. **保留兼容行为**：无坐标 payload 继续走原 legacy hover gate，Windows/Linux
   和网页内部拖放保持原语义。
7. **覆盖边界测试**：验证坐标缺失/越界、半开区间、overlay、disabled、唯一投递、
   receiver 缺失和 legacy fallback。
8. **实机构建验证**：先验证 ASCII、中文输入法和粘贴，再验证 Finder 单文件及
   多文件拖放；不自动退出、重启或替换正在运行的 ACECode。
9. **精简日志**：实机确认后移除 enter、bridge、route-selected、materialize-start
   及输入/focus 高频日志，只保留安装、最终结果和异常拒绝。

## 日志策略

正常情况下只保留：

- 一条 macOS installation 状态；
- 一条最终 `drop-result`。

异常情况下保留 invalid coordinate、ineligible/disabled/overlay target、receiver
missing、materialize failure 和固定枚举的 receiver exception。日志只能包含数量、
布尔值、目标和固定结果，不得包含文件路径、文件名、文件内容、composer 输入、
按键值或异常正文。例如：

```text
[file-drop] drop-result {"count":2,"accepted":true,"target":"composer","failed":false}
[file-drop] drop-result {"accepted":false,"target":"router","reason":"receiver-exception"}
```

## 验证结果

自动验证已通过：

- 完整 Web 测试；
- Web production build；
- macOS x64 `acecode-desktop` 编译链接；
- `git diff --check`；
- `src/apps/desktop` 和 `web/src` 中不存在 `object_setClass`；
- 坐标、边界、overlay、disabled、唯一 receiver、异常结果和 legacy 兼容测试。

用户实机日志确认：四次单文件和一次双文件拖放均得到有效坐标，只路由到
composer，materialize 最终 `inserted=true`；同一测试期间输入保持正常。日志精简
只删除诊断，不改变功能路径。尚未逐一实测所有 Retina/page zoom 组合，相关正确性
由归一化公式和 helper 测试覆盖。

## 回归排查清单

1. 先确认输入、中文输入法和粘贴正常；若异常，检查是否出现实例 class 或 responder
   改写，绝不以恢复 `object_setClass` 解决拖放。
2. 有 installation 但没有最终结果时，检查实际 AppKit 目标和 native bridge 版本。
3. `invalid-coordinate` 时检查 WebView bounds、窗口到 view 的坐标转换及 flipped Y。
4. `ineligible-target` 时检查 `elementFromPoint` 的最上层元素、overlay 标记和 disabled
   状态，不要绕过覆盖层向 composer 透传。
5. Windows 回归时确认 native 层仍构造空 `FileDropContext{}`，payload 不带 location，
   并继续执行 legacy hover 分支。
6. 对比本地包时检查 desktop 启动日志是否使用仓库 `web/dist`；旧二进制可能加载
   新前端，不能只按二进制时间判断实际组合。

## 相关文件

| 路径 | 作用 |
|---|---|
| `src/apps/desktop/web_host.{hpp,cpp}` | macOS drop 坐标采集、平台原生入口和 Windows 空 context 兼容 |
| `src/apps/desktop/main.cpp` | 结构化 payload、坐标必需语义和 legacy fallback |
| `web/src/lib/macNativeFileDrag.js` | 坐标还原、DOM hit test、唯一目标路由和安全诊断 |
| `web/src/lib/macNativeFileDrag.test.js` | 坐标、目标、拒绝和唯一投递测试 |
| `web/src/components/InputBar.jsx` | composer 坐标授权与 legacy hover 接收路径 |
| `web/src/components/ConsoleDock.jsx` | terminal 坐标授权与 legacy hover 接收路径 |
| [`macos-file-drop-debug.md`](macos-file-drop-debug.md) | 本地构建、实机验证和日志采集指南 |
| [`macos-file-drop-handoff.md`](macos-file-drop-handoff.md) | 修复前历史交接，仅用于追溯 |
