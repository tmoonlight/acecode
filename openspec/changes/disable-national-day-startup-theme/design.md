<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

默认配置已经是 `theme: system` 与 `color_theme: blue`。节日主题来自 `App.jsx` 在恢复 daemon 外观后调用 `applyStartupTheme`，而非默认配置值。动机见 proposal.md。

## Goals / Non-Goals

仅调整客户端启动入口；保留已保存外观、资源缓存及手动主题管理。此次不迁移用户配置，不修改 daemon 协议或主题发布资源。

## Decisions

- 从认证后的外观恢复 effect 移除 `applyStartupTheme` 调用及下载控制器依赖，使该入口只读取并恢复配置。
- 保留下载控制器与首次领取接口的兼容实现：关闭唯一产品启动入口即可满足行为要求，避免扩大到无关 API 清理。架构回归检查约束入口不会重新调用首次领取流程。
- 不把现有 `national-day-2026` 配置重置为蓝色；现有数据无法区分历史自动应用和手动选择，强制重置会覆盖用户选择。
- 复用现有测试验证默认外观与持久化恢复，并补充国庆节的手动安装及已有主题恢复覆盖。

## Risks / Trade-offs

- 历史自动应用的国庆节仍会作为已保存主题恢复 → 用户仍可手动切换；本次取消后续版本的默认自动应用。
- 旧版客户端仍可能调用保留的领取接口 → 本次行为由新版本客户端入口控制，文档区分历史兼容协议与当前启动策略。

## Migration Plan

随下次常规 Web/Desktop 构建生效，无需迁移配置或清除旧标记。恢复旧启动调用即可回退代码策略。
