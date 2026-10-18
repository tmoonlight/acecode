## Context

`SectionArchived` loads a merged archive through `api.listAllArchivedSessions`, already including workspace hashes, names, paths, and no-workspace flags. `archivedSessions.js` provides workspace-qualified selection and removal helpers. Restore/delete operations already settle independently, preserve failures, notify the sidebar, and use the shared `Modal` for deletion. See `proposal.md` for the requested navigation improvements.

## Goals / Non-Goals

**Goals:** Keep one source of archive data and derive grouped, filtered results with testable pure helpers. Preserve endpoint routing, existing relative-time labels, and partial-failure behavior.

**Non-Goals:** Changes to daemon APIs, full-text transcript search, session persistence, or the surrounding settings navigation.

## Decisions

- Derive workspace identity, search, sorting, and groups in `archivedSessions.js`. Use workspace hashes rather than names; separate no-workspace sessions and legacy local sessions. Shared display-title logic ensures search matches the row users see. Client-side filtering uses the already loaded archive and avoids additional requests.
- 默认在各工作区内部按最近更新时间排序，可切换最早更新；缺少有效更新时间时回退到创建时间，无有效时间的会话始终放在组内末尾。工作区分组与筛选选项复用 `listAllArchivedSessions()` 返回的 `workspaces` 顺序，和侧边栏的 `/api/workspaces` 顺序一致，不受会话时间及搜索结果影响。筛选选项仍从完整归档列表生成，搜索不隐藏选项。
- `groupArchivedSessions` 接收工作区标识顺序，先分组再分别排序组内会话。无工作区分组固定在前，与侧边栏任务区的位置一致；没有出现在工作区列表中的历史分组放在已知工作区之后，并按标识稳定排序。保留本地兼容回退、空组隐藏、同名工作区隔离以及批量操作行为，不新增请求或修改接口。
- Derive batch targets from current visible results and prune selections hidden by changed filters. Keep matching selections through sorting. This avoids hidden destructive targets and stale selection on clearing a query.
- Place the title/description and three batch buttons in a wrapping header, followed by a search field and two selects. Group sections show folder icons, names, counts, and divided session rows with the current per-row actions. Use settings theme tokens and existing icons, with no global stylesheet changes. Same-name options include a path or hash and group headers expose their path.
- Retain existing batch and modal logic. Updating the list automatically updates counts and removes empty groups; the selected workspace remains available until the user changes it, even if its last session was removed.
- Browser verification exposed the existing archive confirmation below the settings overlay (`z-[200]` versus `z-[300]`). Give this confirmation the existing settings-dialog layer `z-[310]` so mouse confirmation/cancellation remains available; keep the shared Modal default unchanged.

## Risks / Trade-offs

- [Filtering could accidentally retain hidden destructive targets] -> Use visible results for selected targets and test selection pruning, no matches, and combined filters.
- [Shared settings CSS removes generic card borders] -> Use semantic list elements for group frames and marked composite controls for filter borders; inspect actual desktop, narrow, and dark renderings.
- [New Chinese labels can fail localization gates] -> Add English overrides, regenerate the catalog, and review generated changes.
- [归档很多时一次性挂载导致卡顿] -> 完整元数据仍用于搜索、排序、工作区选项和总数；DOM 按 100 条增量挂载，不新增服务端分页协议。

## Migration Plan

No data migration is needed. Ship the frontend bundle through the normal build. Reverting these frontend changes restores the previous archive presentation without altering stored sessions.

## 2026-10-18 归档列表分批显示

- 在完整过滤、分组和排序之后，按累计行数切出首批 100 条，跨工作区共用同一个额度；组标题保留完整匹配数量，未加载的空组不挂载。
- 在设置内容滚动容器内观察列表末尾，接近底部追加 100 条；观察器不可用时用滚动/尺寸事件兜底。每次观察周期只允许追加一批，卸载时清理观察器/事件，尾批加载完移除哨兵。
- 搜索、工作区或排序变化时重置首批并回到列表顶部，避免旧分页额度导致短暂全量渲染；选择状态只保留新显示范围内的会话。追加批次不自动勾选新行。
- 全选及批量操作仅作用于已显示的行；取消归档/删除后保持当前加载额度并补齐缺口，外部撤销触发的数据刷新沿用已有生命周期保护。
- 初始接口仍获取完整元数据，网络/磁盘全量扫描延迟不是本次前端分批显示解决的范围；无工作区请求增加已有的 `scope=no-workspace`，避免读取随后会被前端丢弃的兼容工作区元数据。
- 用 100/101 边界、跨组 205 条、超过 2,000 条的实际组件浏览器夹具验证挂载数、滚动、搜索、排序、选择及刷新；比较修改前后的 DOM 数量。
