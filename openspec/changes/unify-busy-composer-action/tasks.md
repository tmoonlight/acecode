## 1. 主动作实现

- [x] 1.1 在纯状态函数加入空草稿停止模式，覆盖空白、附件、阻塞/提交、继续与重试，运行 `node web/src/lib/inputBarState.test.js` 验证。
- [x] 1.2 增加排队和填充停止 SVG 图标、统一 InputBar 的主题色主按钮与点击分派，更新 i18n 目录，运行图标测试并审查按钮只保留一个。

## 2. 综合验证

- [x] 2.1 用真实 Chromium 验证内容切换、停止点击、停止在途、Enter/点击排队、Shift+Enter、附件、空闲/继续、亮暗与窄屏，保存结果和截图。
- [x] 2.2 运行 `pnpm test`、`pnpm build`、OpenSpec strict 验证、前端 detector 和 `git diff --check`，记录结果及 Desktop 未打包验收的边界。

## 验证记录（2026-10-09，Windows）

- `node web/src/lib/inputBarState.test.js`：13 项状态回归通过。
- `node web/src/lib/interfaceIcons.test.js`：通过；`node scripts/regenerate_web_icons.mjs --check`：119 个规范 SVG 通过。
- `node web/scripts/test-composer-actions.mjs`：使用本机 Google Chrome，9 项浏览器检查通过，保存亮/暗主题、1100/390px、停止/排队共 8 张截图及 `results.json`。Playwright 使用系统临时目录的独立依赖，未修改项目依赖。
- `pnpm test`（web）：全量通过，包含既有 IME Enter、防重复提交、队列和空闲重试回归。
- `pnpm build`（web）：通过，3157 个模块构建完成，4519 个正则兼容性扫描通过。
- `openspec validate unify-busy-composer-action --strict`：通过。
- Impeccable detector（InputBar/Icon）：返回 `[]`。
- `git diff --check`：通过。
- 保留原有模型/API/安装脚本等无关工作区修改。未打包、启动或验收 Desktop，本次结果为源码与 Web/Chromium 验证；本机 Windows 输入法实操未执行，IME 保护由既有生产回调回归测试覆盖。
