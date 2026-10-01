# 阶段 0 与单次历史加载（2026-10-01）

已实现 2.1–2.4 和 3.1–3.3。

- MSVC Release 构建 acecode 与 acecode_unit_tests 通过。
- 7 个诊断/计时/HTTP smoke 测试通过；CLI 在隔离 profile 上输出 1500 会话、5 个样本。
- 前端新回归先失败（实时化后 2 次历史请求），修复后覆盖“磁盘先返回”和“恢复先完成”两种顺序，均只有 1 次请求；已有恢复、过期响应与模型记录测试通过。
- 前端全量 Node 测试（pnpm test 的同一 runTests 入口）通过；pnpm build 通过。
- 100MB 同一合成数据，Chromium + 最新 web/dist：首屏 3038.1ms，历史请求 1 次，103,353,724 字节。基线为 8061.5ms、2 次、206,707,448 字节。
- 前端上报日志记录 elapsed_ms=2902.3、history_requests=1、history_bytes=103353724。自动浏览器采用等待行可见的外部观察，故与应用双动画帧计时存在约 136ms 差异。
- 隔离 daemon 日志实际包含 load、resume、history、metadata_list、session_list、workspace_list、pinned_sessions、pinned_order、status_snapshot、session_open 各类记录。
- 该次 history 操作记录 read_bytes=104881846、opened_files=2、parsed_records=15719；此时仍为全量读取，后续阶段继续优化。
