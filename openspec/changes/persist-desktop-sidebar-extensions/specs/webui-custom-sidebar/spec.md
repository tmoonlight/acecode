## ADDED Requirements

### Requirement: 桌面扩展布局持久化

Desktop SHALL 在同一用户数据目录中恢复“扩展 → 自定义”的固定项和完整顺序。

#### Scenario: 更换端口后重新打开

- **WHEN** 用户调整固定项和顺序，点“完成”，随后在另一个 daemon 端口重新打开 Desktop
- **THEN** 侧栏、扩展菜单和自定义列表均按上次选择展示

#### Scenario: 用户取消全部固定项

- **WHEN** 用户取消所有固定项并重新打开 Desktop
- **THEN** 所有扩展都留在菜单中并沿用上次顺序

#### Scenario: 升级已有布局

- **WHEN** 桌面尚无持久记录且当前来源存在有效的旧布局
- **THEN** Desktop 将旧布局导入持久记录，后续启动优先使用持久记录

#### Scenario: 普通浏览器打开

- **WHEN** 用户通过普通浏览器自定义扩展
- **THEN** 浏览器按当前来源独立保存和恢复布局
