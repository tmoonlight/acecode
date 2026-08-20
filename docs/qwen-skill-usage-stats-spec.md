# Skill 使用次数统计 — Qwen Code 源码调研与需求规格说明

> 基于 Qwen Code `main` 分支源码逐文件研读整理。目标:为 ACECode 实现"skill 使用次数统计"功能提供可落地的需求规格与参考实现(数据模型、记录链路、生命周期、展示面、边界约束均以 Qwen Code 实际实现为准)。
>
> 调研对象(源码文件):
> - `packages/core/src/tools/skill.ts` — Skill 工具调用与记录触发点
> - `packages/core/src/tools/skill-utils.ts` — 可用 skill 收集/过滤
> - `packages/core/src/telemetry/uiTelemetry.ts` — 会话级统计聚合 `UiTelemetryService`
> - `packages/core/src/telemetry/loggers.ts` — `recordSkillInvocation` / `logSkillLaunch`
> - `packages/core/src/skills/skill-curator.ts` — 项目级 auto-skill 生命周期与 `useCount`
> - `packages/core/src/skills/skill-paths.ts` — 目录约定
> - `packages/core/src/services/usageHistoryService.ts` — 持久化 + 聚合
> - `packages/core/src/services/usage-dashboard-service.ts` — Dashboard 负载
> - `packages/cli/src/serve/routes/usage-stats.ts` — daemon API
> - `packages/cli/src/ui/commands/statsCommand.ts`、`skillsCommand.ts`、`curator-command.ts` — TUI 命令
> - `packages/cli/src/ui/components/SkillStatsDisplay.tsx` — TUI 表格组件
> - `packages/web-shell/client/components/dialogs/UsageDashboardTab.tsx` — Web Shell 展示

---

## 1. 背景与目标

Qwen Code 是目前外部编码 agent 中唯一把"每个 skill 被使用过多少次"做成用户可查统计的 agent。其实现分**两条相互独立、互为补充**的链路:

1. **会话/全局实时统计**(`UiTelemetryService` + `usage_record.jsonl`):对**所有类型** skill 的每次调用按名称计数(成功/失败),支持 TUI `/stats skills`、Web Shell daemon 的 Usage Dashboard 按 today/7D/30D 展示。
2. **项目级 auto-skill 生命周期统计**(`skill-curator.json` + `useCount`):仅针对项目内自动生成的 `auto-skill-*` 目录,记录使用次数与时间戳,用于"长期不用则归档"的维护策略(`/curator`)。

本文档目标是让 ACECode 能够据此设计等价能力。

---

## 2. 术语

| 术语 | 含义 |
|------|------|
| Skill 调用 | 模型或用户通过 Skill 工具(/skill 或 `/<name>`)实际加载执行一次 skill 的完整动作 |
| `byName` | 按 skill 名称聚合的统计桶 |
| `useCount` | auto-skill curator 状态里某个 skill 的累计成功使用次数 |
| `lastActivityAt` / `lastUsedAt` | 最近一次活动/成功使用时间戳(ISO8601) |
| `state` | auto-skill 生命周期状态:`active` / `stale` / `archived` |
| 活跃度(inactivity) | 当前时刻与"最近活动时间"的差值,驱动 stale/archive 判定 |

---

## 3. 能力范围(需求清单)

### 3.1 统计记录(核心)

- **R1 每次成功调用计数**:skill 加载成功后,`byName[name].count++`、`success++`,全局 `totalCalls++`、`totalSuccess++`。
- **R2 每次失败调用计数**:skill 未找到、被禁用、加载抛异常时,`byName[name].count++`、`fail++`(仅当未回退到同名命令时)。
- **R3 重复调用计数**:同一 skill 已加载后再次调用(上下文去重分支)仍会计数(视为一次"使用")。
- **R4 命令回退不计数**:当目标名实际是 MCP prompt / 文件命令(非真实 skill)时,委托执行**不**计入 skill 统计(避免污染)。
- **R5 失败态治理**:`skill "X" is disabled` 这种"可预测失败"单独走禁用分支,同样记录为失败。

### 3.2 项目级 auto-skill 生命周期(补充)

- **R6 仅管理受管目录**:目录名 `auto-skill-<name>` 且 frontmatter `source: auto-skill` 且为 project 级。
- **R7 useCount 持久化**:每次成功调用 `useCount += 1`,同时刷新 `lastUsedAt`/`lastActivityAt`,重置 `state=active`、清除 `archivedAt`。
- **R8 时间窗口**:30 天无活动 → `stale`;90 天无活动 → 移入 `.qwen/archived-skills/` 归档;活动恢复 → `reactivated` 回 `active`。
- **R9 间隔约束**:自动维护每 7 天最多一次(`maybeRunAutoSkillCurator` 幂等)。
- **R10 pin 豁免**:pinned 的 skill 不参与自动 stale/archive。

