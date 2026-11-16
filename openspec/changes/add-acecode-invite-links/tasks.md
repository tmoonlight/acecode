# Tasks

阶段划分见 design.md「Migration Plan」：第 1–7 组是阶段 1，第 8 组是阶段 2，第 9 组是阶段 3，第 10 组是阶段 4，第 11 组是阶段 5。每组自带测试和文档；单测按仓库约定写中文注释，写清触发场景和期望行为。

## 1. 模块骨架与邀请链接编解码

- [x] 1.1 在 `src/layers.tsv` 和 `cmake/acecode_layer_libraries.cmake` 登记新模块(`domain/share`、`host/session_share`、`host/peers`)，验证 `check_layers` 和 configure 期守卫通过
- [x] 1.2 实现 `src/domain/share/invite_link.*`：编码、解码、CRC32 校验、版本检查、过期检查、1024 字符上限、从文字中提取(中英文标点、末尾斜杠、百分号编码)，验证 gtest 覆盖每种拒绝原因
- [x] 1.3 新增共享测试向量 `tests/fixtures/invite_links/*.json`(合法、截断、过期、未知主版本、未知键、末尾斜杠、百分号编码、中文标点包裹、显示名含 HTML)，验证 C++ 测试逐个读取并全部通过
- [x] 1.4 实现 `web/src/lib/inviteLink.js`(解码与提取)，用同一组测试向量写 Node 测试，验证 `pnpm test` 通过且与 C++ 解码结果逐字段一致
- [x] 1.5 实现候选地址排序：电脑名优先，物理网卡其次，Hyper-V / VMware / WSL / Docker 等虚拟网卡最后，超长时从尾部截断，复用 `remote_web.hpp::discover_remote_web_hosts`，验证排序和截断的单测通过

## 2. A 侧分享记录与访客鉴权

- [x] 2.1 实现 `src/host/session_share/share_store.*`：在 `<data_dir>/shares/shares.json` 上创建、列出、撤销、过期判定、当日提问计数，走 `utils/state_file` 事务，验证并发写和重启后读取的单测通过
- [x] 2.2 实现 `ShareStore::authorize` 和 Web `require_share_auth`：解析 `X-ACECode-Share`，拆出 share_id 和 secret，常量时间比较，返回只读授权对象，验证错误码区分(缺失、格式错、不存在、已撤销、已过期)的单测通过
- [x] 2.3 在 `auth_result_for_request` 加规则：不是本机直连且远程 Web 未开时拒绝完整 token，验证新单测通过，且现有远程 Web 与 `--require-local-token` 测试不回归
- [x] 2.4 新增越权回归测试：从 `docs/daemon-api.md` 的接口表生成全部现有路由的请求，带分享码逐个请求，验证全部返回 401 且没有副作用
- [x] 2.5 新增主人侧接口 `POST /api/sessions/:id/shares`、`GET /api/shares`、`DELETE /api/shares/:share_id`(dangerous 模式拒绝创建、TUI 主会话不可分享)，验证路由测试通过，并在 `docs/daemon-api.md` 增加「会话分享」一节

## 3. 局域网监听

- [x] 3.1 把 `worker.cpp` 里转发子进程的期望状态改为「远程 Web 开 或 有有效分享」，dangerous 模式下不启动；装配代码拆到单独文件，不让 `worker.cpp` 超过 1000 行；验证用假控制器的测试覆盖四种组合
- [x] 3.2 修改转发端口选择：`remote_port` 为 0 时依次试 12400、daemon 端口加 1、系统分配，验证端口选择单测通过，并同步 `docs/daemon-api.md` 的远程 Web 一节
- [x] 3.3 实现每分钟一次的过期扫描，以及撤销或过期时关闭该分享的全部访客连接，验证撤销后已连接的访客 WebSocket 在 1 秒内被关闭的测试通过

## 4. 只读访客接口与内容投影

- [x] 4.1 实现纯函数 `share_projection.*`：两个档位对每种消息类型(用户、助手、工具调用、工具结果、系统提示、子代理、后台任务通知、附件)的输出，以及全部文本字段过 `redact_secrets`，验证逐类型的单测通过
- [x] 4.2 实现 `routes_share.cpp` 的 `hello`、`messages`、`turns`、`attachments`，会话不在内存时先走 `SessionRegistry::resume`，验证路由测试覆盖分页、换会话 id 被拒、非图片附件被拒
- [x] 4.3 实现 `WS /ws/share/v1` 的订阅、取消订阅、心跳，实时事件走同一个投影函数，其它消息类型一律回错误且不影响会话，验证 WebSocket 测试通过
- [x] 4.4 给访客访问和拒绝加日志(share_id、来源地址、结果)，验证日志捕获测试里没有出现 secret 原文
- [x] 4.5 在 `docs/daemon-api.md` 增加「访客接口」一节，验证 `check_doc_paths` 通过

