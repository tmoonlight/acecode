## 1. Hook 事件契约

- [x] 1.1 为 `SessionTitleChanged` 载荷、matcher 与裸配置识别新增先失败的单元测试。
- [x] 1.2 新增事件常量、载荷构造器与 `AgentLoop` 观测型派发入口。
- [x] 1.3 为 title child environment 的跨平台安全传递新增 runner/manager 回归测试并实现环境覆盖。

## 2. 标题更新入口

- [x] 2.1 在 `/title <text>` 与清除路径成功提交后派发，保持无参数查询不派发。
- [x] 2.2 在 TUI 自动标题与 daemon 自动标题成功接受后派发，拒绝/过期结果不派发。
- [x] 2.3 在 `/resume`、`acecode --resume` 与 daemon resume 成功后派发，包括空标题恢复。
- [x] 2.4 在活动 session 的 Web 标题 API 更新后派发，并为关键入口补回归测试。

## 3. Herdr managed seed

- [x] 3.1 在 agent-reporting seed 中新增跨平台 `SessionTitleChanged` handler，使用精确 `HERDR_TAB_ID` 与安全标题环境变量。
- [x] 3.2 更新 seed 版本、manifest、官方定义指纹与安装/升级/用户改动保护测试。
- [x] 3.3 用假 Herdr CLI 验证非空标题参数、空标题跳过、环境缺失降级与特殊字符保持。
- [x] 3.4 更新 Herdr hook 文档，记录事件载荷、`/title`/`/resume` 行为和自动 tab 同步限制。
- [x] 3.5 修复状态漂移下历史官方 hook 的升级识别，再次提升 bundle 版本，并覆盖官方副本升级与真实用户修改保留。

## 4. 验证与交付

- [x] 4.1 运行 focused hook/session/title/seed 测试与相关构建。
- [x] 4.2 运行完整单元测试、代码质量检查与严格 OpenSpec 校验，并记录完整套件中的偶发失败与独立复跑结果。
- [x] 4.3 在真实 Herdr pane 中验证 `/title`、自动标题或 resume 能更新当前 tab，且 agent 生命周期仍正常。
