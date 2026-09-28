<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## 背景

`ChatView.handleOpenExistingDirectory` 与 `Sidebar.onAddWorkspace` 都使用 `pickExistingWorkspace`。该函数在存在 `aceDesktop_addWorkspace` 时直接返回或抛错，只有没有 bridge 时才调用 `requestPathPick`。`PathPickerHost` 已在 App 顶层挂载，Desktop 也能复用它。

`add-web-path-picker` 的未归档规范要求 Desktop 始终使用原生对话框；本变更按当前用户要求增加工作区目录选择失败后的例外，原生正常成功或取消时仍保持原行为。

## 目标与边界

回退由共享的工作区选择函数唯一负责，覆盖首页和侧栏。文件预览、设置页的目录浏览、原生协议与目录浏览 API 保持各自现有职责。

## 决策

- 将原生调用及 JSON 解析放入恢复边界；原生空结果直接返回 `null`，错误或缺少 `hash` 的非空结果继续进入已有 Web 分支。使用结构化结果判断，不匹配平台错误文案。
- 成功原生结果继续尝试补注册，补注册失败仍返回原生工作区；不会因此弹出第二个选择器。
- 共享原有 `mode: 'folder'`、`purpose: 'workspace'` 和 API 参数。Web 确认后的 `registerWorkspace` 留在原生错误恢复边界外，让最终错误沿现有 toast 链路传播。
- 不在两个 React 调用方分别添加回退，避免分叉；不在 C++ 层启动 Web 弹窗，因为现有宿主及 Promise 生命周期已由前端管理。

## 风险与验证

- 取消被误认为失败会产生第二个弹窗：覆盖原生对象空值、JSON null、空字符串和 Web 取消。
- 恢复边界过宽可能吞掉最终注册错误：覆盖 Web 选择器异常、注册异常及错误响应，并检查调用次数。
- 原生结果格式异常也应恢复：覆盖拒绝、同步抛错、非法 JSON 和缺少工作区标识。
- 本次以共享逻辑回归、路径选择宿主测试、完整 Web 测试、构建和严格 OpenSpec 校验验证；不把 Web 验证等同于已重打包 Desktop 实机验证。

## 交付

变更随重新构建的 Web 资源交付；Desktop 安装包需要按既有流程重新嵌入资源后生效。回滚只需恢复共享函数与相应测试，不涉及数据迁移。
