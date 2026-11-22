# Tickets —— redesign-html-page-annotations

**这里只是索引。工单正文以 GitHub issue 为唯一事实来源**（验收清单、`Blocked by`、讨论都在 issue 上）。改口径请改 issue，回来同步这一页的标题。

- 方案：[#207](https://github.com/tmoonlight/acecode/issues/207)
- 决策记录：`docs/specs/2026-11-21-html-page-annotations-technical-design.md`
- WIP 现状：`docs/specs/2026-11-22-html-page-annotations-wip-alignment.md`

## 仓库准备（不占工单）

**提交当前 WIP 作为基线**：`git add -A && git commit -m "wip: codex visual parity, style panel, native overlay geometry"`。51 个改动里有 4445 行，不先落一条基线，后面「记录上提」的 diff 会混在旧路线代码里，评审分不出哪些是搬运、哪些是新逻辑。**这是 #209 开工的实际前提**（issue 里没有单独建单，因为它是仓库动作不是功能）。

---

## 工单

| # | 工单 | 阻塞于 |
|---|---|---|
| [#208](https://github.com/tmoonlight/acecode/issues/208) | T0 技术验证：定位与捕获耗时 | — |
| [#209](https://github.com/tmoonlight/acecode/issues/209) | T1 批注存储 expand（双写） | #208 |
| [#210](https://github.com/tmoonlight/acecode/issues/210) | T2 读路径迁移到存储 | #209 |
| [#211](https://github.com/tmoonlight/acecode/issues/211) | T3 contract：关页签不丢批注 | #210 |
| [#212](https://github.com/tmoonlight/acecode/issues/212) | T4 截图前移到保存 | — |
| [#213](https://github.com/tmoonlight/acecode/issues/213) | T5 发送组装收口到 `submitWithAnnotations`（工具栏按钮保留） | #212 |
| [#214](https://github.com/tmoonlight/acecode/issues/214) | T6 删净批次链路 | #213 |
| [#215](https://github.com/tmoonlight/acecode/issues/215) | T7 超时自查 | #213 |
| [#216](https://github.com/tmoonlight/acecode/issues/216) | T8 待发卡片完整化 | #211 |
| [#217](https://github.com/tmoonlight/acecode/issues/217) | T9 铅笔路径 A：回页面编辑 | #216, #210 |
| [#218](https://github.com/tmoonlight/acecode/issues/218) | T10 铅笔路径 B：无页签弹窗 | #216 |
| [#219](https://github.com/tmoonlight/acecode/issues/219) | T11 已发卡片只读化 | #213 |
| [#220](https://github.com/tmoonlight/acecode/issues/220) | T12 轮询容错与快捷键 | #217 |

## 依赖图

```
#208 ── #209 ── #210 ── #211 ── #216 ──┬── #217 ── #220
                                      └── #218

#212 ── #213 ──┬── #214
               ├── #215
               └── #219
```

**前沿（现在就能开工）**：#208、#212。

**关键路径**：#208 → #209 → #210 → #211 → #216 → #217 → #220。

**注意**：#208 的结论可能改变记录标识（`record_id` 现为 `<document_id>:annotation:<n>`，HMR 换文档就失配）。若结论要改，**#209 的存储结构必须跟着改**，所以 #209 不能抢跑。
