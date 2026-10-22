# Tasks

## 1. 本地化展示数据

- [x] 1.1 增加全部内置工具中英文名称及紧凑对象/必要字段映射；用后端定义覆盖测试和纯函数测试验证别名、文件名、搜索限制与去重。
- [x] 1.2 历史投影恢复所有已配对工具的身份和参数，提供独立结果；验证历史与实时一致且原始数据不变。

## 2. 工具行和详情

- [x] 2.1 接入冒号、文件名和卡片内元数据，保留源码/diff/问答/附件展示；验证运行、完成、失败和仅元数据展开。
- [x] 2.2 精简 Shell 卡片，验证短命令不重复、长命令完整显示、复制完整命令和输出；箭头保持 hover / focus 设计。

## 3. 集成验证

- [x] 3.1 完成双语、宽窄屏、明暗主题及 hover / focus 浏览器验证，保存截图。
- [x] 3.2 通过全量 pnpm test、pnpm build、OpenSpec 严格校验和差异检查，记录实际验证范围。

## 验证记录（2026-10-22）

- 后端工具定义对照：81 个内置工具名称，中英文均覆盖，包含条件注册和动态工具家族。
- `node src/lib/toolPresentation.test.js`：11 项展示/历史回归用例通过。
- `pnpm test`：最终全量运行退出 0；历史恢复用例同步验证参数恢复且原始记录不变。
- `pnpm build`：最终构建成功，ES2020 正则兼容检查通过。
- `node scripts/test-tool-transcript.mjs`：真实 ToolBlock / ActivityLine / ShellCommandCard 的 1280px 浅色和 390px 深色、中文和英文共 4 组通过；额外覆盖 230px 到 1280px 的命令截断恢复。验证复制、失败、运行、仅统计、新建源码、diff、问答和 hover / focus / Enter / Space。
- 最终截图：`C:/Users/shao/AppData/Local/Temp/ace-tool-transcript-fXMG2s/`；文件名格式为 `1280-light-zh-CN.png`、`1280-light-en-US.png`、`390-dark-zh-CN.png`、`390-dark-en-US.png`。
- Impeccable 检测在 UI 实现后执行一次，无发现；OpenSpec 严格校验和所改文件的差异检查通过。
- 验证范围为源码、Web 构建及 Edge 浏览器组件集成；未重建或替换已安装的 Desktop，不宣称安装版已加载修改。
