## ADDED Requirements

### Requirement: 技能时间戳必须按严格 UTC 解释
技能状态解析器 MUST 只接受 `YYYY-MM-DDTHH:MM:SS[.fff]Z` 形式的有效公历时间，并按 UTC 转换；1 到 3 位小数 MUST 按十进制秒比例转换为毫秒。

#### Scenario: UTC 结果不受本地时区影响
- **WHEN** 解析 `2026-08-01T10:00:00Z`
- **THEN** 结果固定为 Unix epoch `1785578400000` 毫秒

#### Scenario: 小数秒按位数缩放
- **WHEN** 分别解析 `.1Z`、`.12Z` 和 `.123Z`
- **THEN** 毫秒部分分别为 100、120 和 123

#### Scenario: 非法日期被拒绝
- **WHEN** 月、日、时、分、秒越界或时间戳含尾随字符
- **THEN** 解析失败且不会把标准库归一化后的时间当作有效数据

### Requirement: 损坏 schema 不得使公开状态操作抛出
读取和更新技能状态时，系统 SHALL 对错误类型或非对象 entry 使用安全默认值，并将被更新的 entry 正规化为当前对象 schema。

#### Scenario: 字段类型损坏
- **WHEN** `useCount`、`pinned` 或 `lastUsedAt` 的 JSON 类型与 schema 不符
- **THEN** summary 和 dormant 查询返回安全默认值，record/set-pinned 不抛异常

#### Scenario: 技能 entry 不是对象
- **WHEN** 某技能 entry 为字符串、数组或 null
- **THEN** 下一次 mutation 将该 entry 替换为有效对象并保存请求的更新

#### Scenario: 计数达到上限
- **WHEN** `useCount` 已为无符号 64 位最大值且再次 record
- **THEN** 计数保持最大值而不回绕

### Requirement: 多实例 mutation 必须保留所有更新
技能状态的 mutation MUST 通过同一 sidecar lock 在进程间串行，并在持锁后重新读取最新快照，再以原子文件替换提交。

#### Scenario: 两个实例并发记录同一技能
- **WHEN** 两个 store 实例并发各执行一次 `record`
- **THEN** 最终 `useCount` 增加 2 且状态文件保持有效 JSON

#### Scenario: 写入进程异常退出
- **WHEN** 持锁进程退出并留下 lock 文件路径
- **THEN** OS 释放锁，后续进程仍可获取锁并更新状态
