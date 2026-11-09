# ACECode 安卓 App

一个 APK 装上就能用:App 自带一个 Ubuntu 小系统,用 proot 在里面跑原版 Linux 版 `acecode daemon`,
界面是全屏 WebView 打开 daemon 自带的 Web UI。不需要 Root,不需要 Termux,也不需要 NDK。

```
MainActivity(WebView) ──http://127.0.0.1:<随机端口>/?token=…──▶ acecode daemon
        ▲                                                         │ 运行在
RuntimeService(前台服务,常驻通知 + 唤醒锁)                        ▼
        └── DaemonController ── libproot.so ──▶ Ubuntu 24.04(files/rootfs)
                                                 /root ← files/home(bind)
```

## 构建

需要:Docker(buildx,arm64 走 Docker Desktop 自带的 QEMU)、Python 3、JDK 17+、Android SDK(compileSdk 36)。

1. 从包含安卓宿主提示、phone MCP 和 `daemon --require-local-token` 的源码构建对应架构的 Linux 包。先构建 Web 前端，再配置 CMake，使设置入口和触屏布局进入内嵌资源。构建脚本会检查 Linux 可执行文件是否包含手机 MCP 入口和 token 开关。
   布局与发布页的 `acecode-linux-<arch>.tar.gz` 相同。
2. 生成运行环境(每个 ABI 一次;Ubuntu 小系统按 Dockerfile 内容缓存在 `runtime/cache/`):

   ```bash
   python android/runtime/build_runtime.py --abi x86_64 --acecode-tar path/to/acecode-linux-x64.tar.gz
   python android/runtime/build_runtime.py --abi arm64-v8a --acecode-tar path/to/acecode-linux-arm64.tar.gz
   ```

3. 打包:

   ```bash
   cd android
   ./gradlew assembleArm64Release   # 真机
   ./gradlew assembleX64Debug       # x86_64 模拟器
   ./gradlew testX64DebugUnitTest   # JVM 单测
   ```

   `android/keystore.properties`(不入库,字段 storeFile / storePassword / keyAlias / keyPassword)存在时
   release 用它签名,否则用 debug 签名。换签名的包不能覆盖安装。

## 只改前端时的快速预览(调试包)

调试包启动 daemon 前会看 App 内 `home/.acecode/dev-web/index.html` 是否存在,存在就带
`--static-dir=/root/.acecode/dev-web` 用这份前端,不必重编 Linux 包:

```bash
cd web && pnpm build
# 把 web/dist 打成 tar 推到 /data/local/tmp,再用 run-as 解进 files/home/.acecode/dev-web,然后重启 App
```

删掉这个目录就回到内嵌前端。正式包(不可调试)不认这个目录。调试包同时开着 WebView 远程调试,
可以用 `chrome://inspect` 或 adb 转发 `webview_devtools_remote_<pid>` 看页面。

## 网页里与安卓相关的约定

- App 在 WebView 的 UA 末尾加 `ACECodeAndroid/<版本>`,前端用 `web/src/lib/mobileShell.js` 的 `isAndroidApp()`
  识别,隐藏桌面专属项(快捷键、桌面宠物、检查更新、退出、自更新、Agent 浏览器 / Computer Use / iOS 模拟器)。
- 返回键:外壳先执行 `window.__aceHandleBack()`;还有对话框、设置、侧栏浮层、菜单开着就补发一次 Esc
  关最上层,都关完才把 App 退到后台。窄屏设置在分区内容页时先回分区列表。
- 粘贴:手指长按输入框或已选中文字时,全局右键菜单让路给系统的复制 / 粘贴工具条
  (自定义菜单的「粘贴」要读剪贴板,WebView 不给网页这个权限)。
- 密钥输入框旁的「粘贴」按钮:vivo 等手机给密码框弹出禁止粘贴的安全键盘,所以 App 用
  `WebViewCompat.addWebMessageListener` 注入 `aceAndroid` 读剪贴板通道(`ClipboardBridge.kt`),
  只注册给当前 daemon 页面的源、只回主框架,对话里内嵌的可视化小框读不到;前端
  `readTextFromSystemClipboard` 在 App 里优先走它,按钮只在有这个通道时出现。不把密码框改成普通文本框,
  否则输入法会学习、记住密钥。
- 新增 / 编辑模型弹窗在窄屏铺满全屏,分 Provider / 模型 / 高级 三个页签。
- 窄屏(宽度 < 600,与安卓「紧凑宽度」一致)的布局规则在 `web/src/styles/globals.css` 里搜 `max-width: 599px`。

## 目录

