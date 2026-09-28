<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

`SandboxConfig.enabled` 的结构默认值为 true，普通保存会省略默认值。Desktop、TUI、daemon、headless 均调用 `load_config()`；`load_config_from_path()` 同时用于配置编辑和恢复候选校验。`config_mutation.cpp` 已有进程内锁与跨进程文件锁。

## Goals / Non-Goals

目标是按用户配置执行一次迁移，之后保留用户选择。不改权限模式、会话级 `/sandbox on/off` 语义、后端隔离策略、配置恢复校验或发布版本号。

## Decisions

- 在共享 `load_config()` 启动入口检测迁移，保留显式路径读取的原有语义，避免配置恢复候选校验产生迁移副作用。首次正常加载仍负责创建用户目录，保留首次初始化标志。
- 使用顶层 `migrations.disable_sandbox_once` 布尔标记，作为 `AppConfig` 内独立字段参与读取和保存。标记不属于可被安全设置更新替换的 `SandboxConfig`。完成后固定使用此标记，不按版本反复重置。
- 迁移函数复用 `config_mutation.cpp` 的锁，锁内重新读取并校验配置，补丁修改原 JSON 的开关和标记，用已有敏感文件原子写入工具一起提交。保留未知字段，不经整份配置重新序列化。
- 成功后推进 last-good 备份；迁移失败向启动调用者报告错误，磁盘配置保持不变。启动重新加载时才应用环境变量，避免持久化进程环境覆盖。
- 保留结构默认值 true。首次启动（包括新安装）由迁移设置为 false；用户后续启用即使保存时省略 enabled，标记仍可阻止再次关闭。

## Risks / Trade-offs

- 标记被用户删除或旧版本保存时丢弃会被视为未迁移；普通新版本设置保存必须保留标记，并测试此路径。
- 原子写入成功但备份写入失败沿用现有配置保存策略：记录日志，不谎报主配置写入失败。
- 进程锁和文件锁复用现有配置变更边界；不扩展为全仓配置并发重构。

## Migration Plan

随下个版本代码发布生效；当前运行的安装包不直接修改。用户需要恢复沙盒时可使用安全设置中的全局开关。回归覆盖缺省、显式开启/关闭、重新启用、普通保存、并发执行、写入失败和真实启动入口。
