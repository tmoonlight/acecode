# Transcript 尾部窗口稳定性方案

Status: Approved for implementation

## 目的

大会话只挂载最近一段投影行，避免 Markdown、高亮和 diff 组件一次性同步渲染
整段历史。本方案修复历史被 `transcript_replace` 重建后窗口边界失效、主视图
永久退化为全量 DOM，并在后续高速流式输出时出现卡顿和滚动抖动的问题。

## 现场结论

问题会话在初始状态有 267 个投影行，尾部窗口隐藏 145 行并挂载 122 行。
provider 网络重试连续产生三次 `transcript_replace`；每次 reducer 都为 847 条
历史消息重新分配临时 item id。原边界随即找不到，现有 fail-open 行为把 270
个投影行全部挂载，且组件状态没有重新初始化窗口。后续每个 token 都在全量
transcript 上触发投影、React 协调、Markdown 布局和尾部测量。

Markdown 在流式阶段出现高度回缩是正常布局现象，短会话也会发生；它是抖动
放大器，不是本问题的独立根因。

## 设计原则

1. 窗口边界表达消息身份，不表达某次 reducer 构建出来的对象身份。
2. 历史替换后优先保持用户原来的窗口边界。
3. 原边界确实消失时，同一 render 内重新选择新的尾部边界。
4. 用户显式“显示全部”是独立状态，历史替换不得擅自重新窗口化。
5. 本改动不新增自动 `scrollTop` 写入，不改变 tail-follow/review 状态机。

## 总体结构

```text
投影行:  [早期历史.........................................最新回合]
                    ^ stable anchor key

默认 DOM:
┌──────────────────────────────────────────────────────────┐
│ 显示更早的 N 条消息 / 显示全部                           │
├──────────────────────────────────────────────────────────┤
│ anchor 对应的完整 user turn                              │
│ ...                                                      │
│ 最新流式 assistant                                       │
└──────────────────────────────────────────────────────────┘

transcript_replace:
旧投影 --临时 id 全变--> 新投影
   |                        |
   +---- stable key --------+  保持同一消息边界
```

## 窗口状态

`ChatView` 为当前 session 持有 `anchorKey`：

| 值 | 语义 | 替换后的行为 |
|---|---|---|
| `undefined` | 尚未初始化或当前无条目 | 有条目后选择初始尾部边界 |
| `null` | 全量视图（显式显示全部或短会话不需窗口） | 保持全量，不自动收回 |
| 非空字符串 | 窗口首行的稳定身份 | 存在则保留，不存在则同步重选 |

稳定身份的优先级：

1. 持久化 `messageId`，使用带命名空间的 key；
2. 没有持久化身份时，退回当前 item id；
3. key 只能与同一 `transcriptWindowItemKey` 规则生成的值比较，避免消息 id 与
   临时数字 id 偶然碰撞。

## 协调算法

每次 `renderedItems` 改变时，在窗口切片之前执行：

```text
items empty? -------------------------- yes --> undefined
      |
      no
      v
items <= initial tail threshold? ------ yes --> null (full view)
      |
      no
      v
anchorKey === null? ------------------- yes --> null (explicit full view)
      |
      no
      v
anchorKey exists in new projection? --- yes --> preserve
      |
      no / uninitialized
      v
select initial tail anchor synchronously
```

关键要求：协调结果必须用于当前 render 的 `windowTranscriptItems`，不能等到
effect 后再处理，否则仍会绘制一次全量 transcript。

## 交互契约

| 输入或事件 | 行为 |
|---|---|
| 流式 token 追加 | 保持 `anchorKey`，窗口只向尾部增长 |
| retry `transcript_replace`，原消息仍存在 | 通过 `messageId` 保持原边界 |
| recovery 替换且原消息消失 | 同步重选尾部边界，保持 DOM 有界 |
| “显示更早” | 向前选择稳定 user 边界并补偿 `scrollHeight` 差值 |
| “显示全部” | 设置 `anchorKey = null`，后续替换继续显示全部 |
| 切换 session | 回到 `undefined`，按新 session 初始化 |
| 会话内查找 / 回合跳转 | 沿用现有显式展开与下一帧重试路径 |
| 用户向上回看 | 沿用现有 review 状态，不新增尾部滚动 |

## 错误与回退

- 纯切片函数仍可 fail-open，保证调用错误不会隐藏历史内容。
- `ChatView` 作为主调用方必须先协调失效 key，因此正常渲染不得依赖 fail-open。
- 缺少 `messageId` 时允许退回临时 item id；替换使其失效后必须重新选择边界。
- 小会话不超过阈值时初始边界为 `null`，保持现有全量渲染行为。

## 测试契约

至少覆盖：

1. 相同持久化消息用全新临时 id 重建后，原稳定边界仍可找到；
2. `transcript_replace` 前后大会话隐藏数和可见窗口保持有界；
3. 原边界确实不存在时，同步协调为新的初始尾部边界；
4. `anchorKey = null` 在替换后仍代表用户显式显示全部；
5. 现有逐段揭示、短会话、流式追加和找不到 key 的内容安全测试继续通过；
6. `pnpm test`、`pnpm build`、OpenSpec strict validation 和 `git diff --check`
   全部通过。
