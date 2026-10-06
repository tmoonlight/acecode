## ADDED Requirements

### Requirement: Archive browser filters and sorts workspace groups
The archive settings view SHALL provide search, update-time sorting, and workspace selection above a list grouped by workspace identity. Each nonempty group SHALL show its workspace name and matching session count. Sessions without a workspace SHALL have a separate group.

#### Scenario: Browse and sort groups
- **WHEN** 用户打开已归档会话
- **THEN** 工作区分组和工作区筛选选项 MUST 按侧边栏工作区顺序排列
- **AND** 每个工作区内部的会话 MUST 默认按最近更新时间排列，缺失时回退到创建时间
- **AND** 切换最早更新 MUST 只反转组内时间顺序，保持工作区分组顺序不变
- **AND** 没有有效时间的会话 MUST 放在各自组内有时间的会话之后

#### Scenario: Keep workspace order through filtering and archive changes
- **WHEN** 用户搜索、切换工作区过滤条件，或取消归档／删除会话
- **THEN** 剩余工作区分组 MUST 保持侧边栏中的相对顺序
- **AND** 某个工作区最新或最早的会话变化 MUST NOT 改变工作区分组顺序
- **AND** 没有匹配会话的工作区 MUST NOT 显示为空分组

#### Scenario: Keep special and unlisted groups deterministic
- **WHEN** 归档包含无工作区会话、本地兼容会话或未出现在工作区列表中的历史会话
- **THEN** 无工作区分组 MUST 固定放在工作区之前，与侧边栏任务区位置一致
- **AND** 未列出的工作区分组 MUST 放在已知工作区之后并按工作区标识稳定排序
- **AND** 工作区顺序缺失时分组 MUST 仍不受组内会话时间排序方向影响

#### Scenario: Combine search and workspace filtering
- **WHEN** the user enters a search query and selects a workspace
- **THEN** only that workspace's sessions matching the displayed title, workspace name, or path MUST be shown
- **AND** search MUST ignore surrounding whitespace and letter case
- **AND** group counts MUST reflect the matching results

#### Scenario: Distinguish workspaces with the same name
- **WHEN** two workspaces have the same display name
- **THEN** they MUST remain independently filterable and grouped by identity
- **AND** their paths or identifiers MUST be available to disambiguate them

#### Scenario: Empty filtered results
- **WHEN** search and workspace filters match no sessions
- **THEN** the view MUST show a no-results message while keeping the filters accessible
- **AND** no empty workspace groups MUST be rendered

### Requirement: Archive batch actions appear in the header
The archive view SHALL show select all/deselect all, unarchive selected sessions, and delete selected sessions in that order at the upper right above the filters and list. Controls SHALL wrap within narrow views without horizontal overflow.

#### Scenario: Select only current results
- **WHEN** the user selects all after applying filters
- **THEN** only visible matching sessions MUST be selected
- **AND** batch restore and deletion MUST act only on the current selection
- **AND** changing filters MUST remove selections for sessions no longer visible
- **AND** sorting MUST preserve selected sessions

#### Scenario: No available selection
- **WHEN** no current results are selected or an operation is running
- **THEN** unavailable batch actions MUST be disabled

#### Scenario: Partial operation failure
- **WHEN** a batch restore or delete succeeds for some sessions and fails for others
- **THEN** successful sessions MUST disappear and failed sessions MUST remain selected for retry
- **AND** remaining groups and counts MUST update to match the list

#### Scenario: Confirm deletion
- **WHEN** the user deletes one session or the selected sessions
- **THEN** the existing shared confirmation dialog MUST appear before permanent deletion
- **AND** cancelling MUST leave the archive unchanged