### 3.3 展示与聚合

- **R11 TUI `/stats skills`**:非交互输出文本清单;交互模式打开 `SkillStatsDisplay` 表格(名称/调用数/OK/失败/成功率)。
- **R12 Web Shell Usage Dashboard**:`GET /usage/dashboard` 返回 `skills: [{name, count}]`,按 count 降序;Today/7D/30D 切换。
- **R13 聚合上限**:单范围 topSkills 截断到 25 条(与 topTools 截断到 10 条对称,保证 payload 有界)。

---

## 4. 数据模型

### 4.1 会话级统计(`SessionMetrics.skills`)

```ts
interface SkillCallStats { count: number; success: number; fail: number; }
interface SkillMetrics {
  totalCalls: number;
  totalSuccess: number;
  totalFail: number;
  byName: Record<string, SkillCallStats>;  // 键为 skill 名,原型安全(hasOwnProperty 校验)
}
```

- 属于 `SessionMetrics` 的可选字段(`skills?: SkillMetrics`),与 `models`/`tools`/`files` 并列。
- `UiTelemetryService` 维护**全局聚合** `#metrics` 与**按 sessionId 分桶** `#sessionMetrics` 两套,`recordSkillInvocation(skillName, success, sessionId?)` 同时写入两者。

### 4.2 持久化记录(`usage_record.jsonl`)

每条为一行 JSON(`UsageSummaryRecord`, `version: 1`):

```ts
interface UsageSummaryRecord {
  version: 1;
  sessionId: string;
  timestamp: number;      // 会话结束时刻
  startTime: number;
  project: string;
  durationMs: number;
  models: Record<string, {...}>;   // token 明细
  tools: { totalCalls; totalSuccess; totalFail; byName: {...} };
  files: { linesAdded; linesRemoved };
  skills?: {              // 可选:旧记录无此字段
    totalCalls: number; totalSuccess: number; totalFail: number;
    byName: Record<string, { count; success; fail }>;
  };
}
```

- 落盘位置:`~/.qwen/usage_record.jsonl`(全局跨项目)。
- 写入时机:TUI `/clear` 或会话正常退出时 `persistSessionUsage`;transcript 删除前 `persistUsageBeforeTranscriptDeletion` 做兜底 salvage;历史 transcript 可重建 `rebuildFromSessionJsonl`。

### 4.3 项目级 curator 状态(`.qwen/skill-curator.json`)

```ts
interface AutoSkillRecord {
  skillName: string;
  firstSeenAt: string;      // 首次观测
  lastActivityAt: string;   // 最近活动(含 manifest 修改)
  lastUsedAt?: string;      // 最近成功使用
  useCount: number;         // 累计成功使用次数
  state: 'active' | 'stale' | 'archived';
  pinned: boolean;
  archivedAt?: string;
}
interface AutoSkillCuratorState {
  version: 1;
  lastRunAt?: string;
  skills: Record<string, AutoSkillRecord>;  // 键为目录名(非 skill 名)
}
```

- 写路径:`readState`(lstat + O_NOFOLLOW + 大小上限)+ `atomicWriteJSON(mode 0600, noFollow)` + proper-lockfile 互斥(`skill-curator.lock`)。

---

## 5. 记录链路(时序)

### 5.1 模型/用户调用 → 计数

```
SkillToolInvocation.execute()
  ├─ disabled 分支
  │    ├─ 尝试同名命令回退(commandExecutor)成功 → return(不计数)
  │    └─ 失败/无命令 → logSkillLaunch(false) + recordSkillInvocation(success=false) + 返回禁用提示
  ├─ loadSkillForRuntime(name) → null
  │    ├─ 尝试命令回退 → 成功 return(不计数);失败往下
  │    └─ recordSkillInvocation(success=false)(无回退时)+ logSkillLaunch(false)
  ├─ loadSkillForRuntime(name) → skill
  │    ├─ logSkillLaunch(true)
  │    ├─ 已加载?→ recordAutoSkillUsageBestEffort(skill) + return(仍计数——见 R3)
  │    ├─ onSkillLoaded(name)
  │    ├─ applySkillAllowedTools(...)   // 授权 allowedTools
  │    ├─ registerSkillHooks(...)       // 注册 hooks
  │    └─ recordAutoSkillUsageBestEffort(skill) + recordSkillInvocation(success=true)
  └─ catch → recordSkillInvocation(success=false)(无回退时)+ logSkillLaunch(false)
```

**关键点**:
- `recordSkillInvocation` 只更新 UI telemetry(`UiTelemetryService`);`logSkillLaunch` 只发 OTLP span/log(`SkillLaunchEvent{skill_name, success, prompt_id}`)+ QwenLogger,两者解耦。
- `recordAutoSkillUsageBestEffort` 只对 project 级受管 skill 生效(内部 `recordAutoSkillUsage` 校验 `level==='project'` 且目录在 skillsRoot 下)。
- 已加载 skill 重复调用时,`recordAutoSkillUsageBestEffort` 依然执行(useCount 仍 +1),符合 R3。

