# Windows 安装程序(acecode-setup)

自研的 Windows 安装程序,不依赖 Inno Setup、NSIS 等第三方打包套件。界面沿用桌面版启动浮窗的自绘方式(分层窗口 + GDI+),图标动画在启动浮窗时间轴上延伸。只支持装到当前用户目录,不需要管理员权限。

## 组成

| 部分 | 位置 | 作用 |
| --- | --- | --- |
| 安装 / 卸载流程与纯逻辑 | [src/apps/installer/](../src/apps/installer) 里除 `*_win.cpp` 外的文件 | 所有平台都编译,进 `acecode_unit_tests` |
| 自绘窗口与入口 | `installer_window_win.cpp`、`installer_canvas_win.cpp`、`installer_main_win.cpp` | 只属于 Windows 上的 `acecode-setup` 可执行文件 |
| 安装记录 | [src/base/platform/install_record.cpp](../src/base/platform/install_record.cpp) | 安装程序与自升级共用 |
| 打包脚本 | [installer/windows/make_setup.py](../installer/windows/make_setup.py) | 把安装包嵌进 exe 资源区 |

`acecode_installer_support` 只能链 `acecode_desktop_support` 与 `acecode_base_core`(configure 期由 `acecode_assert_no_link_path` 守住),分层规则里 `installer` 是唯一允许依赖 `desktop` 的 apps 模块,用来复用 `splash_animation` 与 `splash_icon_painter_win`。

## 构建

```powershell
cmake --build <build-dir> --target acecode-setup acecode acecode-desktop acecode-computer-use
python installer/windows/make_setup.py --stub <build-dir>/acecode-setup.exe --payload-zip acecode-windows-x64.zip --out ACECode-<version>-windows-x64-setup.exe
```

`--payload-zip` 用 CI 产出的 `acecode-windows-<arch>.zip`;也可以用 `--package-dir` 直接给暂存目录。脚本用标准库 ctypes 调 `UpdateResource` 写入三个 `RT_RCDATA` 资源:

- `ACECODE_PAYLOAD`:安装包 zip;
- `ACECODE_UNINSTALLER`:骨架 exe 的逐字节副本,安装后成为 `uninstall.exe`;
- `ACECODE_PAYLOAD_INFO`:版本、目标平台、两块数据的大小与 SHA-256。

安装包放在资源区而不是拼在文件末尾,签名才不会把它弄坏。签名顺序:先签包内的 exe(含构建中途嵌进 `acecode.exe` 的 `winpty-agent.exe`)→ 打 zip → 签 `acecode-setup.exe` 骨架 → 运行 `make_setup.py` → 签最终的 setup。这样卸载程序副本保留骨架的签名。

链接器默认嵌入 `requestedExecutionLevel=asInvoker` 的清单,文件名带 setup 也不会触发 Windows 的「安装程序检测」自动提权。

## 命令行与退出码

```text
ACECode-<version>-windows-x64-setup.exe [--silent] [--install-dir <目录>]
uninstall.exe --uninstall [--silent]
```

`/S`、`--quiet` 是 `--silent` 的别名。退出码:0 完成;1 失败或取消;2 安装包缺失、损坏或架构不符;3 找不到安装记录;5 ACECode 正在运行;64 命令行有误。日志写在 `%TEMP%\acecode-setup.log`。

## 安装做了什么

1. 默认目录 `%LOCALAPPDATA%\Programs\ACECode`;已有安装(本安装程序或旧 Inno 包)时沿用原目录且不能更改。自选目录不以 `ACECode` 结尾时补一层 `ACECode` 子目录,因为卸载会删除整个安装目录。
2. 安装目录里有 ACECode 进程在跑(桌面版、托盘、daemon、终端里的 `acecode`)时停下,让用户退出后重试。
3. 校验资源区哈希,把 zip 解到旁边的 `<名>.setup-new`,写入 `uninstall.exe` 与安装记录 `.acecode-install.json`。
4. 旧目录改名为 `<名>.setup-old`,新目录改名为正式目录;第二步失败时改回去,所以不会留下半新半旧的目录。旧目录是安装程序或 Inno 装的就删掉,否则(便携 zip 解出来的,可能有用户自己的文件)保留为 `<名>.backup-<日期>`。
5. 写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\ACECode`,创建开始菜单「ACECode」「ACECode 终端」,按选项创建桌面快捷方式、加入用户 PATH(保留原注册表值类型并广播 `WM_SETTINGCHANGE`)。
6. 旧 Inno 安装包装在同一目录时,删掉它的卸载登记和开始菜单分组,`unins000.*` 随旧目录一起删除。
7. 完成页可选填写 ACEModel Key,写入用户数据目录的 `config.json`,合并规则与旧 `seed_acemodel.ps1` 相同。

卸载按安装记录确认目录,撤销快捷方式、PATH 与登记后删除整个安装目录。卸载程序先把自己复制到 `%TEMP%\acecode-uninstall-<pid>\` 再从副本执行;静默卸载中仍在运行的原启动器移到安装目录旁边的 `acecode-uninstall-<pid>-trash`，保证跨盘安装时也能在同一卷内改名。下次针对该位置运行安装或卸载程序时清理残留副本。用户数据目录(会话、设置、技能)两者都不碰。

自升级换完文件后调用 `install_record::refresh_installed_version`,同步安装记录与「设置 > 应用」里的版本号;便携目录没有安装记录,这一步是空操作。

## 界面动画

时间轴在 [installer_timeline.cpp](../src/apps/installer/installer_timeline.cpp),纯函数:

- 欢迎页:与启动浮窗开场逐帧相同,播完后内容淡入;
- 安装中:方砖里的水位随进度上涨(`wave_rise` 大于 1,画笔把水体铺到方砖底边);
- 完成:水退回图标的静止波浪,`>` 再亮一次;
- 失败:水退回、轻震后静止;
- 卸载中:水位随进度退到方砖以下;已卸载:`>` 向右飞出收回,`A` 擦掉,方砖缩小变淡。

## 验证

- 单测:`tests/installer/`、`tests/platform/install_record_test.cpp`、`tests/scripts/make_setup_test.py`(Windows 上会真实调用 `UpdateResource` 并读回)。
- 手动:干净用户下安装 → 开始菜单与「设置 > 应用」出现 ACECode → 新终端里 `acecode --version` → 覆盖安装同版本与新版本 → 运行中安装被拦下 → 卸载后目录、快捷方式、PATH、登记都消失。
- 开发机上先设环境变量 `ACECODE_SETUP_TEST_ROOT=<空目录>` 再运行安装程序:卸载登记与 PATH 改写到 `HKCU\Software\ACECode-setup-test`,开始菜单、桌面与默认安装位置都在该目录下,不会动到本机已有的 ACECode。卸载程序的临时副本会继承这个变量。完成页的 ACEModel Key 写在用户数据目录(由 `USERPROFILE` 推出),测试时把 `USERPROFILE` 也指向沙盒里的目录。

## 发布

`package.yml` 已生成并校验 x64 / ARM64 安装器，两个安装器均纳入发布完整性检查与 SHA256SUMS.txt。独立下载镜像及隔离安装验收见 [Windows 安装器发布](windows-installer-release.md)。

## 尚未做

- 未做代码签名;
- 不支持装到 Program Files(需要管理员权限,也会让自升级无法写入安装目录);
- 不检查、不安装 WebView2 运行时(桌面版已有 Edge 兜底)。
