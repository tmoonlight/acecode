## Context

问题与范围见 proposal.md。`web/src/lib/markdown.js` 维护单个 markdown-it 实例，完整、行内和分块渲染共用该实例。当前 Node 为 22，前端使用 ESM；插件 3.0.0 要求 Node >=18，提供默认导出，与现有调用方式兼容。

## Goals / Non-Goals

- 目标：直接解析现有回复中的中日韩强调，不预处理或改写原文。
- 非目标：修改 TUI、独立可视化编辑器或模型提示词；本次不发布或替换运行中的 Desktop 二进制。

## Decisions

- 固定 `markdown-it-cjk-friendly` 为 3.0.0，通过 `md.use` 接入共享实例，保留任务列表、代码高亮、HTML 转义及链接处理。
- 不全局移除 Unicode 标点判断。实测该方案会让 `提示：**(可选)**` 和 `中文，__重点__。` 由正常加粗退化为字面文本。
- 不用正则替换原文。该做法容易误改代码、转义和流式中间状态，也会让显示与复制内容不一致。
- 通过独立的渲染回归测试覆盖全角标点、中英混排、日文/韩文、删除线和流式状态；纳入现有 `runTests.js`。
- 按上游 [使用说明](https://github.com/tats-u/markdown-cjk-friendly/tree/main/packages/markdown-it-cjk-friendly) 接入，实际兼容性以本仓库测试及构建结果为准。

## Risks / Trade-offs

- 新增插件调整中日韩边界，包括删除线 → 同时覆盖已有正常写法与字面代码，执行全量 Web 测试。
- 新依赖影响浏览器打包 → 执行 `pnpm build` 及现有旧版 WebKit 正则检查，并用浏览器验证生成的 HTML。
- Web 构建成功不代表已安装 Desktop 生效 → 明确记录验收范围，实际桌面发布仍需后续嵌入资产并重建。

## Migration Plan

更新依赖、渲染器与测试，完成 Windows 本机 Web 验证。现有会话数据无需迁移。回退时撤销插件注册和对应依赖即可恢复原有解析行为。
