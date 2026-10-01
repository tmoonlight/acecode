# Microsoft Store 验证与交付记录

日期：2026-09-30（Asia/Taipei）。当前为本地准备完成、等待账号资料；尚未提交认证。

## 产物

- 项目版本：0.9.30；商店包版本：1.9.30.0；Windows x64。
- 本地测试包：build/store-msix/local-test-26brfbee/ACECode-1.9.30.0-x64-LOCAL-TEST.msix。
- 大小：20,828,073 字节。
- SHA-256：4a2facdcd0e862a8979549be1e7ba2ea5a7e8178b6e257ecbef0a0611c54b699。
- 产品身份：ACECode.LocalValidation；Publisher：CN=ACECode Local Validation。
- Package family：ACECode.LocalValidation_c4qemqcy5et8t。
- 精确源码基线、未提交修改和全部文件哈希：同目录 package-report.json。
- 该身份仅供本地测试，不能作为正式商店包提交。

## 自动化验证

- OpenSpec publish-microsoft-store-msix 严格校验通过。
- Python 打包单测 5 项通过：身份必填、XML 转义、版本映射、CLI/full-trust 声明和架构拒绝。
- pnpm test、pnpm build 通过；更新状态行为测试覆盖旧 ZIP 任务不会恢复为安装操作。
- Windows Release 增量构建通过，复用 build/refactor-phase1-windows。
- 原生定向测试：194 项、39 个测试套件全部通过，包含升级、反馈、CLI 相关回归。
- MakeAppx 语义校验通过，归档每个 payload 文件逐项 SHA-256 核对通过。
- seed 111 个文件与源目录一致。
- layer-lint：R1–R14 零问题。
- git diff --check 通过。

## Windows 包身份实测

使用本机原已启用的开发模式注册最终 payload/AppxManifest.xml，没有新增证书信任或修改开发模式。它验证真实 Windows 包身份和执行入口；它不等于商店签名归档安装验收。

- acecode-store-test.exe --version 返回 0.9.30。
- --validate-models-registry 加载包内 share/acecode/models_dev/api.json：225 providers、8276 models。
- upgrade 与 update --force 均返回非零及 Microsoft Store 更新说明。
- 包内 daemon /api/update/status 返回 store_managed、update_available=false，无 ZIP/manifest URL。
- /api/update/start 返回 HTTP 409，未创建自更新任务。
- 本地监听器记录自更新请求数为 0。
- Desktop 与其相邻 daemon 的真实进程路径均位于测试包 payload。
- WebView 内通过实际菜单打开检查更新，简体中文和英文均显示商店说明；没有“立即升级”或“已是最新版本”的误导状态。
- 反馈 API 对 HTTP 地址返回明确 HTTPS 要求，上传前拒绝。
- 测试 Desktop 正常退出，测试包注册已移除；原有普通安装 Desktop/daemon 保持运行。
- 运行证据：runtime-report.json、feedback-runtime-report.json，以及隔离 desktop-profile 下的本次日志（仅供本地验证，不提交商店或 Git）。

## 截图

最终测试包目录中：

- store-desktop-zh-CN.png：中文桌面主页。
- store-desktop-en-US.png：英文桌面主页。
- store-update-zh-CN.png：中文商店更新说明。
- store-update-en-US.png：英文商店更新说明。

截图来自独立测试配置，无模型密钥、个人会话或真实项目内容。正式产品身份到位后可重新采集最终发布截图。

## 发布前必须完成

- 账号生成的 Name、Publisher、PublisherDisplayName、Store ID。
- 发布主体、支持/隐私联系渠道、公开 HTTPS 隐私政策 URL。
- 可用 HTTPS 反馈服务及保存、删除规则；当前 HTTP 配置在商店版会被拒绝。
- 审核人员可使用的模型/账号测试方式。
- 真实身份正式包；商店签名包的安装、版本升级和卸载验收。
- Partner Center 提交、认证结果、公开商店页面及实际安装验证。

微软商店后台没有当前会话可用的连接。启动系统浏览器的操作被自动审批拒绝（仅返回 blocked by policy）；已向用户提供后台链接，由用户登录并取得账号标识。未使用其他方式绕过该拒绝。
