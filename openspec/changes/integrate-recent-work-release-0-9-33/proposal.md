## Why

用户要求整合 2026-10-03 00:38:52 至发布开始时近 24 小时内的主工作区及 worktree 改动，发布正式版，并在发布完成且 Claude 工作结束后关机。盘点发现升级 UTF-8 错误处理和诊断去重仍有未合入部分；当前 master 的 TUI 内置命令文件超过尺寸棘轮，阻断原生 CI。

## What Changes

- 保留已集成的代码，仅三方合入两处 worktree 的升级修复及回归测试，连同主工作区的新对话思考深度、中文 Markdown 强调、蜂群耗时摘要修复一起交付。
- 将 `/tasks` 命令实现移到同层独立模块，保留注册顺序及行为，恢复严格分层与尺寸检查。
- 升级版本至 0.9.33，完成本机、CI、签名、公证、完整产物及更新服务验证。

## Capabilities

### New Capabilities

- `recent-work-release-integrity`: 最近改动整合和完整正式发布的可追溯验证。

### Modified Capabilities

无。

## Impact

涉及升级组件、升级 HTTP 接口和测试、TUI 命令组织、既有前端修复以及发布版本和记录。保持旧分支/worktree 不变，不删除旧引用；不覆盖并发编辑。
