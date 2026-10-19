## Context

原任务通过堆快照与隔离 daemon 对照定位侧栏闭包链、React 空更新队列和 webview 桥接 Promise 保留。实现已在当前检出，发布时统一审查纳入。

## Goals / Non-Goals

目标：空闲刷新不产生无效状态写入，事件处理读取最新状态，桥接回包释放资源，Windows 渲染退出自动恢复且不形成崩溃循环。

不在本轮重构 ChatView/App，不改变其他平台恢复策略。

## Decisions

- `useStructuralState` 与 `commitIfChanged` 在调用 setter 前比较内容；稳定 setter/ref 保持最新数据。
- `useStableCallback` 通过 insertion effect 更新当前实现，仅用于事件处理。
- 替换 webview 的 call/onReply，成功、失败及解析错误均移除待处理项；保持线协议。
- 无变化托盘菜单六十秒后允许重发，覆盖 native 缓存重置。
- 仅主渲染进程退出触发重载，滑动十分钟窗口最多三次。使用 LifetimeRef 同步借用宿主；析构先 revoke，再销毁 WebView，排队重载也检查生命周期。
- `set_dev_tools_enabled` 仅同步使用非空 COM 引用，不存储依赖。

## Risks / Trade-offs

深比较增加刷新 CPU 成本，但减少空闲渲染。自动刷新可能丢失页面尚未持久化的瞬态状态，优于永久错误页。原任务已完成侧栏内存对照与 UI 冒烟，不能据此宣称 ChatView/App 内存问题全部解决。

## Validation

原任务记录：3254 条前端测试通过，侧栏工作区改名/未读交替操作 60 次后不再形成旧作用域链。发布补验使用当前源码重新执行前端全量测试与构建、分层检查、RendererRecovery 定向测试和真实 WebView2 OOM 冒烟；结果登记在 tasks.md。
