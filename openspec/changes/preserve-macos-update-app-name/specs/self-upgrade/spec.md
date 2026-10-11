## ADDED Requirements

### Requirement: macOS update copies preserve the application name

macOS 自更新 SHALL 在暂存、备份及失败隔离阶段保持应用文件名 `ACECode.app`，唯一编号 SHALL 仅放在外层目录。系统 SHALL 保留原有签名验证和失败回滚能力，不更改应用签名内容。

#### Scenario: Update completes before the old process exits
- **WHEN** 自更新替换完成但旧应用仍在运行
- **THEN** 旧应用所在的备份 bundle 仍名为 `ACECode.app`
- **AND** 返回的备份路径指向可恢复的旧应用

#### Scenario: Replacement fails and rollback succeeds
- **WHEN** 安装失败但可恢复原应用
- **THEN** 原应用恢复到正式安装路径，当前更新的空容器被清理

#### Scenario: Rollback cannot restore the original
- **WHEN** 安装和回滚均失败
- **THEN** 仍含旧应用或失败应用的容器被保留，不因清理操作丢失副本

#### Scenario: Update directory already exists
- **WHEN** 本次更新选定的私有根路径已经存在，包括符号链接
- **THEN** 更新拒绝接管该目录，且不删除或覆盖其内容

#### Scenario: Backup copy attempts self-update
- **WHEN** 运行于更新私有目录中的应用尝试自更新
- **THEN** 安装位置校验拒绝该副本作为正式安装目标
