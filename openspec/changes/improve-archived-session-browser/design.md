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
- [Archives with many sessions remain fully loaded] -> This preserves the current loading contract; memoize local transformations without adding server pagination.

## Migration Plan

No data migration is needed. Ship the frontend bundle through the normal build. Reverting these frontend changes restores the previous archive presentation without altering stored sessions.
