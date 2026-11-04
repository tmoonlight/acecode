# 验证记录

2026-11-04 发布补验：等待谓词改为显式借用三个成员并按值捕获 generation，不改变同步等待语义。严格 ownership、layers、size、includes、doc-path 和 map 检查通过；Windows Release 三目标构建通过；相关原生回归执行 360 项，0 失败，2 项 WhatsApp 实网依赖用例按原有条件跳过。完整 Web 测试和构建、定时任务与快捷键浏览器验证通过。快捷键脚本首次在并行原生编译期间等待设置页超时，构建负载降低后原脚本复测通过，未放宽断言。

日期：2026-11-03（Asia/Taipei）。

- `web/pnpm test`：通过，包括新增 history shortcut 测试、生产 RichComposer 回调回归、快捷键存储和国际化覆盖检查。
- `web/pnpm build`：通过；4594 个正则、99 个 bundle 的兼容检查通过。沿用构建器现有大 chunk 提示。
- `cmake --build build/windows-x64-msvc --config Release --target acecode acecode_unit_tests --parallel 8`：通过，产出 Release daemon/CLI 和单元测试程序。
- `python scripts/refactor/run_fast_tests.py --binary build/windows-x64-msvc/tests/Release/acecode_unit_tests.exe --profile fast --shards 2 --filter "LoopStore.*:LoopScheduler.*:LoopSchedule.*:WebServerHttp.Loop*"`：43 项执行，0 失败、0 跳过。覆盖原有调度、手动执行与定时执行竞争、工作区忙、停用/过期任务、排队任务关停、鉴权和模型缺失失败时保留计划。
- `web/scripts/test-shortcut-editing.mjs`：完整 App/Settings/ChatView 浏览器验证通过。覆盖自动保存、录入隔离、冲突、刷新持久化、跨窗口同步、存储失败后重新打开及重试、自定义撤销/重做、旧键失效、恢复默认和原生输入框历史。1440 浅色、420 深色界面已查看。证据：`C:/Users/shao/AppData/Local/Temp/ace-shortcut-editing-S9dM1c`。夹具关窗时出现一次未被拦截的首页意图请求代理拒绝日志；浏览器运行时错误列表为空，全部断言通过。
- `web/scripts/test-scheduled-task-creation.mjs`：真实 LoopPage/表单/Slate 编辑器配合 API 夹具通过。覆盖播放按钮、请求中禁止重复、忙错误和重试、停用状态保留、展开运行记录及 Markdown 自定义历史。1280 浅色、390 深色界面已查看；小屏操作区分行。证据：`C:/Users/shao/AppData/Local/Temp/ace-scheduled-ui-eXnd6k`。
- Impeccable detector：LoopPage、ShortcutsSettings 无发现。
- `openspec validate add-scheduled-run-and-history-shortcuts --strict`、`git diff --check`：通过。

原生测试日志：`N:/acecode-gtest-iso/114801`。构建与前端日志在系统临时目录 `ace-history-*`。本次未启动或重启实际桌面客户端，未调用用户真实定时任务，未提交代码。原有桌面窗口相关修改保持原状。OpenSpec 文件遵循仓库现有忽略规则保存在本地。
