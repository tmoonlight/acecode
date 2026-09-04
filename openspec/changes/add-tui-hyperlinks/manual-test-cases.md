# add-tui-hyperlinks 真机验收：提示词 + 用例手册

配套 `verification-checklist.md`（勾选表）使用。本文件是操作手册：**每批先复制提示词发给 ACECode，等它回复渲染，再做对应验证动作**。

## 前置（每条用例通用）

- 跑新构建：`./build/macos-x64-release/acecode`（gitlink `f98588b4`，含 hover 补丁 + OSC 8）
- 每批开始前 `/new` 开干净会话，避免旧链接干扰 link-region 命中
- 素材靠"让模型逐字复述"生成。**若回复里出现代码块包裹或文字被改写**（渲染形态不对），补一句"不要用代码块包裹，逐字复制"重试
- 链接渲染形态：可点击链接 = 链接色 + 下划线；纯文本 = 普通颜色

> ⚠️ 原理备忘：TUI 里**用户消息按纯文本渲染**（不解析 markdown），只有 **assistant 回复**走 markdown 渲染、才产生可点击链接。因此下面每一条"提示词"都是**发给 ACECode 的对话消息**，让它回复出目标 markdown 文本；ACECode 的回复内容即被测对象。请在**真实终端窗口**（iTerm2 / Terminal.app / Windows Terminal）里直接运行 `acecode` 二进制，不要在嵌套终端/重定向环境里跑（会干扰鼠标事件与 OSC 8 探测）。

---

## B1 标准链接素材（iTerm2 等全部终端的基础批）

**提示词（复制整段发给 ACECode）：**

```
逐字输出下面这些 markdown 行作为你的回复正文。不要用代码块包裹，不要增删改任何字符，不要加解释：
---
- 打开官网：[ACECode 官网](https://example.com)
- 直接点这里：[GitHub](https://github.com)
- 本地文档：[项目说明](/Users/liuxin557/DEV/acecode/README.md)
- 伪装链接测试：[github.com/ACECode](https://evil.example.com)
- 同 host 短写：[github.com/ACECode/issues](https://github.com/ACECode/issues/1)
- 普通文字标签：[安装指南](https://example.com/guide)
裸网址 https://github.com 这一行没有方括号语法，应显示为普通文本
---
```

**预期渲染**：第 1/2/3/5/6 行（官网/GitHub/本地文档/同 host 短写/安装指南）= 链接色+下划线；**第 4 行（伪装链接）** = 普通文本色、无下划线（label host `github.com` ≠ href host `evil.example.com`，被降级）；最后一行裸网址 = 普通文本。

**验证动作（对应 checklist 1.1–1.7）：**

| # | 动作 | 预期 |
|---|---|---|
| 1.1 | 左键点击"ACECode 官网" | 默认浏览器打开 example.com |
| 1.2 | 左键点击"GitHub" | 浏览器打开 github.com |
| 1.3 | 左键点击"项目说明" | Finder/文件管理器定位到仓库根 README.md（本地路径，应用内打开） |
| 1.4 | 观察 + 点击"伪装链接测试"行 | 纯文本样式；点击无任何反应（不进可点击区） |
| 1.5 | 点击"同 host 短写"行 | 浏览器打开 github.com/ACECode/issues/1（label host == href host，放行） |
| 1.6 | 点击"安装指南"行 | 浏览器打开 example.com/guide（label 无点号非 URL 形状，放行） |
| 1.7 | 点击"裸网址"行 | 无反应（非链接） |

---

## B2 OSC 8 原生链接（仅白名单终端：iTerm2 / kitty / WezTerm / Windows Terminal）

**提示词 A（基础，验证 Cmd/Ctrl+点击与悬停）：** 先完成 B1，复用 B1 渲染出的链接。

**验证动作（对应 checklist 1.9–1.11、3.1）：**

| # | 动作 | 预期 |
|---|---|---|
| 1.9 | Cmd（WinTerm 用 Ctrl）悬停链接 | 终端显示原生超链接样式（手型/下划线增强/URL 提示） |
| 1.10 | Cmd+点击"GitHub"链接 | iTerm2 原生打开浏览器（不经应用内逻辑，URL 是真实 https://github.com） |
| 1.11 | 右键链接 | 原生菜单出现"打开链接 / 拷贝链接" |

**提示词 B（跨行链接，验证行边界关闭与无多余发射）：**

```
逐字输出下面内容作为回复正文。不要用代码块包裹，不要改写：
---
跨行超长链接测试，请点击这一长串：[https://example.com/very/long/path/abcdefghijklmnopqrstuvwxyz/0123456789/abcdefghijklmnopqrstuvwxyz/0123456789/abcdefghijklmnopqrstuvwxyz/0123456789/end](https://example.com/very/long/path/abcdefghijklmnopqrstuvwxyz/0123456789/abcdefghijklmnopqrstuvwxyz/0123456789/abcdefghijklmnopqrstuvwxyz/0123456789/end)
---
```

