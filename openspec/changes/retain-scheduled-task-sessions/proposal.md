## Why

重复运行的定时任务会积累执行会话。用户需要为每个任务单独选择如何处理较早的通知。

## What Changes

- 在任务编辑器、创建工具和内置技能中支持自动归档、自动删除、保留数量。
- 将选择保存在任务定义中，在下一次定时或手动运行时执行清理。

## Capabilities

### New Capabilities
- `scheduled-task-session-retention`：按任务清理执行会话。

## Impact

涉及 LOOP 数据库迁移、调度器、daemon API、Web 任务编辑器和内置 scheduled-task 技能。
