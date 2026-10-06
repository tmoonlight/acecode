## Context

顶部 `.office-controls` 常驻；Windows 创建窗口与 `apply_placement` 都使用 TOPMOST，macOS 使用 floating NSPanel。两端目前无条件把顶部 32 场景像素计入命中区域。共享页面通过 overlay 消息上报气泡与成员列表。

## Goals / Non-Goals

实现用户指定的悬停菜单、置顶切换与关闭。保留房间、动画、会话切换、跟随和拖动缩放。不改 daemon 协议，不增加用户设置页面；pin 状态在当前桌宠生命周期内保留，重启默认置顶。

## Decisions

- 页面管理唯一一个 1000ms 隐藏计时器。进入场景、菜单或键盘焦点时取消计时；离开后计时。鼠标点击留下的焦点不会永久锁住菜单，键盘焦点则保持可见。
- 控件隐藏使用 opacity 与 pointer-events，不移除键盘顺序；焦点进入时显现。保留现有浅纸色按钮，pin 以 aria-pressed 和选中底色表达状态，两个按钮都有中文可访问名称。
- overlay 新增独立 `controls` 矩形，隐藏时为 null；原生只合并实际显示的控件矩形与房间/气泡，移除固定顶部区域。几何解析仍执行边界、类型和数量限制。
- `pin` 消息携带 boolean；宿主以 `pet-window-state` 回传实际 pinned 状态，ready 时同样回传。Windows 用 SetWindowPos TOPMOST/NOTOPMOST，普通布局调用改为 SWP_NOZORDER；macOS 切换 floating/normal level。
- `close` 通过宿主下一轮窗口消息/主队列调用现有 close，避免在 WebView 回调中销毁自身。主界面和 agent 任务继续运行。

## Risks / Trade-offs

菜单与房间之间有透明空隙，延迟隐藏允许鼠标跨越空隙。最小 172px 宽度仍须容纳五个会话、跟随、pin 和关闭，按钮尺寸随现有紧凑断点收紧。Windows 本机编译与浏览器自动化验证；macOS 修改只做源码核对，无法在 Windows 宣称实测。

## Validation

扩展现有浏览器回归，检查默认隐藏、进入/离开/重新进入、鼠标与键盘焦点、pin 消息/回执、关闭和窄窗口。原生几何回归验证隐藏区域穿透、显示区域命中和浮层限制。Windows 增量构建并运行 DesktopPetLayout 测试；记录源码验证与运行中 Desktop 验证边界。
