## Why

国内网络常无法访问 DuckDuckGo，现有 RSS 索引也无法覆盖一般网页查询。用户需要开箱即可使用的中文网页搜索。

## What Changes

- 为国内和地区尚未探测的请求提供匿名 AnySearch 搜索，结果不足时使用 360 自然搜索结果补齐。
- 默认并行模式同时保留 RSS。已探测的海外网络继续使用原有来源。允许通过配置或会话命令选择国内线路。

## Capabilities

### New Capabilities

- `domestic-web-search` 提供无 Key 网页搜索、按需补搜和区域路由。

### Modified Capabilities

## Impact

影响共享搜索适配层、配置和 TUI 命令。三个入口复用同一路由策略，工具参数和返回格式兼容。