窗口调窄使该行换行。**验证（1.14–1.15）：** 跨行后整串仍是一个链接（悬停/点击/右键均正确，不串到相邻行）；若无法肉眼判断，见下方字节级法。

**字节级验证（可选，Linux/macOS）：** 在 `script -q /tmp/tty.log ./build/macos-x64-release/acecode` 中跑完 B2-B 后退出，检查：

```bash
# OSC 8 开序列 \x1b]8;;URL\x1b\ 与关序列 \x1b]8;;\x1b\
python3 - <<'PY'
import re
d = open('/tmp/tty.log','rb').read().decode('utf-8','ignore')
pairs = re.findall(r'\x1b]8;;(.*?)\x1b\\', d)
print('osc8 sequences:', len(pairs))
PY
```

---

## B3 悬停气泡（hover motion 终端：iTerm2 / kitty / WezTerm）

**前提**：B1 素材已渲染。**鼠标不按键**操作。

**验证动作（对应 checklist 1.12–1.13、3.4）：**

| # | 动作 | 预期 |
|---|---|---|
| 1.12 | 指针悬停在"GitHub"链接上不动约 300ms | 指针附近浮出气泡，显示真实 URL `https://github.com` |
| 1.12b | 悬停"伪装链接测试"行（纯文本） | 无气泡 |
| 1.13a | 气泡可见时把指针移出链接区域 | 气泡消失 |
| 1.13b | 再悬停出气泡，按 Esc | 气泡消失 |
| 1.13c | 气泡出现后按任意其他键 / 拖动鼠标 | 气泡消失，无布局跳动（dbox 叠加不参与布局） |

**提示词（可选，若需要无歧义素材：悬停长 URL 看气泡完整值）：**

```
逐字输出下面内容作为回复正文，不要用代码块包裹：
---
悬停气泡长 URL 素材：[点我看完整地址](https://example.com/a/very/long/path?query=1&lang=zh#section)
---
```

气泡应显示完整 URL（含 query 与 fragment），而非显示文字。

---

## B4 Apple Terminal.app 字节级回退（macOS 自带终端）

**前提**：Terminal.app 不在 OSC 8 / hover 白名单 → 应完全回退。跑同一批 B1 素材。

**验证动作（对应 checklist 2.1–2.4）：**

| # | 动作 | 预期 |
|---|---|---|
| 2.1 | 观察链接 | 仍是 TUI 自己的链接色+下划线（应用层样式），无终端原生超链接效果 |
| 2.2 | 左键点击"GitHub" | 浏览器照常打开（应用内 link-region 兜底，不受无 OSC 8 影响） |
| 2.3 | 悬停链接 300ms | **无气泡**、屏幕无抖动/无异常重绘（hover 探测关闭，未发 `?1003`） |
| 2.4 | 点击"项目说明"本地链接 | Finder 打开（与旧版一致） |
| 2.5 | 观察滚动/拖选文本 | 无鼠标事件异常（?1003 未启用，拖选行为与旧版相同） |

---

## B5 Windows Terminal（需要 Windows 构建，另机或 CI 产物）

用与 B1 相同的素材（OSC 8 在 WinTerm 白名单内）。**对应 checklist 3.1–3.4：**

| # | 动作 | 预期 |
|---|---|---|
| 3.1 | Ctrl+点击链接 | 系统浏览器打开（OSC 8 原生） |
| 3.2 | 快速移动鼠标划过链接/文本 | 无闪屏、无整屏重绘抖动（hover motion 未开启或安全） |
| 3.3 | 观察"伪装链接测试"行 | 纯文本（降级逻辑与平台无关） |
| 3.4 | 悬停链接约 300ms | 无气泡或按平台行为显示（记录实际） |

---

## B6 6.5 附注核实终端（顺带记录，没有可跳过）

| 终端 | 做法 | 记录点 |
|---|---|---|
| kitty | 跑 B1+B2+B3 | OSC 8 原生、Cmd+点击、悬停气泡、跨行 |
| WezTerm | 跑 B1+B2 | OSC 8 原生、Ctrl+点击 |
| 老式/经典 conhost | Windows 老控制台跑 B1 | 无 OSC 8、无气泡、无闪屏回归 |

---

## 收尾

- 全部符合预期 → `verification-checklist.md` 对应项打勾，勾选 tasks.md 6.4/6.5 → `openspec archive add-tui-hyperlinks`
- 任何 FAIL → 记录终端 + 现象 + 复现素材，回填 tasks.md 备注转代码修复
