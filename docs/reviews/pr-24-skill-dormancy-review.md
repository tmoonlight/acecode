# PR #24「Skill dormancy」审核结论

## 基本信息

- PR：[tmoonlight/acecode#24 - Skill dormancy](https://github.com/tmoonlight/acecode/pull/24)
- 作者：`LIUXIN557`
- 审核时 head：`4e3928d00a9585d03ed83480587e9f4c61216d1c`
- 审核日期：2026-08-23
- 结论：**Request changes，当前不应合并**

## 总体结论

这个 PR 的方向有价值：长期不使用的 Skill 不再占用系统提示词，同时保留显式调用和恢复能力。但当前实现尚未形成完整闭环，存在以下合并阻断：

1. Desktop/Web 会话没有接入休眠逻辑，核心功能在主要产品表面实际上不生效。
2. 使用记录只覆盖显式 Skill 注入，模型通过 `skill_view` 正常使用 Skill 时不会记录。
3. 从未显式使用过的 Skill 永远不会休眠，无法解决冷门 Skill 长期占用提示词的问题。
4. 合法但字段类型损坏的状态 JSON 可以让 `record()` 抛异常，违反“损坏状态优雅降级”的接口约定。
5. Web/TUI 管理、pin、唤醒和 `idle_days` 配置入口没有实现完整。
6. UTC 时间戳被按本地时间解析，休眠边界会随系统时区偏移。

此外，该 PR 当前存在合并冲突、严重落后于 `master`、夹带无关提交、没有 CI，并且没有遵循仓库要求的 OpenSpec 流程。

## 阻断性问题

### P1-1：Desktop/Web 的 AgentLoop 没有接入 SkillUsageStore

#### 证据

daemon 创建了 `SkillUsageStore`，并把它交给 `WebServerDeps`，但没有把它放入 `SessionRegistryDeps`：

- [`src/daemon/worker.cpp` L616-L629](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/daemon/worker.cpp#L616-L629)
- [`src/daemon/worker.cpp` L709-L720](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/daemon/worker.cpp#L709-L720)

`SessionRegistry` 创建 Web/Desktop 会话的 `AgentLoop` 时，只设置了 `SkillRegistry`，没有调用 `set_skill_usage_store()` 或 `set_skill_idle_days()`：

- [`src/session/session_registry.cpp` L938-L974](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/session/session_registry.cpp#L938-L974)

而 `AgentLoop::dormant_skill_names()` 在 store 为空时直接返回空集合：

- [`src/agent_loop.cpp` L430-L446](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/agent_loop.cpp#L430-L446)

#### 影响

Desktop/Web 会话中：

- 不会过滤休眠 Skill；
- 不会记录显式 Skill 调用；
- `/api/skills` 最多只能读取一个基本没有数据来源的状态文件；
- TUI 和 Desktop/Web 的行为不一致。

#### 建议

- 在 `SessionRegistryDeps` 中增加共享的 `SkillUsageStore*` 或等价依赖；
- 创建、恢复、专家切换和子代理会话时统一传入 store 与 `idle_days`；
- 添加从 Web 会话提交 Skill 调用、随后验证状态文件和下一次系统提示词的集成测试。

### P1-2：模型通过 skill_view 使用 Skill 时不会刷新使用记录

#### 证据

当前唯一的 `record()` 调用位于显式 Skill 展开之后：

- [`src/agent_loop.cpp` L1758-L1769](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/agent_loop.cpp#L1758-L1769)

系统提示词要求模型对匹配任务调用 `skill_view`，但 `skill_view` 的成功路径没有接入 usage store：

- [`src/tool/skill_view_tool.cpp` L56-L166](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/tool/skill_view_tool.cpp#L56-L166)

PR 自己的设计文档要求覆盖“显式注入、path 激活、模型自动”三类路径，实际实现只覆盖了第一类的一部分。

#### 影响

一个每天都被模型自动选择并通过 `skill_view` 使用的 Skill，仍可能在最后一次显式 `$SkillName` 调用 30 天后被判定为休眠。`useCount` 和 `lastUsedAt` 也不能代表真实使用情况。

#### 建议

- 把“成功激活 Skill”收敛到一个统一记录点；
- 明确定义主 `SKILL.md` 加载和 supporting file 加载的计数口径；
- 覆盖显式调用、`skill_view`、linked path、slash command、子代理和 headless 路径；
- 添加“自动使用会刷新休眠时间”的回归测试。

### P1-3：从未使用过的 Skill 永远不会休眠

#### 证据

没有状态记录时，`is_dormant()` 直接返回 `false`：

- [`src/skills/skill_usage_store.cpp` L79-L88](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/skills/skill_usage_store.cpp#L79-L88)

记录只会在成功调用 Skill 后创建。现有安装升级后，所有 Skill 初始都没有 usage 记录。

#### 影响

真正长期不用、甚至从未用过的冷门 Skill 会永久保留在系统提示词中。只有曾经被显式调用过的 Skill 才有机会在未来休眠，这与该功能“缩减长期闲置 Skill 上下文”的核心目标相反。

#### 建议

- 为发现到的 Skill 记录独立的 `firstSeenAt`，不要把首次发现伪装成一次使用；
- 明确从未使用 Skill 的休眠公式，例如基于 `firstSeenAt` 计算；
- 处理升级迁移、Skill 新增、卸载后重装和同名不同来源等边界。

### P1-4：合法但 schema 损坏的 JSON 会抛异常

#### 证据

`load_state_or_empty()` 只捕获 JSON 语法解析异常。后续代码假定 `skills.<name>` 一定是对象、`useCount` 一定是无符号整数：

- [`src/skills/skill_usage_store.cpp` L20-L47](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/skills/skill_usage_store.cpp#L20-L47)
- [`src/skills/skill_usage_store.cpp` L57-L70](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/skills/skill_usage_store.cpp#L57-L70)

以下内容是语法合法的 JSON，但会让 `record("pdf", ...)` 抛出 `nlohmann::json::type_error`：

```json
{
  "version": 1,
  "skills": {
    "pdf": true
  }
}
```

#### 实测

临时审核测试得到：

```text
[json.exception.type_error.305]
cannot use operator[] with a string argument with boolean
```

这与头文件中“persistence failure never throws”和设计文档中“状态文件损坏时优雅降级”的约定相反。

#### 建议

- 加载后完整验证顶层和每条记录的 schema；
- 对类型错误采用逐条忽略、修复默认值或整体隔离重建策略；
- `record()`、`set_pinned()`、`is_dormant()`、`get_summary()` 的 public 边界应兜住 JSON 异常；
- 增加合法 JSON、错误字段类型、溢出计数和未知字段测试。

## 重要问题

### P2-1：ISO 8601 的 Z 时间戳被按本地时间解析

#### 证据

代码注释称时间是 UTC，但实际使用 `std::mktime()`：

- [`src/skills/skill_usage_store.cpp` L139-L155](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/skills/skill_usage_store.cpp#L139-L155)

`std::mktime()` 把 `std::tm` 当作本地时间。存入状态文件的时间则由 `gmtime` 生成并带 `Z`：

- [`src/session/session_storage.cpp` L659-L678](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/session/session_storage.cpp#L659-L678)

#### 实测

在 Asia/Taipei 时区：

```text
输入：2026-08-01T10:00:00Z
正确：1785578400000
实际：1785549600000
偏差：-28800000 ms，也就是 -8 小时
```

#### 影响

正时区机器会提前判定休眠，负时区机器会延后判定；阈值附近的行为跨平台不一致。

#### 建议

- Windows 使用 `_mkgmtime64`，POSIX 使用 `timegm`，或复用项目中已有的 UTC 时间工具；
- 严格校验 `Z`、尾随字符和可选小数秒；
- 添加固定 epoch 断言，不要只比较“同一错误解析函数的两次结果”。

### P2-2：Web/TUI 管理功能没有形成闭环

#### Web

后端新增了 `useCount`、`lastUsedAt`、`pinned`、`dormant` 字段，但前端 `normalizeSkillList()` 会把这些字段全部丢弃：

- [`web/src/lib/skillsSettings.js` L7-L18](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/web/src/lib/skillsSettings.js#L7-L18)

Skill 卡片只展示名称、来源、描述和启停开关：

- [`web/src/components/SettingsPage.jsx` L1604-L1646](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/web/src/components/SettingsPage.jsx#L1604-L1646)

#### TUI

TUI 只追加一行只读文本，没有 pin、解除 pin或唤醒操作；无记录的 Skill 还会得到空状态和空 last-used 文案：

- [`src/tui/settings/management_center.cpp` L1318-L1349](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/tui/settings/management_center.cpp#L1318-L1349)

#### API 与配置

`SkillUsageStore::set_pinned()` 除单元测试外没有实际调用入口；`skills.idle_days` 只能手工编辑配置文件，没有对应的 Web/TUI 设置项。

#### 建议

- Web 和 TUI 都展示使用次数、最后使用时间和 active/dormant/pinned 状态；
- 增加 pin/unpin 和显式唤醒 API，并接入前端；
- 增加 `idle_days` 设置、范围校验及运行时刷新；
- 无记录 Skill 应显示稳定、明确的状态文案。

### P2-3：跨进程写入可能丢失计数或 pin 状态

`SkillUsageStore` 的 mutex 只保护单个实例。TUI 和 daemon 使用不同进程、不同 store 实例，却写同一个 `~/.acecode/.skill_usage_state.json`。

底层 `atomic_write_file()` 使用固定的同级 `.tmp` 文件。两个进程并发执行“读取旧状态、修改、写临时文件、rename”时，虽然最终 JSON 通常不会半写损坏，但可能发生：

- 后写覆盖先写，丢失 `useCount`；
- pin/unpin 状态被旧快照覆盖；
- 两个进程争用同一个 `.tmp`，其中一次 rename 失败。

建议使用跨进程文件锁、带唯一名的临时文件和锁内 read-modify-write；至少补充双进程并发测试。

### P2-4：每次模型请求会重复读取并解析状态文件 N 次

`AgentLoop::dormant_skill_names()` 遍历全部 Skill，并对每个 Skill 调用一次 `is_dormant()`；而每次 `is_dormant()` 都重新打开和解析完整 JSON 文件：

- [`src/agent_loop.cpp` L430-L446](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/agent_loop.cpp#L430-L446)
- [`src/skills/skill_usage_store.cpp` L74-L90](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/skills/skill_usage_store.cpp#L74-L90)

Skill 数量增加后，这是每次 provider 请求前的同步磁盘 I/O。建议一次读取快照并在内存中完成全部判定，写入成功后更新缓存，必要时通过文件时间检测外部进程更新。

### P2-5：状态文件没有按设计要求使用 0600 权限

设计文档要求状态文件使用 `0600`。底层 helper 已提供 `restrict_permissions` 参数，但本 PR 调用时没有传 `true`：

- [`src/skills/skill_usage_store.cpp` L71](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/skills/skill_usage_store.cpp#L71)
- [`src/skills/skill_usage_store.cpp` L106](https://github.com/tmoonlight/acecode/blob/4e3928d00a9585d03ed83480587e9f4c61216d1c/src/skills/skill_usage_store.cpp#L106)

在 POSIX 默认 umask 下，文件可能对其他本机用户可读。Skill 名称和使用时间可能暴露用户工作习惯，应按设计启用受限权限。

## PR 范围与流程问题

### 当前无法自动合并

GitHub 报告该 PR 为 `CONFLICTING / DIRTY`。本地 `git merge-tree` 确认冲突涉及：

- `src/agent_loop.cpp`：内容冲突；
- `src/main.cpp`：内容冲突；
- `scripts/macos_create_dmg.sh`：`master` 已删除、PR 仍修改。

审核时，该 PR 相对当前 `origin/master`：

- 落后 76 个提交；
- 领先 16 个提交。

### 夹带无关改动

PR 共修改 38 个文件，其中 16 个与 Skill dormancy 无关：

- `docs/tui-comparison/` 下 15 个报告和 Python 演示文件；
- `scripts/macos_create_dmg.sh` 的旧 macOS raster fallback 修复。

建议拆为至少三个独立 PR：

1. Skill dormancy；
2. TUI comparison 文档与演示；
3. macOS DMG 修复。

### 未遵循当前 OpenSpec 流程

这是非平凡行为变更，仓库要求先在 `openspec/changes/` 下创建或继续 OpenSpec change。当前 PR 没有 OpenSpec artifacts，却提交了两个 `docs/superpowers/plans/` 文档；当前仓库已明确禁用 Superpowers 工作流。

建议将有效设计内容迁移为中文 OpenSpec：

- `proposal.md`；
- `design.md`；
- `tasks.md`；
- `specs/skill-dormancy/spec.md`。

### 没有 GitHub CI

`gh pr checks 24` 返回：

```text
no checks reported on the 'skill-dormancy' branch
```

合并前至少应运行 C++ unit tests，以及涉及 Web UI 后的 `pnpm test` 和 `pnpm build`。

## 本地验证结果

所有验证均在独立临时 worktree、PR head `4e3928d` 上完成，没有切换或修改当前脏工作区。验证后临时 worktree 已删除。

### 构建

以下目标构建成功：

```text
cmake --build build-review --target acecode_unit_tests --config Release --parallel
```

这说明 PR 在自己的旧基线上可以编译，但不代表它已经能与当前 `master` 合并。

### PR 原有测试

PR 新增及直接相关的 8 个测试全部通过：

```text
100% tests passed, 0 tests failed out of 8
```

排除临时审核测试后，名称包含 `Skill` 或 `Skills` 的 160 个测试全部通过：

```text
100% tests passed, 0 tests failed out of 160
```

这些测试没有覆盖 daemon SessionRegistry 链路、模型 `skill_view` 使用记录、Web 前端消费、pin 操作、时区精确值或合法但 schema 损坏的 JSON。

### 临时补充测试

审核过程中在临时 worktree 中补了两个测试，两个都失败；它们没有写入当前仓库或 PR。

#### UTC 精确值

```text
Expected: 1785578400000
Actual:   1785549600000
```

#### 合法 JSON 的 schema 损坏

```text
Expected: record() does not throw
Actual: nlohmann::json::type_error.305
```

## 建议的修订顺序

1. 从当前 `master` 创建干净分支，只保留 Skill dormancy 改动。
2. 按仓库规范建立中文 OpenSpec，并明确 never-used、active、dormant、pinned、disabled 的状态语义。
3. 设计统一的 Skill 成功激活记录点，覆盖全部调用表面。
4. 将 store 和配置完整传入 TUI、Desktop/Web、headless、子代理的所有 `AgentLoop`。
5. 增加 `firstSeenAt` 或等价基线，解决从未使用 Skill 永不休眠的问题。
6. 修正 UTC 解析、schema 容错、跨进程并发和文件权限。
7. 完成 Web/TUI 展示、pin/unpin、唤醒和 `idle_days` 设置。
8. 增加端到端测试并启用 GitHub CI。

## 最终审核意见

**Request changes。**

当前 PR 的基础方向可以保留，但实现尚未满足其设计文档描述的核心行为，且 Desktop/Web 主链路不生效。应先拆分无关提交、同步当前 `master`、补齐 OpenSpec 和上述 P1 问题，再进行下一轮审核。