| 路径 | 内容 |
|---|---|
| `runtime/pins.json` | Termux 的 proot / libtalloc / libandroid-shmem 版本与 sha256,Ubuntu 镜像名 |
| `runtime/rootfs/Dockerfile` | Ubuntu 小系统里装哪些包 |
| `runtime/build_runtime.py` | 下载并校验 proot、导出小系统、重打 acecode,产物写到 `runtime/out/<abi>/` |
| `app/src/main/java/dev/acecode/android/` | 解压、启动、前台服务、WebView |

## 设计取舍

- **targetSdk 停在 28**:安卓 10 起 targetSdk ≥ 29 的 App 不能执行自己数据目录里的文件,
  小系统里的程序都在数据目录。代价是只能侧载、上不了 Google Play(Termux、dsh-Go 同样如此)。
- **proot 伪装成 `lib*.so`**:只有 `nativeLibraryDir` 里的文件一定可执行;安卓只解压 `lib*.so`,
  所以 `libtalloc.so.2` 改名并原地改写 proot 的 DT_NEEDED(`build_runtime.py` 的 `ELF_RENAMES`)。
- **归档扩展名用 `.tgz`**:AGP 打包时会把 `.gz` 资源解压并去掉扩展名。
- **用户数据在小系统外**:`files/home` bind 到 `/root`,会话、配置、项目都在这里;
  小系统(`rootfs`)和 acecode 分开记版本(`files/runtime-state.json`),升级 App 时只换变了的那份。
- **安全**:安卓上所有 App 共用 127.0.0.1,「本机请求免 token」不成立。App 总是带
  `--require-local-token` 启动 daemon(实现在 `src/apps/web/auth.cpp`),token 和端口存在 App 私有的
  SharedPreferences 里,WebView 用 `?token=` 交给前端。明文 HTTP 只对 127.0.0.1 放行
  (`res/xml/network_security_config.xml`)。
- **硬链接**:安卓禁止普通进程建硬链接,解包时复制;proot 用 `--link2symlink` 模拟运行期的硬链接。
- **通知权限**:安卓 13+ 上 targetSdk 28 的 App 默认没有通知权限,常驻通知(带「停止」)会被藏起来;
  系统补弹的授权框实测不出现,`requestPermissions` 也无效,所以界面第一次就绪时弹一次对话框跳到通知设置。
- **构建脚本用 Groovy**:本机 Gradle 9.2.1 的 Kotlin DSL 脚本连空项目都编不过,App 代码仍是 Kotlin。

## 已知限制

- 没有命令沙盒:bubblewrap 在安卓上用不了,Auto 模式下未知命令会改为弹确认。
- 安卓 12 起系统会杀掉过多的子进程(所有 App 合计约 32 个),长任务里同时开很多 bash / LSP / MCP 可能被杀;
  需要用户在开发者选项或 adb 里关掉这个限制。
- 部分厂商的省电策略仍会冻结后台,需要用户把 App 加进电池白名单。
- 暂不能访问手机共享存储(`/sdcard`),项目放在 App 内的 `/root/projects`。

## 验证记录

2026-11-07 在 android-35 x86_64 模拟器上验证:首次启动约 9 秒(解压 + 起 daemon)到界面;
别的用户不带 token 访问 API 全部 401;带 token 的对话里 bash、git、python3 正常;
覆盖安装只替换 acecode,小系统、会话和项目保留;通知栏「停止」让 daemon 正常退出,「启动」能重新拉起。
同日 OnePlus 9 Pro(安卓 13,内核 5.4 aarch64)真机:首装约 8 秒到界面;别的用户不带 token 访问 API 全部 401;
内置终端里 bash、git commit、python3 正常,起 100 个进程约 0.43 秒。
vivo X Fold(V2170A,安卓 15,折叠屏双显示)真机:USB 安装要在手机上解锁后确认;对话里模型调用(HTTPS)、bash、git、curl 访问外网都正常,一轮约 15 秒;别的用户不带 token 访问 API 全部 401。
2026-11-08 的窄屏适配已覆盖 300 CSS 像素宽的设置页和模型编辑弹窗。该版已装到 vivo X Fold，确认页面宽度为 300、剪贴板通道与返回键通道已加载。密码框实际粘贴仍待使用者复核。
未验证:附件上传、文件下载、长时间后台运行。

## 手机操控与悬浮宠物

在「设置 → 工具 → 手机操控」进入系统无障碍设置，开启「ACECode 手机操控」。在对话中输入“打开手机时钟，切到秒表”。默认权限模式会询问是否允许打开应用和点按控件；切到其他应用后，可在该应用上方直接允许一次或拒绝。关闭服务、锁屏、遇到密码框或受保护的系统界面时，手机工具会返回原因。读屏基于安卓无障碍控件树，屏幕文字可能发送给该会话的模型。

