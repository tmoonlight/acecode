# 开发环境启动器多实例优化需求规格

**日期：** 2026-09-20
**状态：** 需求与技术方案均已确认（§7 决策已落定）；后续按波次分别走 OpenSpec 提议
**来源：** 多实例特性提交 `42912f75`（`plan_instance_startup` / `desktop-instances` 模型）落地后的开发启动器排查与交接记录（分支 `fix/dev-launcher-runtime-identity` 上的实测复现）

## 1. 目标

多实例（主仓库 + 多个 worktree 并行开发）成为常态后，`dev_environment.py` 系启动脚本的三个旧假设失效：28080 端口基本空闲、run-dir 天然可复用、失败可人工排查。本规格让 web / desktop 两条开发启动链路做到：

- **快**：端口决策秒级完成，不再为被占的 28080 白等 15 秒。
- **稳**：陈旧运行目录自愈，重启即可恢复，不需要手动 `daemon stop`。
- **可诊断**：启动失败时能在 run-dir 内直接读到真实退出原因。
- **吃到多实例红利**：desktop 开发场景获得稳定实例身份，同一 worktree 同一 commit 重启复用同一 daemon，不再每次产生新的随机实例目录。

## 2. 范围

### 本轮范围（A + B + C + D + E + G）

| 项 | 一句话 | 主要落点 |
|---|---|---|
| A 陈旧自愈 | 死 pid 的 run-dir 自动清理并重试启动 | `scripts/dev_environment.py` |
| B 固定端口 | 端口由 worktree 身份确定性派生，被占立即换 | `scripts/dev_environment.py` |
| C 失败可诊断 | daemon worker 输出落盘到 run-dir 内日志 | `scripts/dev_environment.py` |
| D 死目录清理 | 启动自动清 + 手动全量清理命令 | `scripts/dev_environment.py` |
| E 实例身份 | desktop 接受外部指定实例身份，dev 脚本注入稳定身份 | `src/apps/desktop/main.cpp`、`scripts/dev_desktop.py` |
| G 进程级多开 | desktop 支持进程级"允许本实例多开"覆盖，不污染全局配置 | `src/apps/desktop/main.cpp`、`scripts/dev_desktop.py` |

每项均需同步更新 `.agents/skills/development-environment/SKILL.md` 与对应 Python 单测（`tests/scripts/`，unittest 风格）。

### 不在本轮范围

- **F** run-dir 统一到 `~/.acecode/run/`（脚本与产品两套运行时目录世界的合并，涉及 `daemon_pool` 复用判定，另立议题）。
- **H** `token.tmp` 回退读取移除（3 行小改，建议后续与 `atomic_file.hpp` 修复合并处理）。

## 3. 需求明细

### 3.1 A：陈旧运行目录自愈

启动时 run-dir 健康检查未通过（现状：直接硬失败），改为按 pid 死活分支：

| 检查结果 | 行为 |
|---|---|
| run-dir 健康（pid / port / token 与 `/api/health` 身份匹配） | 现状保持：复用 daemon |
| 不健康，且记录的 pid **已死** | 自动清理该 run-dir 的运行时文件，**重试一次**启动；重试仍失败则报错并指向 C 的日志文件 |
| 不健康，且记录的 pid **活着**（真 daemon，只是身份不匹配） | 不干预、不终止，保留现有"请手动停止"类提示 |

**安全红线：任何自动清理路径都绝不对存活进程发出终止。**

### 3.2 B：worktree 派生固定端口

- 默认端口不再固定 28080，改由 worktree 身份（目录名 + commit）**确定性派生**，落在 28080–28280 区间内。
- 同一 worktree 每次启动得到同一端口；不同 worktree 天然错开，互不抢占。
- 派生端口被占时**立即**换下一个候选，不等待健康检查超时（消灭 15 秒白等）。
- 日常通过 Vite 代理访问不受影响；直接访问 daemon 端口时以启动输出为准。
- 若用户显式指定端口，显式值优先于派生值。

### 3.3 C：失败可诊断

- 启动器拉起的 daemon worker 的输出（stdout/stderr）落盘到 run-dir 内的日志文件（目录已被 `.gitignore` 覆盖，不污染仓库）。
- 启动失败时，错误输出明确指向该日志文件路径。
- 目标：定位一次失败不再依赖 heartbeat + netstat + `daemon status` 反推。

### 3.4 D：死目录清理

- **启动时自动清理**：每次启动顺带清理「目录名匹配当前 worktree 前缀 **且** pid 已死」的 run-dir，日常零堆积。
- **手动全量清理命令**：一次性清理所有可判定为死（pid 已死）的 run-dir，包括：已删除 worktree 的遗留、现存存量（约 20 个目录 / 19 个死 pid）、以及旧版本遗留的 `.acecode-dev-run/` 目录。
- 清理动作只作用于「pid 已死」可判定的目录；判定不了的（缺 pid 文件等）跳过并报告，不猜。

### 3.5 E：desktop 实例身份稳定化

