# UOS / Deepin 专用构建

Deepin 包使用系统 WebKit2GTK **4.0**、Qt **5.11 或更新的 Qt 5**、DTK **5**。DTK 只为现有窗口提供原生圆角和阴影；标题栏、最小化/最大化/关闭按钮、拖动和缩放交互仍由 ACECode 现有实现负责。不会增加 DTK 标题文字、三点菜单或原生按钮。

## 构建开关

在现有 CMake 配置命令后增加：

```bash
-DACECODE_BUILD_DESKTOP=ON -DACECODE_DEEPIN=ON
```

`ACECODE_DEEPIN` 默认 `OFF`，只允许 Linux。开启后 Desktop 才查找和链接 Qt Widgets / DTK Widget，且固定选择 `WEBVIEW_WEBKITGTK_API=4.0`；daemon/TUI 只获得 Deepin 更新标识，不链接 Qt/DTK。普通 Linux、Windows 和 macOS 流水线不开启此选项。

使用发行版匹配的 `qtbase5-dev`、`libdtkwidget-dev`、`libdtkgui-dev`、`libdtkcore-dev`、`libgtk-3-dev`、`libwebkit2gtk-4.0-dev` 和 `libx11-dev`。独立 SDK 可通过 `CMAKE_PREFIX_PATH` 指定。开发库必须与目标系统的 Qt/DTK ABI 匹配。

## 运行条件

- x64、ARM64：归档包含 `acecode` 和 `acecode-desktop`。
- ARMv7：1.0.7 是最后一个 Deepin ARMv7 包（终端/daemon，不含 Desktop）。已下载的版本仍可使用，但不会再有新版本。普通 `acecode-linux-armv7.tar.gz` 按较新的 glibc 构建，不保证能在 GLIBC 2.28 的旧 UOS / Deepin 上运行。
- 桌面依赖系统 `libwebkit2gtk-4.0.so.37`、`libdtkwidget.so.5`、`libdtkgui.so.5`、Qt 5 及 Deepin `dxcb` 平台插件。使用系统随附的对应运行库，归档不捆绑 Qt、DTK 或 WebKit。
- 原生窗框效果还要求 `XDG_CURRENT_DESKTOP` 或 `XDG_SESSION_DESKTOP` 含有 `Deepin` 桌面名称，并且窗口实际使用 GDK X11 后端。XWayland 满足这一后端条件，CPU 架构不参与判断。原生 Wayland 跳过 X11 窗框适配。
- Deepin 的 DPI 修正也受编译开关和上述运行条件限制；有效字体 DPI 大于 96 且缩放参数有效时启用，显示设置改变后实时刷新。
- 这些设置只作用于 ACECode 进程，不修改系统显示设置。
- 专用版本在 GTK 创建窗口之前初始化 X11 线程支持，避免旧版 Xlib 在 GTK 与 Qt 混用时因线程锁初始化过晚而崩溃；初始化失败时保留 GTK 窗口，不启用 DTK。

## 打包与升级

下载文件为 `acecode-linux-deepin-x64.tar.gz` 和 `acecode-linux-deepin-arm64.tar.gz`，替代原 `linux-old-*` 系列。普通 `acecode-linux-*` 包继续单独发布。

专用流水线保持 Debian Buster / GLIBC 2.28 基线。`scripts/build_deepin_dtk_sdk.sh` 从固定提交及 SHA-256 校验的源码构建 DTK 5.2 SDK，避免系统仓库更新 Qt 后抬高 ABI 要求。SDK 只用于 CI 链接，不放进发行包。流水线校验 GLIBC 上限、WebKitGTK 4.0、DTK 5 和动态依赖是否齐全。打包前及解包后运行 `scripts/verify_deepin_package.py`，拒绝归档中的 Qt/DTK 运行库和 Qt 插件。

Deepin 更新标识为 `linux-deepin-<arch>`。当前发布流程提供手动下载的 Deepin tar.gz，尚不发布对应自动更新 ZIP；检查更新时不会退回普通 Linux 更新包。请下载同架构 Deepin 新版，退出 ACECode 后替换完整目录。