### 5.2 会话结束 → 持久化

```
Session 结束 / /clear
  └─ persistSessionUsage({sessionId, startTime, endTime, project, metrics})
       └─ metricsToUsageRecord()   // SessionMetrics.skills → UsageSummaryRecord.skills
            └─ jsonl.writeLineSync(~/.qwen/usage_record.jsonl)

Transcript 删除前
  └─ persistUsageBeforeTranscriptDeletion(transcriptPath)
       └─ summarizeTranscript() → 已存在同 session 记录则跳过(幂等)
```

### 5.3 查询 → 聚合 → 展示

```
TUI:  /stats skills
  └─ context.session.stats.metrics.skills ?? EMPTY_SKILL_METRICS
       ├─ 非交互: formatSkillStats() 纯文本(总调用 + 逐 skill count/success/fail)
       └─ 交互:   MessageType.SKILL_STATS → SkillStatsDisplay 表格

Daemon: GET /usage/dashboard?range=&heatmapDays=
  └─ loadUsageHistoryWithLive()   // 持久文件 ∪ 最近 35 天 transcript 重建
       └─ buildUsageDashboard(records, {range, heatmapDays})
            └─ aggregateUsage(records, range)
                 └─ skills.topSkills 按 count 降序,截断 25
                      └─ UsageSkillCall[] {name, count}
                           └─ Web Shell UsageDashboardTab 表格(name + count)
```

---

## 6. 生命周期维护(auto-skill curator)

### 6.1 判定公式

```
inactivityMs = nowMs - lastActivityMs(skill, record, nowMs)
其中 lastActivityMs = max(manifest mtime, firstSeenAt, lastActivityAt, lastUsedAt)
```

- `inactivityMs >= 90d` → archive(目录 rename 到 `.qwen/archived-skills/`)
- `inactivityMs >= 30d` → state=stale(仅标记)
- `inactivityMs < 30d` 且 state≠active → reactivated
- `record.pinned` → 跳过所有维护
- manifest 近期被编辑(mtime 更新)视为活动 → 抑制归档(测试用例证实)

### 6.2 触发时机

- 显式:`/curator run [--dry-run]`、`/curator status`、`/curator pin|unpin|restore`
- 自动:每次启动 `maybeRunAutoSkillCurator`,`lastRunAt` 距今 < 7 天则 `not_due` 快速返回
- 状态清理:目录在 live 与 archive 均不存在时,记录被 prune,防止状态文件无限增长

### 6.3 命令面(`/curator`)

| 子命令 | 行为 |
|--------|------|
| `status`(默认) | 显示 lastRun、active/stale/archived 计数与清单、pinned 清单 |
| `run [--dry-run]` | 执行/预览一次维护(checked/seeded/stale/archived/collision/error 统计) |
| `pin` / `unpin <dir>` | 设置豁免 |
| `restore <dir>` | 从归档移回活动区(校验命名、防覆盖、失败回滚) |

安全约束:safe mode / 非 trusted workspace 下,变更类操作(`run` 非 dry-run、pin、restore)被拒绝;状态查看与 dry-run 允许。

---

## 7. 边界与约束(实现时必读)

