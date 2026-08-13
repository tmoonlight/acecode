## 背景

现有 `package` 工作流用一个 `macos-release.enabled` 开关控制应用签名、公证、更新 ZIP 和 PKG。该开关要求 Application 与 Installer 两套证书全部存在，因此只缺 Installer 证书时，正式标签会在 macOS 构建开始阶段失败，无法发布本来可以安全生成的签名应用资产。

## 目标 / 非目标

**目标：**

- 分离“可信 macOS 应用资产”和“可信 PKG”两级凭据门槛。
- 正式标签缺少 Installer 凭据时发布零个 PKG，而不是生成 unsigned PKG。
- Installer 凭据齐全时保持现有两个架构 PKG 的签名、公证和验证流程。
- Release 收集阶段拒绝单个 PKG、unsigned PKG、DMG 和 unsigned 更新 ZIP。

**非目标：**

- 不恢复 DMG。
- 不允许 `Developer ID Application` 代替 `Developer ID Installer` 签署 PKG。
- 不降低 macOS 应用、更新 ZIP 或归档的签名与公证要求。
- 不改变自更新 manifest 继续使用 ZIP 的约束。

## 决策

### 决策 1：输出两个独立凭据状态

凭据检查输出 `enabled`（应用签名/公证）与 `pkg_enabled`（Installer）两个状态。正式标签缺少 Application 或公证凭据仍立即失败；Installer 两项同时缺失时仅输出通知并禁用 PKG；只缺其中一项视为配置损坏并失败。

备选方案是完全跳过 macOS，但这会无谓失去可正常签名和公证的更新资产，因此不采用。

### 决策 2：Installer 证书按需导入

临时 keychain 始终只在 `enabled=true` 时创建并导入 Application 证书；仅在 `pkg_enabled=true` 时写入、导入和验证 Installer P12。PKG 创建、验证和上传步骤使用同一条件，避免空路径或伪身份进入打包命令。

备选方案是继续导入占位证书，但占位不能建立 Apple 信任，且会掩盖配置错误，因此不采用。

### 决策 3：Release 接受零个或两个 PKG

资产收集允许 PKG 数量为零或二。数量为二时必须精确匹配 x64 与 arm64 文件名；数量为一或出现 unsigned 后缀时失败。这样既支持本次无 PKG 发布，也保持未来恢复 PKG 时的双架构完整性。

## 风险 / 权衡

- [风险] 用户可能误以为 GitHub Release 仍提供图形安装器。→ 在发布说明与文档中明确本次可省略 PKG，保留更新 ZIP 和归档。
- [风险] Installer Secret 只配置一半时被静默跳过。→ 将部分配置视为硬错误，不允许继续。
- [风险] 未来证书补齐后 PKG 未自动恢复。→ `pkg_enabled` 由两项 Installer Secret 自动计算，完整配置后无需改代码即可恢复。

## 迁移计划

1. 更新工作流与文档并通过契约检查。
2. 在无 Installer Secret 的 `workflow_dispatch` 上确认 macOS 应用签名、公证、可信更新 ZIP 成功，且没有 PKG artifact。
3. 推送 v0.8.16 标签，确认 Release 不包含 DMG、unsigned 文件或 PKG。
4. 后续配置 Installer P12 后再次干跑，确认两个 PKG 自动恢复。

## 待确认问题

无。
