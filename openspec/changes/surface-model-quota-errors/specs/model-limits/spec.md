## ADDED Requirements

### Requirement: 明示额度与限流错误
系统 SHALL 区分用户余额耗尽、独立 Key 额度耗尽、上游用量窗口和临时请求限流，提供状态、错误码及恢复方式。

#### Scenario: 用户额度为零
- **WHEN** 已用尽额度的用户发起模型请求
- **THEN** 返回可读的终止错误，本回合不重新连接或扣费。

#### Scenario: 持续临时限流
- **WHEN** ACEModel 持续返回临时限流
- **THEN** ACECode 有界重试，显示限流原因并结束本回合；后续用户可主动重试。