## 5. B 侧代连与已保存对端

- [x] 5.1 在 `proxy_resolver` 加私有网段和不带点主机名的直连判定，并解析 Windows 系统代理的例外列表(含 `<local>`)，验证单测覆盖 10/8、172.16/12、192.168/16、169.254/16、fc00::/7、fe80::/10、单段主机名和公网地址
- [x] 5.2 实现 `src/host/peers/peer_store.*`(`<data_dir>/peers.json`，同一对端同一会话去重，对外只给分享码末 4 位)，验证单测通过
- [x] 5.3 实现连接流程：按候选地址依次尝试，单个地址 3 秒超时，记住成功的地址，握手检查主版本，验证用本机两个端口模拟对端的集成测试覆盖成功、全部失败、主版本不同
- [x] 5.4 实现 `routes_peers.cpp`(`connect`、列表、删除、消息与附件转发)和 `WS /ws/peers/:id` 中继，只放行订阅和提问两类消息，验证路由测试通过
- [x] 5.5 在 `docs/daemon-api.md` 增加「对端」一节

## 6. 前端：分享、确认与远程会话视图

- [x] 6.1 会话标题栏「属性」左侧加用户指定的向上箭头分享图标，打开「分享」对话框(默认关闭的可交互开关、固定含工具内容和两天有效期、复制邀请、复制链接)，有分享的会话在侧栏显示 18 px 发射天线标记，操作图标与状态标记都通过统一 VsIcon 渲染，按 `acecode-frontend-style` 和 UI 文案约束实现，验证组件测试通过
- [x] 6.2 新增「已存在的链接」列表：主人侧的有效分享(复制、撤销)和访客侧的已保存对端(打开、删除、失效标记)，验证组件测试通过
- [x] 6.3 实现连接确认框(基于 `components/Modal.jsx`)，显示名和标题按纯文本显示，确认后才调用 `POST /api/peers/connect`，验证测试断言确认前没有发出请求
- [x] 6.4 实现只读远程会话视图：复用对话渲染组件，不挂输入框和审批区，不请求本机会话专用接口，`visual` 代码块按源码显示，验证架构测试断言这些限制
- [x] 6.5 输入框粘贴识别：调用 `extractInviteLink`，命中时弹确认，取消后按原文本插入光标处，验证 `composerFileIntake` 相关测试通过
- [x] 6.6 新中文文案先补 `i18n-en-overrides.mjs` 再跑 `pnpm i18n:catalog`，验证 `pnpm test` 和 `pnpm build` 通过
- [x] 6.7 用两个隔离的 daemon(各自 `USERPROFILE` 指向临时目录)在本机做端到端检查：A 分享、B 粘贴链接连接、看到历史和实时更新、A 撤销后 B 显示失效；起好进程后把地址和步骤交给用户自己点

## 7. Windows 协议注册与第二实例交接

- [x] 7.1 实现协议注册决策的纯函数(没有注册、指向不存在的程序、指向本程序、指向另一个存在的安装)和注册表读写，Desktop 启动时调用，验证决策表单测通过
- [ ] 7.2 安装器写入协议注册，卸载器只在命令行指向本次安装时删除，在 `ACECODE_SETUP_TEST_ROOT` 沙盒里验证安装和卸载前后的注册表状态
- [x] 7.3 `DesktopOpenRequest` 增加 `invite` 字段和 `--open-invite` 参数，和现有 workspace/session 请求互斥，验证 `open_request` 单测通过
- [ ] 7.4 `main.cpp` 处理首次启动和第二实例交接：页面就绪前收到的链接先存着，就绪后通过桥接事件交给网页弹确认，验证 Desktop 已运行和未运行两种情况都只出现一个窗口
- [ ] 7.5 在 Chrome、Edge、Firefox 地址栏粘贴邀请链接并回车，验证出现「打开 ACECode」提示，同意后 Desktop 弹出连接确认；再在一台没有注册协议的电脑上确认这三个浏览器都把它当成搜索词(用来核实设计里「没装 ACECode 会被搜索」的风险说明)
- [x] 7.6 更新 `docs/desktop-shell/multi-workspace.md` 的桥接函数清单和 `docs/desktop-shell/design.md` 的 8.6 节

## 8. 访客提问

