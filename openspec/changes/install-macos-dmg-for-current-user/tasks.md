## 1. 当前用户安装入口

- [x] 1.1 增加 `Applications.app` 的原生安装逻辑、Info.plist、图标资源和 ICNS 生成脚本
- [x] 1.2 在 macOS CMake 配置中构建当前用户安装入口并复用严格的用户目录路径策略

## 2. DMG 与发布流程

- [x] 2.1 将 DMG 脚本从系统 `/Applications` 链接切换为签名的 `Applications.app` 拖放目标
- [x] 2.2 在 macOS 发布工作流中构建、处理符号、签名并传入当前用户安装入口

## 3. 契约与文档

- [x] 3.1 更新 DMG 布局和发布脚本测试，断言用户级目标存在且系统链接不存在
- [x] 3.2 更新 macOS 发布文档，说明 `~/Applications` 安装、权限边界和卸载方式

## 4. 验证

- [x] 4.1 运行 OpenSpec 严格校验、Shell 语法与 macOS 打包契约测试
- [x] 4.2 检查完整差异和工作区状态，确认没有修改非目标平台行为

## 5. Applications 目标视觉修正

- [x] 5.1 用 macOS 标准 Applications 文件夹图标替换 ACECode 下载徽标图标，并移除不再需要的自定义图标生成链
- [x] 5.2 更新布局契约并重新运行严格校验

## 6. Finder 拖放交互修正

- [x] 6.1 将 `Applications.app` 改为 Finder 可稳定识别的 droplet，并补齐单文件打开事件处理与来源等价校验
- [x] 6.2 移除直接打开时自动安装和成功确认对话框，确保拖入后无需额外点击即可完成安装并启动
- [x] 6.3 增加 macOS Launch Services 拖放集成检查并接入发布工作流
- [x] 6.4 更新文档与契约测试，完成 OpenSpec 严格校验和完整回归检查
