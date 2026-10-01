## ADDED Requirements

### Requirement: MSIX 安装由商店管理升级
系统 SHALL 对拥有 Windows 包身份的 ACECode 在检查更新和执行更新时使用商店管理路径，不请求普通自更新 manifest、不下载 ZIP、不创建替换安装目录的 runner。

#### Scenario: 商店安装检查更新
- **WHEN** MSIX 安装用户检查更新
- **THEN** API 返回 store_managed 且 update_available 为 false，界面说明前往 Microsoft Store 获取更新，不宣称已是最新版本

#### Scenario: 商店安装执行命令行升级
- **WHEN** MSIX 安装用户运行 acecode upgrade 或 acecode update，包括 force 参数
- **THEN** 命令输出商店更新说明并以非零退出码拒绝自替换，不访问自有升级服务或修改安装目录

#### Scenario: 普通安装检查和执行升级
- **WHEN** 程序没有 Windows 包身份
- **THEN** 检查和升级继续遵守既有自更新配置、下载、校验及回滚行为
