# Design

## Context

见 proposal.md。请求构建器已经按当前 session id 设置 `ChatRequestOptions::prompt_cache_key`，stream collector 也已传递该选项。遗漏发生在两种 OpenAI provider 的端点能力白名单。线上中转镜像对应提交 `9d8a67e1`，无显式身份的 HTTP 请求使用默认 isolated 模式，每请求产生新的上游缓存键。

## Goals / Non-Goals

目标是打通已有会话身份到已验证中转端点的传递，保持其他供应商的默认兼容行为。此次不改服务器隔离模式、不新增全局共享身份、不修改用户模型配置或其他未提交工作。

## Decisions

1. 在 OpenAI provider 的缓存策略实现中增加受保护的 ACEModel 请求端点判断，复用内置 catalog 地址；Chat 与 Responses 分别校验自身后缀，支持 HTTPS 默认 443、完整 URL 查询参数。
2. 继续使用现有按调用快照传入的缓存键，不增加 provider 级可变会话状态；空键不发送，主/子会话键独立。
3. 复用现有显式拒绝回退与端点/模型作用域。Chat 的压缩 tool_choice 能力白名单不随缓存能力扩展。
4. 新增独立回归测试文件，避免干扰用户正在修改的 Responses 测试和序列化文件。验证实际请求 JSON、协议转换、交错会话和拒绝回退。

## Risks / Trade-offs

- 缓存是否命中仍由上游决定 → 用字段与身份稳定性作为确定性验收，用真实请求记录命中表现，不承诺固定命中百分比。
- Windows 运行中程序可能锁住输出 → 先确认构建环境和占用情况，保留旧产物与现有会话，区分已构建与运行中实例已加载。

## Migration Plan

在当前 checkout 增量构建并运行定向测试，使用修复产物进行隔离的真实 ACEModel 连续请求验证。运行中的旧进程需要在安全的会话边界重启后加载修复；不改变服务器配置。
