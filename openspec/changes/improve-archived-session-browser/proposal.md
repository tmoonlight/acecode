## Why

The archive settings page is a flat list with its batch actions below every session, making long archives difficult to navigate and manage. The requested layout puts search, sorting, workspace filtering, and batch actions above sessions grouped by workspace.

## What Changes

- Add a search field matching session titles, workspace names, and paths, plus newest/oldest update-time sorting and a workspace selector.
- Group results by workspace identity, display each workspace name and matching session count, and keep sessions without a workspace in a separate group.
- 工作区分组和筛选选项沿用侧边栏工作区顺序；最近／最早更新时间只决定各工作区内部的会话顺序。
- Move select all, unarchive selected sessions, and delete selected sessions into the upper-right header; restrict selection and batch operations to current filtered results.
- Retain single-session actions, localized relative timestamps, shared deletion confirmation, and partial-failure retry behavior.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `session-archive`: Add searchable, sortable workspace groups and header batch controls to the archive settings view.

## Impact

Frontend-only changes to `web/src/components/SettingsPage.jsx`, the archived-session helpers and tests, and localization overrides/catalog. Existing daemon APIs and dependencies remain sufficient. Verification covers filtering, group identity, selection boundaries, batch failures, responsive layout, and the complete web test/build gates.