- [x] 8.1 给 `build_side_chat_toolset` 加「额外拒读判定」参数，作用到 `file_read` 的拦截和 `grep` / `glob` 的结果过滤，验证单测覆盖 `.env`、私钥、证书出现在三种工具里的情况
- [x] 8.2 实现访客 WebSocket 的 `ask_start` / `ask_stop`：走 `SessionRegistry::stream_side_chat`，不提供转发给主会话，流式片段和完整答案都先脱敏，不写主会话历史、不改忙碌状态，验证测试通过
- [x] 8.3 实现额度：同一分享码同时一个提问、每天 30 次、返回恢复时间，额度检查在调用模型之前，验证单测通过
- [x] 8.4 提问记录写 `<data_dir>/shares/<share_id>.asks.jsonl`(每条分享最多 200 条)，新增 `GET /api/shares/:share_id/asks`，前端在分享详情里显示，验证路由和组件测试通过
- [x] 8.5 B 侧可交互远程会话视图加提问入口：流式显示、可停止、额度用完时显示原因；只读链接不显示聊天入口且服务端拒绝提问；中继转发 `ask_*` 消息，验证组件测试和中继测试通过
- [x] 8.6 更新 `docs/daemon-api.md` 的访客接口和对端两节

## 9. 剪贴板识别

- [ ] 9.1 实现剪贴板判定纯函数：带 `ExcludeClipboardContentFromMonitorProcessing` 格式或 `CanIncludeInClipboardHistory` 为 0 就跳过、只看前 8 KiB、提取第一条链接、同一链接只提示一次，验证单测通过
- [ ] 9.2 Windows 宿主窗口注册剪贴板变化监听，前台时直接弹确认，后台或托盘时发系统通知、点击后拉起窗口再弹确认，验证手动检查：从密码管理器复制不提示，复制邀请文字会提示且只提示一次
- [ ] 9.3 macOS 在应用激活时先用 `detectPatterns(for:)` 探测、探测到才读取，`accessBehavior` 为 `.alwaysDeny` 时跳过；在 Mac 上用 `defaults write dev.acecode.desktop EnablePasteboardPrivacyDeveloperPreview -bool yes` 打开隐私提示，验证复制普通文字后激活窗口不弹提示，并实测自定义协议能否被探测到(结论写回 design.md)
- [x] 9.4 新增配置 `desktop.detect_invite_links_in_clipboard`(默认 true，默认值不写入文件)和设置 > 常规里的开关，验证配置读写测试和组件测试通过
- [x] 9.5 新增主题文档 `docs/session-sharing.md`，写清链接格式、三条识别入口、隐私规则，验证 `check_doc_paths` 通过

## 10. AI 读取和询问对端

- [x] 10.1 新增「列出对端」「读取对端会话」「询问对端会话」三个工具，只接受已保存对端的 id，在 TUI、daemon、headless 三处启动时注册，工具表始终注册以保持请求前缀稳定，验证 `RequestPrefixIsByteStableAcrossIterationsInATurn` 不回归
- [x] 10.2 工具结果标明来自外部对端、属于不可信内容，模型侧工具名走 `model_tool_name_for_native`，验证单测通过
- [x] 10.3 在 `docs/session-sharing.md` 补充 AI 工具一节

## 11. macOS 与 Linux 协议注册

- [ ] 11.1 `cmake/macos/ACECodeDesktopInfo.plist.in` 声明 `CFBundleURLTypes`，macOS 宿主接收打开 URL 的系统事件并派发给网页，验证在 Mac 上编译通过并完成一次浏览器打开链接的手动检查
- [ ] 11.2 Linux 打包新增 `.desktop` 文件(`MimeType=x-scheme-handler/acecode;`，`Exec` 用 `%u`，安装后 `xdg-mime default`)，补上 Linux 的第二实例通知(目前是空实现)，验证在 Deepin 包里点击网页上的邀请链接能拉起 Desktop(Linux 的 Chrome 地址栏会把自定义协议当搜索词，不在验收范围内)

## 12. 集成检查

- [x] 12.1 跑 `check_layers`、`check_file_size`、`check_ownership --strict --final`、`check_doc_paths`、`validate_map`，新文件用临时索引加进去再跑，验证全部通过
- [ ] 12.2 两台真实电脑在同一局域网做验收(用户本人)：分享、浏览器粘贴打开、剪贴板识别、提问、撤销、公司代理开启时仍能直连
- [x] 12.3 在 `CLAUDE.md` 加一行索引，链接到 `docs/session-sharing.md`

## 13. 精简分享弹框（2026-11-16）

- [x] 13.1 用默认关闭的「可交互」开关替换权限单选，与生成链接同排；移除内容、有效期控件及旧提示；仅开启时显示圈叹号和指定额度提示，入口改为「已存在的链接」。
- [x] 13.2 新链接固定含工具输出、172800 秒后失效，后端默认只读；拒绝不符合固定规则的创建参数，保留已存在分享的原有权限与时间。
- [x] 13.3 同步图标、翻译、规格和文档，验证组件状态与实际创建参数、原生创建接口、前端测试/构建和真实弹框布局。

## Workflow follow-up

- 两台电脑的验收通过后再归档本变更。
