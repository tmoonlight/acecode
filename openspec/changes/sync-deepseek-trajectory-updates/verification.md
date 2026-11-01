# 验证记录（2026-10-27）

- 本机上游：`C:\Users\shao\deepseek-harness`，HEAD `687f282e86`。检查前后工作树均干净，没有修改上游或拉取远端。
- ACECode：保持当前 master 检出。开始时工作树干净，最终修改限定在轨迹组件、适配模型、测试和同步说明。没有提交、推送或替换安装版。
- `node web/src/lib/trajectoryModel.test.js`：21 项通过，覆盖七轮真实事件顺序、前缀投影、错误请求、回合别名、实时去重、压缩与步骤重试。
- `node web/src/lib/trajectoryInspection.test.js`：7 项通过，覆盖请求重试恢复、工具结束与响应耗时分离、无响应结束、混合附件、代码识别与原文保真、存储受限、50 节点历史窗口。
- `node web/scripts/test-trajectory-turn-order.mjs`：5 组浏览器场景通过，保持原轨迹回合归属与流式记录顺序。
- `node web/scripts/test-trajectory-inspector.mjs`：8 组浏览器场景通过。验证代码行号和原文复制、JSON 独立换行及偏好、紧凑思考标题 13px/20px、重试恢复、附件重复顺序/0 B/原始折叠、等待状态、历史分页/虚拟化/搜索、追加不抢滚动、跨视图跳转展开驻留历史。未捕获页面运行异常。
- 浅色桌面与深色窄窗口截图位于 `%TEMP%\ace-trajectory-inspector`；既有回合回放截图位于 `%TEMP%\ace-trajectory-turn-order`。
- `pnpm test`：最终退出码 0。首次运行出现已有 Agent Browser 等待就绪场景失败；该项单独运行通过，完整重跑也通过，未修改无关模块。最终日志 `%TEMP%\ace-trajectory-pnpm-test-final.log`。
- `pnpm build`：最终通过，3230 模块，产物约 13.28 MB；兼容扫描 4592 个正则字面量，无 lookbehind。日志 `%TEMP%\ace-trajectory-build-final.log`。
- `openspec validate sync-deepseek-trajectory-updates --strict`、`git diff --check`：通过。
- 界面机械检查仅报告原有 5 处轨迹列宽/间距动画建议，本次未扩大视觉重构。保留颜色回退；新遮罩有特性检测；JSON 深色主题接入 ACECode 选择器。

上游全量增量 Session 引擎、专属数据协议及插件框架不在适配范围，详见 `web/src/components/trajectory/deepseek/UPSTREAM.md`。OpenSpec 目录按现有 `.gitignore` 规则保留为本地记录；可随源码交付的来源和适配说明位于该 `UPSTREAM.md`。

本次证明源码、生产 Web 构建和浏览器回放通过；没有重建或重启已安装 Desktop，因此不宣称运行中的安装版已经加载更新。