- desktop 新增**外部指定实例身份**的覆盖入口（环境变量或 CLI，形态见开放决策）；不指定时行为与现状完全一致（随机 uuid）。
- dev 启动路径（`dev_desktop.py` / `dev_environment.py desktop`）自动注入身份 = `<worktree 名>-<commit 前 12 位>`，与 web 启动器现有 run-dir 命名策略对齐。
- 效果：同一 worktree 同一 commit 重启 desktop 复用同一实例目录与 daemon；切 commit 后新开实例，旧实例目录由 D 回收。
- 身份字符串的可用字符约束与 web 侧命名规则一致（避免路径非法字符）。

### 3.6 G：进程级"允许本实例多开"覆盖

- 背景：desktop 的单例锁是机器级命名互斥体、锁名编译期固定，**安装版与开发版、不同 worktree 的 dev desktop 之间竞争同一把锁**。默认 `allow_multiple_instances=false` 时，只要机器上已有任何 desktop 在跑（如安装版常驻 dogfood），dev desktop 会直接不启动并聚焦已有实例——E 的稳定身份走不到生效分支（instance_id 仅在允许多开时使用）。
- 需求：desktop 支持进程级覆盖"本实例允许多开"（入口形态见开放决策），dev 启动路径默认注入；**不读写全局用户配置**。
- 效果：安装版 desktop 常驻时可直接并行启动 dev desktop；安装版的日常行为（二次启动聚焦已有窗口）不受影响。
- 覆盖未使用时，行为与现状完全一致。

## 4. 约束

- **进程安全**：任何自动清理/自愈不得终止存活进程（A、D 共同红线）。
- **token 安全**：daemon 认证 token 只经进程环境传递，不落日志、不进命令行参数（维持现状）。
- **不改 daemon 端口语义**：不引入 `--port=0` 之类新语义，端口选择完全在脚本侧解决（`daemon status --json` 属可选的输出格式增强，见开放决策 3）。
- **向后兼容**：E/G 的覆盖入口未使用时，desktop 产品行为与全局配置语义完全不变；`--run-dir` 显式指定的用户路径行为不变。
- **仓库流程**：非平凡行为变更，两波各自先建 OpenSpec change 再实现；实现后同步 `development-environment` SKILL 文档。
- **测试**：Python 侧用 unittest（本机无 pytest）；C++ 侧改动（E/G）须编译 `acecode_unit_tests` 并跑 `ctest`。
- **输出风格**：脚本控制台输出沿用现有 ASCII 风格提示语，不引入 emoji。

## 5. 验收标准

1. 28080 被其他实例占用时启动 web：端口探测立即完成（毫秒级 bind 探测），全程无 15 秒健康检查等待。
2. 同一 worktree 连续两次启动 web 得到同一端口；两个不同 worktree 并行启动互不冲突。
3. 手动杀掉 daemon 后重跑同一启动命令：自动清理死 run-dir 并成功恢复，全程无需 `daemon stop`。
4. run-dir 指向一个存活的异名 daemon（pid 活、身份不匹配）：启动器不终止它，输出保留"请手动停止"类提示。
5. 人为制造启动失败：run-dir 内日志文件包含 worker 真实输出，错误信息指明该文件路径。
6. 同一 worktree 同一 commit 两次启动 desktop：落在同一实例目录、复用同一 daemon；切 commit 后落到新目录。
7. 不指定身份与多开覆盖直接运行 `acecode-desktop`：行为与现状一致（随机实例目录、遵循全局配置）。
8. 安装版 desktop 常驻时启动 dev desktop：不改动全局配置即可启动为独立实例；全局配置文件未被写入，安装版二次启动仍表现为聚焦已有窗口。
9. 手动全量清理命令一次清掉现存死目录与 `.acecode-dev-run/` 遗留；日常启动后死目录不再堆积。
10. `tests/scripts/` 单测全绿；Wave 2 的 C++ 编译与 `ctest` 通过。

## 6. 分波交付

| 波次 | 内容 | 交付物 |
|---|---|---|
| Wave 1（纯 Python，先行） | C（先落日志拿到诊断能力）→ A（已验证修复路径）→ B → D | 一个 OpenSpec change + 单测 |
| Wave 2（含 C++） | E + G：desktop 身份与多开两个进程级覆盖 + dev 脚本注入 | 一个 OpenSpec change + 单测 + ctest |

Wave 2 建议在 `fix/dev-launcher-runtime-identity`（含未验证的 `atomic_file.hpp` 改动）验证合并后再动 C++，避免两笔未验证的 C++ 改动叠加。

## 7. 已定技术决策（grilling 两轮落定，2026-09-20/21）

以下为技术方案阶段全部决策，作为 Wave 1 / Wave 2 OpenSpec 提议的直接输入。

### 7.1 端口派生（B）

- 散列：`zlib.crc32(safe_identity) % 201 + 28080`，`safe_identity` 复用 `worktree_runtime_dir` 的 `<name>-<commit12>` 字符串——端口与 run-dir 同源同变。禁用 Python 内置 `hash()`（每进程随机加盐）。
- 被占后线性 +1 顺延（区间内回绕），每候选毫秒级 bind 探测；全区间占满才报错。
- 已知副作用：identity 含 commit12，切 commit 后端口随 run-dir 一起变；Vite 代理目标不可写死，验收 2 仅在 commit 不变时成立。