1. **统计口径一致**:`SkillTool` 描述是**静态**的,可用 skill 列表走 `<available_skills>` system-reminder 注入;计数以**实际加载**为准,不受列表静态化影响。
2. **命名归一**:禁用判断用 `name.toLowerCase()`;`byName` 键用**原样 skill 名**(不归一),统计与 UI 一致展示原名。
3. **原型安全**:`byName` 用 `Object.create(null)` 或 `hasOwnProperty` 守卫,防止名为 `constructor` 等的 skill 破坏桶。
4. **旧数据兼容**:`skills` 字段可选(旧记录缺失),聚合侧 `if (r.skills)` 跳过。
5. **有界性**:topSkills 截断 25;daily 序列上限 92 天;heatmap 183 天(UI 用 12 个月≈365 天);状态文件读上限 1MB、manifest 上限 4MB。
6. **并发与安全**:curator 用 proper-lockfile(8 次重试/随机退避/10s stale);所有路径 O_NOFOLLOW + lstat 防符号链接穿越;写入 0600。
7. **幂等/去重**:持久化按 sessionId last-wins 去重(#4994);transcript salvage 检测已存在记录则跳过;同 session 不重复写。
8. **命令 vs skill 区分**:同名 MCP prompt / 文件命令优先于禁用 skill 执行,且不计数——这是统计不被污染的硬约束。
9. **两套统计互不干扰**:UI telemetry(面向用户展示)与 OTLP telemetry(面向观测)分离;`useCount`(curator)与 `byName.count`(dashboard)是两套独立数字,不要合并。

---

## 8. 展示面细节

### 8.1 TUI `/stats skills`(SkillStatsDisplay)

| 列 | 说明 |
|----|------|
| Skill Name | 左对齐,链接色,宽 30 |
| Calls | 右对齐,8 列 |
| OK | 右对齐,成功色,8 列 |
| Fail | 右对齐,失败色(>0 时),8 列 |
| Success Rate | `success/count*100` 保留 1 位,按阈值着色(高/中/低) |

空态:`No skill calls have been made in this session.`;标题 `Skill Stats For Nerds`;按 count 降序、同名按字母序。

### 8.2 Web Shell Usage Dashboard(`/usage/dashboard`)

- 表头:range word + "SKILL CALLS";列为 Skill Name / Count(右对齐)。
- 排序:按 count 降序(由 `aggregateUsage` 的 topSkills 决定)。
- 空态:skills 数组为空时整段隐藏。
- 缓存:daemon 侧 60s TTL、range 无关、并发请求合并到同一 in-flight load。

---

## 9. ACECode 落地映射建议

| ACECode 现状 | 缺口 | 建议(对齐 Qwen) |
|--------------|------|------------------|
| `src/tui_state.hpp` 只有 `slash_command_usage_counts`(slash 命令次数,用于排序) | 无 skill 级统计 | 新增 `SkillUsageStats{totalCalls,totalSuccess,totalFail, byName{count,success,fail}}` 挂在会话状态;slash 命令次数与其并存不混用 |
| `src/skills/skill_activation.cpp` 负责激活判定 | 加载点无计数钩子 | 在 skill 成功注入上下文处 `recordSkillUsage(success=true)`;解析/禁用/异常处 `success=false`;复用 `ToolErrors`/`ToolArgsParser` 等既有 helper |
| 无 per-session 持久化 usage | 无历史聚合 | 仿 `usage_record.jsonl`:会话结束/清理时追加 JSONL,按 sessionId 去重;聚合按 today/week/month/all 过滤并取 topN |
| TUI 无 skill 统计命令 | 无展示入口 | 在 `/stats` 或独立 `/skills-stats` 增加 skill 调用统计(文本 + 可选表格);遵循仓库 ASCII/无 emoji 规范 |
| Web/daemon(`src/daemon`、`src/web`) | 无 dashboard API | 仿 `GET /usage/dashboard` 只读路由,返回 `{skills:[{name,count}]}`,带短 TTL 缓存 |
| 无"长期未用归档"能力 | 可选增强 | 若后续做 auto-skill 维护,按 `skill-curator.json` 模型实现 `{firstSeenAt,lastActivityAt,lastUsedAt,useCount,state,pinned,archivedAt}`,阈值 30/90 天、间隔 7 天 |

> 注:ACECode 若只需"展示使用次数"这一核心需求,落地最小集为 **R1-R5 + R11(或 R12 之一)+ 第 7 节约束**;auto-skill 生命周期(R6-R10、第 6 节)为可选增强。

---

## 10. 验证与参考

### 10.1 关键测试(源码中已存在,作为行为契约)

- `skill-curator.test.ts`:
  - `protects recently used skills and increments durable usage` — 使用后归档抑制 + `useCount: 1`
  - `treats a recent manifest edit as activity` — mtime 更新抑制归档
  - `reactivates a stale skill once activity resumes`
  - `preserves an existing usage baseline when seeding the first run`
  - `ignores non-project usage records` / `ignores project usage records outside the skills root`
  - `refuses to restore over an existing active directory`、`rejects archive directory traversal during restore`
  - `fails closed on corrupt state` / `oversized state file` / `non-regular-file state file`
- `usage-dashboard-service.test.ts`:`ranks per-model share and aggregates skills for the range`(跨 session 聚合 + count 降序)

### 10.2 关键常量

| 常量 | 值 |
|------|----|
| `AUTO_SKILL_CURATOR_INTERVAL_MS` | 7 天 |
| `AUTO_SKILL_STALE_AFTER_MS` | 30 天 |
| `AUTO_SKILL_ARCHIVE_AFTER_MS` | 90 天 |
| `CURATOR_STATE_FILE` / `CURATOR_LOCK_FILE` | `skill-curator.json` / `skill-curator.lock`(位于 `.qwen/`) |
| `LIVE_REBUILD_WINDOW_DAYS` | 35 天 |
| `DEFAULT_HEATMAP_DAYS` | 183(路由 1..366) |
| `MAX_DAILY_DAYS` | 92 |
| topSkills / topTools 截断 | 25 / 10 |
| `MAX_STATE_FILE_BYTES` / `MAX_MANIFEST_BYTES` | 1MB / 4MB |
