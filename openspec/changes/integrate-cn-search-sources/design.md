## Context

默认 `parallel` 并发 RSS 与 DuckDuckGo。`auto` 以前始终选择 DuckDuckGo。已有探测器以 DuckDuckGo 可达性区分网络，支持缓存与后台发布结果。

## Decisions

- 将 AnySearch 和 360 组合为一个可选的 `domestic` 后端。AnySearch 请求带中文和国内区域参数。每次只查询一页，对两源都检查真实 HTTP(S) 链接、合并重复项并保留来源。
- 国内或无缓存时，`parallel` 并发国内后端与 RSS，合并时先取国内网页。`auto` 直接使用国内后端。海外的两种模式沿用原有路径。显式单源配置沿用原来的行为。
- 采用同一搜索截止时间，为 360 预留一半时间；AnySearch 提前返回时剩余时间均可用于补搜。取消后停止发起请求。来自服务端的错误正文不进入诊断。
- 三个入口在无缓存时均可发布探测结果。异步探测可以更新地区，会话中手动选定的后端由用户通过 reset 或 refresh 重置。

## Risks / Trade-offs

AnySearch 的匿名额度受出口 IP 影响，360 的网页结构可能变化。离线夹具覆盖两种故障和部分成功；本次按用户要求跳过大陆出口实测。