### 7.2 显式端口（B）

- 新增 `--port`（与 `--run-dir` 同级，走 extra parser）。
- 显式端口被占 → 立即报错退出，不顺延（显式 = 外部有确切依赖，静默换端口更难查）。

### 7.3 A 自愈载体

- 脚本内联 `pid_is_alive(pid)`（Windows ctypes `OpenProcess` / POSIX `os.kill(pid, 0)`）；死 → `daemon stop --run-dir=...`（复用其 `cleanup_runtime_files`，已实测验证路径）；活 → 维持"请手动停止"类提示。
- 不用 `daemon status` 退出码判死活（rc=1 含"pid 活但不可复用"，误用会踩红线）；不给 status 加 `--json`（保持 Wave 1 纯 Python）。
- **保守假设（Q10）**：pid 号被 OS 回收给无关进程时判"活"、不自愈、走跳过报告；不做进程身份校验，红线优先于自愈覆盖率。

### 7.4 C 日志形态

- 脚本写 `daemon-worker.log`：Popen stdout/stderr 重定向，append + 每次拉起写分隔头（时间戳 + 参数摘要），>1MB 滚动保留 1 代（`.log.1`）。
- 与 daemon 自带 `daemon-startup.log`（`startup_diagnostics.cpp`）并存；启动失败时报错同时指向两个路径。
- 不做多代保留（与 D 的"零堆积"目标一致）。

### 7.5 D 命令形态

- `prune` 作为与 `web/desktop/tui` 平级的 positional target。
- 输出三段：已清理（目录 + 死 pid）、跳过（目录 + 原因）、总计；存在跳过项退出码仍为 0。
- 结构不认识的目录（含 `.acecode-dev-run/` 遗留）一律只报告不删除。

### 7.6 D 自动清理时点

- 放在"健康检查判不可复用 → 准备新拉起"路径上、端口探测之前；健康复用路径不扫目录（零开销）。

### 7.7 E/G 覆盖入口（Wave 2）

- 只用环境变量，dev 专用定位，C++ 不加 CLI：
  - `ACECODE_DESKTOP_INSTANCE_ID=<worktree>-<commit12>`；空串视为未设置 → 回退随机 uuid。
  - `ACECODE_DESKTOP_ALLOW_MULTIPLE_INSTANCES`：白名单解析，大小写不敏感 `1`/`true`/`yes`/`on` 为真，其余一切（含 `0`、空串）为假；未设置不进入覆盖分支。
- 由 `dev_desktop.py` 注入子进程环境；文档写进 SKILL 与 dev 文档，不进对外产品文档。

### 7.8 E 身份字符串责任划分

- Python 生成时 sanitize（`worktree_runtime_dir` 正则提为共享 helper 供 `dev_desktop.py` 复用）。
- C++ 只校验不清洗：`^[A-Za-z0-9_.-]{1,64}$`，不合法 → 忽略注入值、回退随机 uuid（行为退化为现状，绝不产生非法路径）。

### 7.9 测试切面

- Python：三个外部依赖做成模块级小函数供 `patch.object` 打桩——`pid_is_alive()`、`port_is_available(port)`、集中封装的 worker Popen 调用（断言重定向到 `daemon-worker.log`）。另加：crc32 派生端口固定向量测试（防换成不稳定散列）、线性顺延顺序断言（含回绕）。
- C++（Wave 2）：`is_valid_instance_id()` 与 `parse_allow_multiple_instances()` 做成 `instance_startup.hpp` 的 header-only 纯函数；`main.cpp` 只 `getenv` + 透传，env 读取不进被测代码；单测与 `plan_instance_startup` 现有测试同文件扩展。

### 7.10 worker 提前退出快速失败（C 的延伸）

- 健康等待循环中加 `Popen.poll()`：worker 进程已退出 → 立即失败并指向日志文件，不再等满健康检查超时。至此"启动失败秒级可见"覆盖全部场景。

## 8. 变更记录

| 日期 | 变更 | 原因 |
|---|---|---|
| 2026-09-20 | 初版 | brainstorming 澄清产出：范围定为 A–E（F/G/H 出局），端口策略取 worktree 派生、清理取"自动 + 手动全量"、实例身份取 worktree+commit |
| 2026-09-20 | G 拉回范围并入 Wave 2；新增 3.6 与验收 8 | 用户确认 desktop 会并行跑（安装版 dogfood / 多 worktree）；单例锁为机器级固定锁名，E 在默认配置下走不到生效分支，G 与 E 同代码缝成本极低 |
| 2026-09-21 | §7 由"开放决策"改为"已定技术决策"（12 项）；状态推进为方案已确认 | grilling 两轮：第一轮 8 项（端口派生、显式端口、自愈载体、日志形态、prune 形态、E/G 入口、sanitize 划分、清理时点）全按推荐；第二轮 4 项（测试切面、pid 复用保守边界、bool env 白名单解析、worker 提前退出快速失败）全按推荐 |
