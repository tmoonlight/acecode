---
id: theme-management-001
scope: 自定义主题导出与删除
status: done
depends-on: []
---

# objective
落实已批准 V2 原型及用户补充的删除主题和无压缩包图标要求。

# context
- openspec/changes/manage-custom-theme-packages/proposal.md
- openspec/changes/manage-custom-theme-packages/design.md
- openspec/changes/manage-custom-theme-packages/specs/custom-theme-management/spec.md
- docs/themes.md
- docs/daemon-api.md

# path
后台：src/adapters/themes、src/apps/web/routes/routes_themes.cpp、必要的偏好路由和原生保存类型、对应 C++ 测试。
前端：web/src/components/ThemeCards.jsx、主题操作控制器、现有偏好队列/ThemeProvider、api、国际化及测试。
文档由主代理维护。

# verification
后端真实 ZIP、持久化和 HTTP 测试；前端保存/取消/删除与缓存和队列测试；Web 全套测试/build；独立源码及有界 UI 检查；OpenSpec 严格校验；git diff --check。

# delivery
遵循用户当前分支/检出偏好，在 master 保留既有脏改动。开发代理先并行只读定位，代码实施按后台、前端顺序交接；不同作者独立复核。不开新分支、不提交、不发布。实现契约由本次“就按这个开始开发”及新增删除要求授权。

# evidence
- OpenSpec `manage-custom-theme-packages` 开发前及契约补全后严格校验通过。
- 后台 MinSizeRel `acecode_unit_tests` 构建通过，主题、ZIP、HTTP/鉴权、偏好与原生保存聚焦测试 60/60 通过（6.79s，无跳过），日志/XML 为 `build/theme-export-tests-20260912.{log,xml}`。
- Web 完整 `pnpm test` 2233 项通过，`pnpm build` 完成 3033 模块及 4427 正则兼容检查，日志为 `build/theme-management-web-tests-20260912.log` 和 `build/theme-management-web-build-20260912.log`。新文案通过 overrides 生成 catalog。
- 独立存储审查发现的四项问题已修复并关闭，见 [存储审查](../reviews/theme-management-001-1.md)。前端迟到配置恢复、取消旧快照覆盖已保存结果两个阻塞问题及状态行隐藏差异已修复；独立 58 项聚焦检查、限定 HTTP/偏好/原生类型复核通过，见 [Web 与协议审查](../reviews/theme-management-001-web.md)。
- 一次机械样式检查报告 12 项，均经独立比对确认是本轮未修改的既有规则，保留原样，见 `build/theme-management-impeccable-detect-20260912.json`。
- 浏览器初始化引用缺失的 `openai-bundled/browser/26.903.71938/scripts/browser-service.mjs`，报 `Cannot find module`。停止该路径，未修改工具安装或绕过策略；没有真实 DOM 截图，原生另存为窗口实际交互亦未手测。组件静态渲染与逻辑测试不替代视觉验收。
- 用户随后追加三个主题外观参数，继续于 [theme-surfaces-001](theme-surfaces-001.md)。两部分已一起完成最终应用/Desktop 构建；`build/MinSizeRel/acecode.exe` 嵌入最新 Web 全量字节，`acecode-desktop.exe` 同目录可用，旧 Release 程序哈希未变。证据为 `build/theme-surfaces-final-status-20260912.json` 和 `build/theme-surfaces-final-artifacts-20260912.json`。
- 两个 OpenSpec change 最终严格校验和全工作区 `git diff --check` 均通过。真实视觉与系统保存窗口的实机检查仍受上述验证边界限制。
