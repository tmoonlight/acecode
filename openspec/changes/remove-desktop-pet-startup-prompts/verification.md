# 验证记录

- `pnpm test`：通过 Web 全量测试。
- `node src/lib/desktopOfficePreferences.test.js`：通过旧壳提示字段过滤、开关与类型选择、原生关闭竞态及失败返回回归。
- `pnpm build`：通过生产构建及正则兼容检查。
- `node scripts/test-virtual-office-preferences.mjs`：通过真实 App 首屏的新安装/升级待提示状态回归，确认没有认领或预览调用；设置、快捷菜单、宠物选择、原生关闭重开、停止轮询、浅色/深色和窄屏回归通过。
- 浏览器证据：`C:/Users/shao/AppData/Local/Temp/ace-office-preferences-3airVG`，包括 `startup-new.png`、`startup-upgrade.png` 及设置页截图。使用隔离浏览器和 API 夹具，不访问用户 daemon。
- `impeccable detect --json web/src/App.jsx`：返回空问题列表。
- `openspec validate remove-desktop-pet-startup-prompts --strict`、`git diff --check`：通过。
- `cmake --build build/windows-x64-msvc --config Release --target acecode_unit_tests --parallel 4`：通过，使用 MSVC x64 开发环境。
- `acecode_unit_tests.exe --gtest_filter=DesktopOfficeServiceTest.*`：9/9 通过，包含缺失/已看过/未看过提示记录、两种启用状态、多次重启不写盘、设置正常保存、关闭重开和失败回滚。原生 XML：`C:/Users/shao/AppData/Local/Temp/ace-remove-pet-prompts-native-tests.xml`。

本次验证覆盖源码、浏览器和共享原生服务；没有替换或重启已安装 Desktop，没有发布版本。macOS 原生界面未在本机运行。
