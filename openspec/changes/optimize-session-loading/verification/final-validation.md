# 最终验收（Windows，2026-10-01）

`optimize-session-loading` 的 50 项任务已实现并验收。保持当前 master 检出，HEAD 为 `5e9258d16a9caf978c982e262e985b416e19b040`，保留工作区其他 change 的原有修改。没有创建分支、worktree、发布版本或修改真实会话数据；未执行提交/推送。本 change 可按 OpenSpec 流程归档。

## 结果

- 100MB 合成会话：首屏 **8061.5ms → 286.8ms**，首次历史 **2 次/206.7MB → 1 次/1.61MB**。
- 20MB 首屏 **282.8ms**；100MB 与 20MB 尾页 HTTP 均约 **18ms**。
- 100MB 完整历史读取期间，并发列表 **930ms → 4.426ms**；恢复读取约 **975KB**。
- 实际浏览器向前翻页保留行 ID，滚动位置偏差 **0.375px**；新字节链接与旧序号链接均定位到正确用户消息，浏览器无异常。
- 侧栏稳态 HTTP 条数没有达到最初预期的明显下降；已如实记录，主要收益为列表流量减少 99.66% 和重复 WS 订阅消除。

详细数据见 [阶段 2 结果](stage2-results.md)、[侧栏测量](stage1-sidebar.md) 和对应原始 JSON。基准没有使用真实用户内容，数字不代表所有机器的冷缓存 p95。

## 构建与测试

- `cmake --build build/refactor-phase1-windows --target acecode acecode_unit_tests --parallel 4`：通过。使用该工作树现有 Ninja/MSVC Release 目录，加载正确的 Visual Studio 编译环境；Web 构建后重新配置并嵌入新资源。
- `pnpm test`、`pnpm build`：通过；全量前端 runner 包含分页、恢复、WS 去重、自愈、网格、Sidebar、导航及搜索链接测试。产物兼容性检查没有 lookbehind。
- 完整串行 CTest 覆盖原有 **5366 个登记项**，其中 **9 项按既有条件跳过、1 项原本禁用**；架构项单独执行。完整运行有两个失败：中断优先级的 250ms 时序断言、后台建议恢复状态。**不将这次完整命令的退出码记为成功。**
- 随后发现并修复已升级索引的初始化仍申请写锁的问题：只读检查 `user_version`，仅迁移时拿写锁，锁内再核对版本。增加“另一个连接持有写事务时仍可初始化读取”的测试。
- 最终二进制：上述两个失败用例及索引测试共 **17 项连续 5 轮通过（85 次，0 失败）**。再以 CTest 串行复验整个受影响集合，**71/71 通过**，含会话恢复、历史分页、索引迁移/追加、任务建议、中断与 HTTP 搜索/分页。新增回归后总登记项为 **5367**。所有观察到的失败均已关闭，没有提高断言的时间阈值。
- `check_layers.py --layout final --strict`：**0 finding**；没有放宽 R12 或分层约束。检查器只认 Git index，使用临时 index 将当前新文件纳入，真实 index 不变。临时 index 仅传给架构检查，避免影响测试里自建 Git 仓库的操作。
- `openspec validate optimize-session-loading --strict`、`git diff --check`：通过。

最终二进制与 Web 产物 SHA-256 见 [机器记录](final-validation.json)。构建/测试原始日志保留在本机 `C:/Users/shao/AppData/Local/Temp/ace-session-loading-edit/`，包括 `schema-readonly-build.log`、`schema-readonly-tests.log`、`final-ctest-clean.log`、`final-affected-ctest.log`、`final-layer-lint.json`；前端日志在同一 Temp 下的 `ace-session-web-final-*.log`。

## 实现边界

JSONL 格式保持，SQLite 只扩充既有可重建搜索投影。daemon 按最新有效压缩点恢复，TUI 仍完整显示；文件回退、差异及完整导出按需读取旧数据。前端与 daemon 的新分页字段应同包交付。阶段 3/4 的现场门槛尚无满足证据，结论与数据限制已在阶段 2 结果中记录，没有擅自扩充存储重构范围。
