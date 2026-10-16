## Purpose

允许用户按子任务与专家角色配置采样参数，同时复用既有模型配置与执行引擎。保证参数覆盖规则、会话恢复、错误处理及不同发起入口的一致性，使实际模型请求可靠地体现用户设置。

## ADDED Requirements

### Requirement: 子 agent 发起支持采样覆盖
系统 SHALL 在 `spawn_subagent` 与 `agent_spawn` 接受可选 `sampling` 对象，支持 temperature、top_p、top_k、min_p。缺失、null 或空对象表示无额外覆盖；数值零 MUST 按参数范围保留。

#### Scenario: 按字段覆盖且隔离父会话
- **WHEN** 模型设置 temperature=1、top_p=0.9，发起子 agent 指定 temperature=0
- **THEN** 子 agent 的请求使用 temperature=0、top_p=0.9
- **THEN** 父会话、兄弟会话与已保存模型不发生变化

### Requirement: 专家预设支持采样配置
系统 SHALL 在专家包与管理 API 读写 `sampling`，并在专家编辑器提供四项可选输入和取值校验。旧客户端省略 sampling MUST 保留原预设，显式 null 或空对象 MUST 清除预设。

#### Scenario: 保存回填与清除
- **WHEN** 用户保存专家采样参数并重新打开编辑器
- **THEN** 输入显示保存的参数；全部清空后保存则恢复继承

#### Scenario: 专家团使用成员预设
- **WHEN** 专家团主理人派遣具有采样预设的成员
- **THEN** 成员使用自己的专家预设；团队默认值仅补充该成员未设置的字段

### Requirement: 统一优先级与生命周期
系统 SHALL 按字段使用本次发起覆盖值、所选专家预设、模型默认值的优先级，并持久化会话显式覆盖。创建、恢复、模型重载与切换 SHALL 保持该规则。专家切换 SHALL 在现有控制边界一起更新角色与采样配置，失败时保持原绑定。解绑继续遵守既有仅清元数据、不立即清除已加载角色状态的约定。

#### Scenario: 恢复会话与热重载
- **WHEN** 带有显式采样覆盖的子会话恢复，或其已保存模型配置重新加载
- **THEN** 显式覆盖继续生效，未覆盖字段使用当前专家预设与模型值

#### Scenario: 本次发起值优先于专家
- **WHEN** 专家 temperature=0.4、top_k=20，发起子任务指定 temperature=0.8
- **THEN** 子会话使用 temperature=0.8、top_k=20

### Requirement: 拒绝无法生效的采样配置
系统 MUST 复用模型采样范围与 provider/protocol 支持校验，对错误类型、越界值、未知采样字段及不受支持的参数返回明确错误；MUST 在子任务开始前拒绝无效配置，模型或专家切换失败不得改变原有效状态。

#### Scenario: 协议不支持参数
- **WHEN** 使用 Responses 协议的子会话请求 top_k
- **THEN** 发起失败并说明不兼容原因，不向模型发送任务

#### Scenario: 参数类型错误
- **WHEN** sampling 的 top_k 是小数或 temperature 是字符串
- **THEN** 发起或专家保存失败并说明字段错误
