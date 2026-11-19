# 新建会话的等待遮罩

## 目的

从首页发送第一条消息时,界面要先建会话,勾了 worktree 的话 daemon 还要在首条消息的请求里
同步建好 worktree 才返回。大仓库上这一步可能是几秒到几十秒。这段时间界面原先没有任何阻挡:
消息气泡已经出现、输入框里还留着刚发的字、「停止」按钮可点(busy 只是界面先行置位),
用户再发一条会被静默丢掉,点停止会发给一个还没开始回合的会话。

遮罩解决的就是这段等待,并且**只在迟迟没完成时才出现**:正常建会话几十毫秒,一闪而过的
遮罩比没有更糟。

## 行为

| 项 | 取值 | 说明 |
| --- | --- | --- |
| 出现延迟 | 600 毫秒 | 从开始建会话算起,中途从「建会话」进到「建 worktree」不重新计时 |
| 最短停留 | 400 毫秒 | 刚出现就完成时补足,避免闪一帧 |
| 文案 | 「正在新建会话…」/「正在创建 worktree…」 | 会话建好之前是前者,建好之后还在等的只剩 worktree |
| 覆盖范围 | 整个会话工作区(对话列 + 右侧面板) | 不盖左侧会话列表:等待中可以切到别的会话照常发消息,回来时遮罩还在 |
| 顶栏 | 会话按钮(分享 / 侧边聊天 / 会话菜单等)同时禁用 | 顶栏在遮罩外面,靠 `sessionTitleActionStates` 的 `starting` |

覆盖两条路径:

- 首页发送:建会话 →(附件导入)→ 首条消息被接受。内置命令(`/init` 等)在会话建好后就结束等待,
  命令自己的耗时不归这层遮罩。
- 已有的空会话里发首条消息并勾了 worktree。

分叉、任务建议卡片不在范围内,它们有各自的加载反馈。

## 结构

```text
ChatView.jsx  submit / createHomeComposerSession
      |  begin / attachSession / end / isBlocking
      v
lib/useSessionStartMask.js      登记进行中的启动,算出当前界面要不要画遮罩
      |
      v
lib/sessionStartMask.js         纯逻辑:延迟门、归属判断、文案种类
      |
      v
components/SessionStartLoading.jsx  ->  SessionContentLoading(label / trapFocus / anchorKey)
```

- `createSlowPendingGate`:「等了很久才亮,亮了至少亮一会儿」的开关,时钟与定时器可注入,
  测试用假时钟。
- `sessionStartMaskApplies`:遮罩只盖正在启动的那个会话所在的界面。等待中切到别的会话、
  定时任务页,或进过新会话后又回到首页开下一个任务,都不被挡住(`arrived` 就是为最后一种情况)。
- 遮罩挂在 `ChatView` 根节点,首页与会话两个分支共用同一个实例,切换时不重挂。

## 约束

- **出现之前不拦截输入**。阈值之前不吞点击和按键,这段时间的保护只有入口早退:
  `submit` 与 `stopCurrentWork` 开头查 `sessionStartMask.isBlocking()`。
  已知的遗留:消息被接受之前输入框里还留着刚发出的文本,这 600 毫秒内打的字会接在它后面,
  接受后因为内容对不上也不会被清空(改动之前整段等待都是这样)。
- **计时从离开确认之后开始**。`createHomeComposerSession` 先等未保存文件的离开确认,
  确认弹窗开着的时候不计时,遮罩也不会去抢弹窗的焦点。
- **守焦点要等焦点事件结束再拿回**。切进新会话后有代码会自动聚焦输入框,遮罩显示期间要把
  焦点拿回来,否则用户看着遮罩还能打字。但不能在 `focusin` 回调里同步调用 `focus()`:
  Slate 会先处理失焦、后处理这次获得焦点,最后一直以为自己还有焦点,遮罩散掉后再也聚焦不上。
  现在的做法是 `setTimeout(0)` 之后再拿回(`SessionContentLoading` 的 `trapFocus`)。
- **建 worktree 的发送不走默认 30 秒超时**。`api.sendInput` 在请求体带 `worktree.create` 时用
  `WORKTREE_CREATE_TIMEOUT_MS`(5 分钟)。否则大仓库会出现界面报「发送失败」、
  daemon 却照常建完并开始回合。
- 请求结束(成功、失败、超时)一定调用 `end(token)`;token 不是当前这次启动的会被忽略,
  所以旧请求的收尾不会清掉新一次的等待。

## 验证

单元与接线测试:`web/src/lib/sessionStartMask.test.js`、
`web/src/lib/sessionStartMaskArchitecture.test.js`、`web/src/lib/api.test.js`。

正常建会话触发不了遮罩,真实页面里要人为放慢:在页面里包一层 `fetch`,给
`POST /api/sessions`(或 `/api/workspaces/<hash>/sessions`)和 `POST /api/sessions/<id>/messages`
加延迟或抛错,再看这几档:300 毫秒(什么都不出现)、700 毫秒左右(出现并停满 400 毫秒)、
几秒、请求失败、等待中切到别的会话或回到新建任务页。

两个容易看错的地方:

- 自动化工具的「输入文本」多半走 `Input.insertText`,它跟着文档选区走,焦点在遮罩上时照样能
  写进输入框。要用真实按键事件验证拦截。
- 页面在后台(面板被隐藏)时 `requestAnimationFrame` 降到每秒一次,遮罩散掉后焦点回到输入框
  会显得晚几百毫秒,前台窗口里是下一帧。