「设置 → 外观 → 悬浮宠物」可选择鲸鱼娘、大眼回形针或 Rover。首次使用需允许 ACECode 显示在其他应用上层。离开 ACECode 后宠物出现，拖动可贴边并记住位置，点按返回 ACECode。选择「关闭」可移除悬浮宠物。

手机操控沿用工具设置的图标行与状态开关；悬浮宠物使用与桌面版同源的角色预览卡，点选即生效。授权未完成时保留目标角色并显示继续授权入口，失败可重试原操作。原生启动状态页与跨应用审批卡复用默认主题的亮暗配色、描边和主次按钮；原生界面跟随系统亮暗模式，暂不实时同步自定义 Web 主题。

手机工具通过 APK 随附的 Python stdio 中继连接原生进程，每次 daemon 启动分配独立的本地 socket，校验调用进程的安卓 UID。普通桌面会话不加载该入口。每次点按都会对照最近的屏幕和控件；界面更新后须重新读屏，操作完成后返回观察结果。

模拟器触屏验收覆盖 301、412 和 659 CSS 像素宽布局。301 像素宽时，顶栏与输入工具点击高度达到 44 像素，权限、模型、推理强度和发送按钮互不重叠。实测输入法弹出时可视高度为 365 像素，发送按钮仍完整可见。文件面板在 301 和 412 像素宽时用抽屉打开，系统返回可关闭抽屉或模型选择菜单。16 个设置分区在 301 像素宽时均未产生横向滚动。

## 接入前的对照记录

2026-11-08 核对当前安卓包时，Android 15 模拟器中 WebView 能识别 `ACECodeAndroid/`。同一页面请求 `GET /api/mcp` 得到空的全局配置，请求 `GET /api/config/computer-use` 得到 `supported: false`。分别在该模拟器和 vivo X Fold 上按包名查询系统无障碍服务，两个已安装的 ACECode 包都未声明此服务。

| 能力 | 接入前的 ACECode 安卓包 | dsh-Go 本机引擎 |
| --- | --- | --- |
| 模型知道自己在哪运行 | 系统提示显示 Linux，shell 在 Ubuntu 中运行。安卓标记用于 WebView 界面适配。 | 开启手机操控后，引擎加载 phone MCP 和使用说明，模型可以调用手机工具。 |
| 打开应用、发起系统操作 | 网页可把外链交给安卓，模型没有用于打开手机应用、设置闹钟的内置工具。 | `apps`、`open` 和 `intent` 对接安卓。定闹钟等系统意图可独立于无障碍服务使用。 |
| 读取并操作其他应用 | APK 提供网页剪贴板和文件选择器。模型不能通过这些接口读取其他应用的屏幕或点按控件。 | 用户启用无障碍服务后，`screen` 返回带编号的控件，`tap`、`type`、`scroll` 等操作可返回新的屏幕状态，`handoff` 可交还用户处理。 |

dsh-Go 在启动本机引擎前配置 stdio MCP，由客体内的中继连接安卓侧服务。手机操控关闭时移除该入口。对照版本为 `4cc476d2`。[引擎接入](https://github.com/ccch1mneyyy/dsh-Go/blob/4cc476d29172745ff00255f1035e5de31737af29/app/android/app/src/main/kotlin/io/github/ccch1mneyyy/dsh_app/phone/PhoneGuest.kt) · [工具定义](https://github.com/ccch1mneyyy/dsh-Go/blob/4cc476d29172745ff00255f1035e5de31737af29/app/android/app/src/main/kotlin/io/github/ccch1mneyyy/dsh_app/phone/PhoneToolCatalog.kt) · [权限与接管](https://github.com/ccch1mneyyy/dsh-Go/blob/4cc476d29172745ff00255f1035e5de31737af29/docs/guide/zh/04-phone-control.md)

2026-11-08 接入后的模拟器验收使用 Android 15 自带 Google 时钟。真实会话调用 `mcp_phone_open`，拒绝失效的控件编号后重新读屏，随后成功调用 `mcp_phone_tap`。工具回读与系统界面均显示 Stopwatch，计时器停在零；另一次通过主界面发送请求，并在时钟上方点击「允许一次」，会话同样完成。

同日 vivo X Fold（V2170A，Android 15）真机验收通过。vivo 的闹钟专用入口要求额外权限；ACECode 通过它识别系统时钟，再打开时钟的普通桌面入口。在默认权限模式下，真实会话用 `clock` 打开时钟并从「闹钟」切到「秒表」，再次读取确认选中状态，计时保持为零。Rover 显示在系统时钟上方，点按后回到 ACECode；300 CSS 像素宽的外屏也验证了文件抽屉和返回。
