## Context

见 proposal.md。当前 ThreadScope 只保留 cwd；ThreadService::create 只设置 cwd/model，SessionOptions 的 no_workspace 默认 false。界面按 no_workspace 与可见 WorkspaceRegistry 分别列举会话。续聊的 resolve_thread 只查调用方项目，无法覆盖目标切换后的创建结果。

## Goals / Non-Goals

**Goals:** 在工具适配层保留结构化归属，在服务层统一解析目标，复用现有注册、缓存目录分配和全局会话定位机制。

**Non-Goals:** 不迁移历史数据、不修改提问工具行为、不扩展删除/归档等其他写工具作用域，不变更 UI 布局或发布运行版本。

## Decisions

1. 引入 ThreadCreateTarget 值对象；ThreadScope 补充 no_workspace。工具通过调用方 SessionManager 的元数据提取归属，不根据路径名猜测。创建支持 workspaceHash/cwd/noWorkspace，互斥冲突在执行前报错。
2. 默认继承 no_workspace；无工作区交给 SessionRegistry 分配独立缓存目录，避免多个独立会话共用文件写入位置。普通目录复用 WorkspaceRegistry::register_new，随后读取持久化 marker 确认可见性，避免只报告内存注册成功。显式 hash 保留注册表中的 cwd 身份；显式目录使用 canonical 路径。
3. 返回实际 entry 的 cwd、存储 hash、noWorkspace；失败清理由新 entry 的存储位置决定，不能继续使用调用方 scope.cwd。
4. 续聊复用 resolve_discovered_thread，支持 workspaceHash；恢复使用目标元数据，并验证 runtime 中同 ID 的存储项目一致，阻止跨项目误投递。其他写工具保持既有作用域。
5. 用临时 HOME 的真实 SessionRegistry 与记录输入的 LocalSessionClient 测试替身验证持久化和路由，不发真实模型请求；追加 Web 列表验证（若已有测试入口适用），复用 Windows 增量构建。

## Risks / Trade-offs

- 显式创建到隐藏工作区会使该工作区重新可见 → 保留名称、图标和附加目录，符合用户创建后能找到的预期。
- 新增可选参数不能破坏旧调用 → 默认行为只修复归属，title/model/prompt 保持原有约定。
- 当前运行中的 Desktop 不会热加载 C++ 修改 → 验证源码和测试，不将其表述为已替换运行版本。
- 工作区注册后首条投递可能失败 → 清理新会话文件，保留可见工作区注册及原有会话数据。

## Migration Plan

代码合入后随下次构建启动生效。已有不可见会话不做自动修复；本次不直接改写正在运行会话的磁盘元数据。回滚可移除新增可选工具参数与目标解析实现，既有会话格式不变。
