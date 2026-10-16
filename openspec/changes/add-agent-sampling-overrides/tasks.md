## 1. 数据与专家预设

- [x] 1.1 复用四项采样类型，增加字段合并与严格覆盖解析，并覆盖零值、类型及范围拒绝测试。
- [x] 1.2 接入专家加载、引用成员投影、CRUD 与 DTO，验证保存/回填、旧客户端保留及显式清除。

## 2. 子会话执行

- [x] 2.1 接入会话持久化及统一模型解析，验证优先级、隔离、恢复、热重载和专家/模型切换。
- [x] 2.2 接入 spawn_subagent 与 agent_spawn schema/解析/执行，验证合法覆盖到达 provider、错误在任务开始前返回。

## 3. 界面与交付验证

- [x] 3.1 增加专家采样表单及字段校验，验证真实表单保存/回填/清除、零值和窄屏布局。
- [x] 3.2 更新子 agent、daemon API 与专家包说明，完成 Windows 定向原生测试、Web 全量测试与构建、OpenSpec strict 和差异检查。

## 验证记录

- Windows / MSVC / MinSizeRel 原生增量构建通过；最终相关回归组 181/181 通过，包括 9 项新增测试。结果：`build/agent_sampling_final_tests.xml`。
- 扩展回归曾发现一项既有失败：`SessionRegistry.CreateNoWorkspaceKeepsWorkspaceHashEmpty` 仍断言目录名 `no-workspace`，当前工作区在本任务之前已有的 `compatible_generated_path` 改动返回 `no_workspace`。保留该命名改动与原测试，本任务最终回归仅排除此项；原始失败记录为 `build/agent_sampling_regression.xml`。
- `pnpm test`、`pnpm build` 通过；专家管理脚本新增 2 项采样测试与既有 6 项头像测试通过。
- 真实浏览器组件验证通过：四参数保存、零值、回填、清除继承、非法 Top K 阻止保存、亮色桌面及暗色窄屏无溢出。执行入口：`web/scripts/test-expert-sampling.mjs`，API 使用测试夹具。
- OpenSpec strict、分层检查、所有权检查、界面机械检查和 `git diff --check` 通过。新增文件以 intent-to-add 纳入差异审查，未提交或推送。
- 未重启或替换用户正在使用的桌面程序；浏览器与源码测试不代表已安装 Desktop 已加载新代码。
