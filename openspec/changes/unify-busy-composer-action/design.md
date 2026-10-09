## Context

`InputBar` 当前分别渲染红色中断按钮及运行中带文字的排队按钮；空闲发送按钮使用另一段 JSX。`inputBarState` 集中判定文本、附件、禁用、提交请求和暂停队列继续语义，`getGoalStopControlState` 保留停止请求在途状态。动机见 proposal.md。

## Goals / Non-Goals

**Goals:** 让主动作的模式由已有纯状态函数统一决定，按钮只负责显示和分派动作；复用现有图标体系及主题变量。

**Non-Goals:** 不修改后端、队列管理、停止快捷键或富文本键盘适配器，不改变空闲发送/重试/继续语义。

## Decisions

1. 为 `getInputBarActionState` 增加 `stop` 模式，使用已存在的 `hasText` 与 `hasExtras` 判定真正空草稿。直接在 JSX 另判文本会遗漏附件和上下文。
2. `canSubmit` 继续只表示发送/排队/继续资格，停止模式保持 false。按钮点击按模式调用 `onAbort` 或既有 `submit`；Enter 继续调用 `submit`，避免空框回车突然中断任务。
3. 主按钮统一为现有 28px 圆形按钮，始终 `bg-accent`，使用对应 ARIA 标签、tooltip、焦点样式。停止请求在途复用已有停止状态，提交请求不会阻挡停止。
4. 新增 `Queue` 图标及 `queue` 语义别名，使用三条队列线加折向队尾的箭头；通过图标生成脚本导出 SVG。为本按钮添加填充的 `StopFilled` 方形图标，不改变其他界面的既有 `Stop` 轮廓图标。

## Risks / Trade-offs

- 隐藏独立停止按钮后，有草稿时需清空草稿或使用已有停止快捷键来中断 → 主按钮精确遵循用户指定的内容切换，保留现有停止快捷键。
- 文本为空但附件存在时可能误判停止 → 使用现有 `hasExtras`，补纯函数和浏览器交互覆盖。
- 修改按钮可能破坏暂停队列继续和空闲重试 → 复用原来的 `submit` 和状态闸门，运行已有回归测试。
- Web 构建不更新已运行的 Desktop → 验证明确限定为源码/Web，Desktop 发布另行执行。
