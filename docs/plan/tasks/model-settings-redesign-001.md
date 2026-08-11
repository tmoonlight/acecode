---
id: model-settings-redesign-001
scope: ACECode 模型设置全栈重做
status: completed
depends-on: []
---

# 模型设置全栈重做

## objective

完整实施 `redesign-model-settings-with-presets` OpenSpec：按既定 WorkBuddy 对比结论重做模型设置页面，并同时交付目录与推荐数据、安全的模型预设修改语义、Provider 运行时高级选项、Web 自适应弹窗和全部验证门禁。严格逐项完成并即时更新 OpenSpec 的 42 个复选框，不得把仅完成入口或静态 UI 视为整体完成。

## context

- `openspec/changes/redesign-model-settings-with-presets/proposal.md`
- `openspec/changes/redesign-model-settings-with-presets/design.md`
- `openspec/changes/redesign-model-settings-with-presets/tasks.md`
- `openspec/changes/redesign-model-settings-with-presets/specs/model-preset-catalog/spec.md`
- `openspec/changes/redesign-model-settings-with-presets/specs/model-profile-runtime-options/spec.md`
- `openspec/changes/redesign-model-settings-with-presets/specs/web-model-management/spec.md`
- `docs/daemon-api.md`
- `AGENTS.md`

## path

- `assets/models_dev/`
- `src/config/`
- `src/provider/`
- `src/web/`
- `src/daemon/`
- `src/models_dev*.{hpp,cpp}` 及当前目录实现所在的等价路径
- `web/src/components/`
- `web/src/lib/`
- `web/src/styles/`
- `web/src/i18n/`
- `tests/`
- `docs/daemon-api.md`
- `openspec/changes/redesign-model-settings-with-presets/`
- 构建与安装清单中仅与上述资产和源码直接相关的条目

## verification

- 覆盖配置、saved model editor、models.dev、Daemon 路由、Provider 请求、模型上下文和会话模型的聚焦 C++ 测试。
- 覆盖模型管理纯函数、组件架构和交互的聚焦 Web 测试。
- `pnpm test`
- `pnpm build`
- `pnpm i18n:audit`
- `scripts/code_quality_check.bat`
- `git diff --check`
- `openspec validate redesign-model-settings-with-presets --strict`
- 对当前环境不能完成的真机或凭据相关手工门禁，必须逐项写明原因和可复现验收步骤，不得伪称通过。

## delivery constraints

- 只在 `N:\Users\shao\acecode\.worktrees\redesign-model-settings-with-presets` 工作。
- 保留 ACECode 原有原生 Anthropic、Copilot、默认模型、搜索、探测、批量新增、手动模型 ID 和请求头能力。
- 不复制 WorkBuddy 的配色；只使用 ACECode 语义 token。
- API Key 不得出现在读取响应、日志、错误文本、DOM 或快照中。
- 所有高级设置必须真正作用于 Provider 请求；不允许只做 UI。
- 先提交开发产出，再交给独立验证代理；验证通过后才允许合并。
