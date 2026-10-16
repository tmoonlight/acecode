## ADDED Requirements

### Requirement: Apple 票据服务断线必须重试
macOS 打包流程中的每一次 `stapler staple` / `stapler validate` SHALL 经 `scripts/macos_stapler.sh` 的 `acecode_stapler` 执行。退出码 68（无法连接 Apple 票据服务）MUST 原样重试同一命令，默认最多 6 次、每次间隔 15 秒；其它退出码 MUST 立即返回，不得重试。

#### Scenario: 断线后恢复
- **WHEN** stapler 前两次因票据服务断线返回 68，第三次成功
- **THEN** 同一命令被重试，打包继续并产出可信资产

#### Scenario: 票据服务持续不可用
- **WHEN** stapler 在全部重试次数内都返回 68
- **THEN** 该步骤以 68 失败，不继续公证或产出后续安装器

#### Scenario: 非断线类 stapler 错误
- **WHEN** stapler 因票据缺失或签名无效返回 68 以外的退出码
- **THEN** 不重试，步骤立即失败

### Requirement: 不得绕过重试直接调用 stapler
`scripts/macos_*.sh` 与 `packaging/macos-installer/build.sh` 中除 `scripts/macos_stapler.sh` 外 MUST NOT 直接调用 `xcrun stapler`；契约测试 SHALL 在出现直接调用时失败。

#### Scenario: 新增直接调用
- **WHEN** 有脚本新增一条 `xcrun stapler` 调用
- **THEN** `tests/scripts/macos_release_scripts_test.sh` 失败并提示改用 `acecode_stapler`
