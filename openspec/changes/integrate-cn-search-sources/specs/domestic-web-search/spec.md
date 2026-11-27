## Purpose

让使用中国大陆网络或首次运行时尚未取得地区缓存的用户，通过内置工具直接取得可引用的中文网页结果。路由可复用既有地区探测，按需补搜以控制单用户低频使用的请求量。

## ADDED Requirements

### Requirement: 匿名国内网页搜索
国内搜索 SHALL 首选 AnySearch 匿名接口，仅在去重后的可用条目不足时请求 360，并标明每条的来源。诊断 SHALL 省略上游错误正文。

#### Scenario: 额度耗尽
- **WHEN** AnySearch 返回 HTTP 402
- **THEN** 在剩余时间内查询 360，返回可用结果且不回显凭据

#### Scenario: 结果不足
- **WHEN** AnySearch 只有部分有效结果
- **THEN** 保留主源顺序，在总条数上限内补入 360 的不同原始链接，补搜失败时保留已有结果

#### Scenario: 超时与取消
- **WHEN** 主源响应缓慢或用户取消
- **THEN** 两源共享配置的超时预算，取消后停止启动后续请求

### Requirement: 按网络选择网页来源
默认并行搜索 SHALL 根据可达性探测选择网页来源，同时搜索 RSS。用户 SHALL 能显式选用国内搜索。

#### Scenario: 国内或首次启动
- **WHEN** 缓存地区为国内或尚未探测
- **THEN** 默认并行模式返回国内网页和 RSS 结果，auto 模式选择国内后端

#### Scenario: 海外与会话选择
- **WHEN** 探测判定 DuckDuckGo 可达
- **THEN** 默认并行模式采用 RSS 和 DuckDuckGo，auto 使用 DuckDuckGo，已手动选择的单源继续有效
