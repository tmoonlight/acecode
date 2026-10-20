## 1. 网站同步能力

- [x] 1.1 增加必需标签校验与仅刷新 Release 模式，单元测试覆盖缺失、未完整发布及幂等内容。
- [x] 1.2 更新网站部署说明并通过现有下载清单测试，发布网站仓库变更。

## 2. 发布衔接

- [x] 2.1 添加可复用工作流及 package 发布后的调用，完成工作流语法与依赖检查，补齐发布说明。
- [x] 2.2 配置网站专用部署密钥和 ACECode Secret，验证授权限定目标仓库。
- [x] 2.3 通过 PR 合入 master，精确提交本任务文件并确认其他未提交内容保持。

## 3. 联调

- [x] 3.1 用现有 Release 执行同步，确认自动 push 触发 Pages，并验证 CDN 清单内容。
- [x] 3.2 重复执行同版本确认没有空提交，记录成功运行链接及严格 OpenSpec 校验。

## 验收记录

- 发布衔接通过 [PR #152](https://github.com/tmoonlight/acecode/pull/152) 合入，合并提交 `64a2b3ad7`。
- 使用已有 `v1.0.13` 完成 [首次同步](https://github.com/tmoonlight/acecode/actions/runs/39451555347)，自动生成网站提交 `155bce0`，并触发成功的 [Pages 部署](https://github.com/tmoonlight/acecode-site/actions/runs/39451592217)。
- [同版本重试](https://github.com/tmoonlight/acecode/actions/runs/39451786802) 成功，网站 HEAD 仍为 `155bce0`，无空提交。两次均完成公开 CDN 清单哈希验证。
- 网站生成器 5 项单元测试、104 个版本/1,165 个资产的下载逻辑检查、工作流语法/依赖/权限检查和 OpenSpec 严格校验通过。
- 主仓库 CI 的既有 R15 `delayed_injection` 检查在 `src/apps/desktop/linux_desktop.cpp` 和 `.hpp` 失败；变更前 master `01d74217` 已有同项失败。本任务没有修改这些源文件，未将主仓库全量 CI 记为通过。
