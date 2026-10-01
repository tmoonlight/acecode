## Context

现有 Inno Setup 安装器将 Desktop、CLI 和 Computer Use 放在同一目录，后台通过相邻 CLI 启动。模型目录和 seed 使用 share/acecode；普通升级会在安装目录建立 runner 并替换文件。已有 Windows SDK 10.0.26100.0 和可复用的 Release 构建。用户已选择 MSIX；账号产品尚待创建。

## Goals / Non-Goals

**Goals:** 以当前源码构建可核验的 x64 MSIX，保留完整桌面和命令行能力，明确分离 Store 更新路径，提供真实提交材料。

**Non-Goals:** 不改为 UWP/AppContainer，不把普通 ZIP/Inno 安装用户迁移到 MSIX，不触发其他平台发布，不将本地测试包当成已获商店认证。

## Decisions

1. 使用 Windows SDK MakeAppx 和显式 manifest 直接打包原生 Win32 产物。无需引入新的 UI 框架或转换安装器。应用使用 full trust，声明 Desktop 可见入口和 CLI AppExecutionAlias。
2. 打包参数包含 Product identity 的 Name、Publisher、PublisherDisplayName。正式模式缺失任一字段立即失败；单独的 local-test 模式使用显著测试身份，产物名和报告均标记不可提交。不收集账号密码或私钥。
3. 使用四段商店版本，默认将项目 a.b.c 映射为 (a+1).b.c.0，保证当前 0.x 版本符合商店首段大于零约束；版本映射记录在报告，后续升级保持同一规则。
4. 完整收集发布所需可执行文件、相邻依赖 DLL/WinPTY helper、models.dev、seed、许可说明。根据 PE 头核对架构，禁止默认包含整个 build 目录、PDB、私有配置或旧浏览器扩展。
5. 运行时从 Windows 包身份判断更新归属，不依赖可变环境变量或文件名猜测。在公共检查和升级入口网络请求之前短路。API 返回 store_managed，前端沿用现有更新弹窗显示商店说明，不误报最新版本或可安装更新。普通安装继续原逻辑。
6. 数据继续位于用户目录，不在包目录写配置。打包后验证资源解析、命令行入口和 Desktop/daemon 启动。MSIX 安装/升级/卸载实测与 MakeAppx 结构校验分别记录。
7. 商店素材使用既有品牌图标和真实界面截图。隐私文本以源码数据流为依据；发布者联系信息和隐私政策公网 URL 待账号资料补齐后确认。首次模型接入说明和 full-trust 用途写入审核备注。

8. 源码核对发现反馈复用默认 HTTP 升级服务。商店版上传前复用包身份检测，强制 HTTPS；未配置时明确报告未上传，普通安装兼容原有协议。正式可用的反馈端点仍需发布者提供，不擅自替换服务器地址。

9. MSIX 别名实测暴露 argv[0] 指向 WindowsApps 别名而非程序目录，导致模型资源无法解析。仅对包身份进程复用系统进程路径查询取得 CLI 安装目录；普通安装保持原路径行为。

## Risks / Trade-offs

- 缺少账号身份 → 先生成测试包，正式包和提交任务保持未完成。
- 本机 MSIX 安装可能需要开发模式或测试信任 → 先检查现状，不自动降低系统安全设置；无法安装时记录未验证。
- Desktop 单实例和已有 daemon 可能混用普通安装 → 使用隔离用户数据与独立端口验证，核对实际进程路径。
- 缺少 WebView2 或外部开发工具 → 在干净环境验证启动兜底，商店资料明确网络和所选工具依赖。
- 商店审核判断 full-trust/AI 服务可测试性 → 提供权限说明及专用测试方式；不承诺审核结果。

## Migration Plan

先完成定向测试、Web 全量测试与构建、Windows 原生增量构建，再制作并检查包。真实产品身份到位后重新构建正式包，补齐联系和隐私 URL，提交并跟踪审核。回滚时停止新包分发；用户数据不随打包脚本删除。

## Open Questions

- 账号生成的产品身份、Store ID 和公开支持联系方式。
- 隐私政策的正式托管地址及审核测试账号/模型方式。
