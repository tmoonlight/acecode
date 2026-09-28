---
generated_from_state_version: 21
---

# 验证

## 当前结果

- 结果: **已归档**
- 验证情况: **已完成检查，验证结果已确认**
- 目标周期: 1
- 迭代: 5
- 验证器尝试次数: 1
- 完成时间: 2026-09-19T05:55:11.199Z
- 摘要: A1-A13 全部通过；实机日志确认主问题修复，日志已精简为安装、最终结果和拒绝/异常，且隐私安全。

## 验收

| 编号 | 结果 | 来源 | 验收项 | 原因 |
| --- | --- | --- | --- | --- |
| A1 | passed | brief.md | Scenario: 输入能力不受修复影响 - GIVEN macOS 桌面端已安装文件拖放处理 - WHEN 用户在 composer 中输入 ASCII、中文输入法文本或粘贴文本 - THEN 输入事件继续由原有 WKWebView/responder 路径处理，修复不使用 `object_setClass` 或改写输入相关类 | 未修改实例 class 或输入 responder，用户实机输入正常。 |
| A2 | passed | brief.md | Scenario: Finder 文件在 composer 实际落点插入一次 - GIVEN native drop payload 含有效释放坐标和至少一个本地路径 - WHEN 坐标命中可用、可见且未被遮挡的 composer - THEN composer 不依赖 DOM hover 时间戳，materialize 路径并只插入一次 | 实际坐标唯一路由 composer 并 materialize；用户已验证单/多文件成功。 |
| A3 | passed | brief.md | Scenario: 非法或错误落点不进入 composer - GIVEN native drop 坐标缺失、越界、命中侧栏/预览/模态遮罩，或 composer 已禁用 - WHEN 前端路由该 drop - THEN composer 不 materialize 文件且记录不含路径的拒绝阶段 | 无效坐标、外部目标、overlay、disabled 均拒绝且有隐私安全诊断。 |
| A4 | passed | brief.md | Scenario: 唯一目标和兼容行为 - GIVEN composer 与 terminal 接收函数同时存在 - WHEN native drop 的实际坐标只命中其中一个目标 - THEN 最多一个目标接收；无坐标 payload、Windows、Linux 和非文件 WebKit 拖放保持既有行为 | 唯一坐标目标，Windows legacy、Linux 和非文件路径保持。 |
| A5 | passed | brief.md | Scenario: 坐标边界与页面缩放 - GIVEN AppKit 使用 view-local point、Retina backing scale 或 WebView 页面缩放 - WHEN native 坐标转换为前端 viewport 坐标 - THEN 命中逻辑使用一致的 viewport 比例并正确处理上下边界和翻转方向 | view-local bounds 归一化、flipped Y、viewport 恢复正确。 |
| A6 | passed | brief.md | Scenario: 运行期间不干扰当前 ACECode - GIVEN 修复、测试和构建正在当前 ACECode 会话中进行 - WHEN Agent 完成自动验证 - THEN 不自动退出、重启、启动或替换任何 ACECode 实例或已安装应用 | 未操作运行实例或安装应用。 |
| A7 | passed | specs/macos-native-file-drop/spec.md | 输入路径保持不变 - GIVEN 文件拖放处理已安装 - WHEN 用户键入、使用输入法组合或粘贴文本 - THEN 文件拖放代码不改写 WKWebView 实例 class，也不接管键盘或文本输入事件 | 仅 class-wide 拖放 swizzle，无实例改类或输入事件拦截。 |
| A8 | passed | specs/macos-native-file-drop/spec.md | composer 接收带坐标 drop - GIVEN payload 含本地路径和有效 viewport 归一化坐标 - AND 最上层元素位于可见、启用的 `.ace-composer-card` 内 - WHEN 前端路由最终 drop - THEN 只调用 composer receiver 一次 - AND composer materialize 并插入路径，不要求 DOM drag hover 已激活 | 最上层 eligible composer 单次接收且不依赖 hover。 |
| A9 | passed | specs/macos-native-file-drop/spec.md | 错误目标被拒绝 - GIVEN payload 的坐标越界、命中 composer 外部、命中覆盖层，或 composer 不可用 - WHEN 前端路由最终 drop - THEN 不向 composer materialize 路径 - AND 诊断只记录坐标有效性、目标和结果，不记录路径或输入内容 | 错误目标不 materialize；同步异常固定枚举、异步失败最终结果均有日志且不含路径、内容或异常正文。 |
| A10 | passed | specs/macos-native-file-drop/spec.md | terminal 与 composer 不重复接收 - GIVEN composer 与 terminal receiver 同时存在 - AND 实际释放坐标只命中其中一个 eligible target - WHEN native bridge 分发 payload - THEN 仅命中的 receiver 被调用 | 互斥分支与异常返回防止重复分发。 |
| A11 | passed | specs/macos-native-file-drop/spec.md | legacy payload 保持兼容 - GIVEN receiver 收到不含坐标的既有路径数组 - WHEN 平台或旧 bridge 使用 legacy callback - THEN 各 receiver 继续使用现有 hover gate，且其他平台行为不改变 | legacy 数组/字符串和 hover gate 保留。 |
| A12 | passed | specs/macos-native-file-drop/spec.md | 归一化坐标跨缩放命中 - GIVEN WebView bounds、backing scale 或页面 viewport 尺寸不同 - WHEN native drop 点转换为归一化坐标并由前端还原 - THEN 左、上、右、下边界使用一致的半开区间规则 - AND 有效内部点命中对应的最上层 DOM 元素 | 两端 [0,1) 边界与缩放转换一致。 |
| A13 | passed | specs/macos-native-file-drop/spec.md | 自动验证不干扰运行实例 - GIVEN Agent 在当前 ACECode 会话中构建和测试候选实现 - WHEN 自动验证完成 - THEN 当前 ACECode 进程与已安装应用未被自动停止、启动或替换 | 验收仅测试、构建和静态检查。 |

## 检查

| 检查 | 命令 | 工作目录 | 状态 | 退出码 | 耗时 |
| --- | --- | --- | --- | ---: | ---: |
| frontend-tests | -lc export PATH="$HOME/.nvm/versions/node/v22.22.2/bin:$PATH"; corepack pnpm@10.11.0 test | web | passed | 0 | 6000 ms |
| frontend-build | -lc export PATH="$HOME/.nvm/versions/node/v22.22.2/bin:$PATH"; corepack pnpm@10.11.0 build | web | passed | 0 | 26584 ms |
| desktop-build | -lc export PATH="/Users/hudy/Desktop/GitHubCode/acecode/.acecode/tmp/session-20260918-145721-6662/build-tools/bin:$PATH"; export VCPKG_FORCE_SYSTEM_BINARIES=1 CMAKE_BUILD_PARALLEL_LEVEL=4 VCPKG_ROOT="$HOME/vcpkg"; cmake --build build/macos-x64-release --target acecode-desktop -j4 | . | passed | 0 | 734 ms |
| diff-check | -lc git diff --check && ! grep -R "object_setClass" -n src/apps/desktop web/src | . | passed | 0 | 796 ms |

### Builder 报告的证据

以下为 Builder 报告，不等同于 Runtime 检查凭据或独立验收结果。

- web pnpm test: passed — 完整 runner 通过，含同步异常诊断守卫
- acecode-desktop compile: passed — x64 target 编译链接通过
- git diff --check and object_setClass guard: passed — 通过
- 已知限制: 日志精简后二进制未再做 Finder 实机验证；用户已验证精简前同一功能路径。

## 阻塞项

_无。_

## 风险与跳过的工作

- 日志精简后二进制未再次 Finder 实机验证，但功能路径未变且精简前用户已验证。
- 同步异常测试为源码断言，源码审查确认逻辑。
- 全缩放/Retina 组合未逐一实机覆盖。

## 之前的迭代

| 目标周期 | 迭代 | 尝试 | 结果 | 未解决项 | 摘要 | 完成时间 |
| ---: | ---: | ---: | --- | --- | --- | --- |
| 1 | 1 | 1 | fail | A1, A3, A7, A9 | 有效坐标路由总体正确，但 macOS 无效坐标错误降级 legacy，违反 A3/A9；需回 Build 修复。 | 2026-09-18T17:48:22.683Z |
| 1 | 2 | 1 | fail | A9 | 无效坐标 fallback 与输入热路径风险已修复，但 composer 内嵌 native overlay 仍可能被误判，A9 失败。 | 2026-09-18T17:56:48.166Z |
| 1 | 3 | 1 | pass | — | 实现级验收通过：坐标必需语义、唯一目标、legacy 兼容、边界、隐私诊断及 composer 内嵌 overlay 拒绝均符合规格；保留实机交互风险。 | 2026-09-18T18:01:20.054Z |
| 1 | 3 | 1 | recovery | — | 用户实机验证日志确认单文件四次与双文件一次均完成 coordinate_valid=true、唯一 composer 路由及 materialize inserted=true，输入事件也持续正常。归档前按用户要求精简诊断日志：保留必要的安装、最终 drop 路由/结果和异常信息，移除重复 entered、bridge、materialize-start 及历史 input/focus 噪声后重新验收。 | 2026-09-19T05:40:08.152Z |
| 1 | 4 | 1 | fail | A9 | 功能与实机证据通过，日志显著精简；仅同步 bridge/router/receiver 异常仍静默，A9 失败。 | 2026-09-19T05:49:35.371Z |
| 1 | 5 | 1 | pass | — | A1-A13 全部通过；实机日志确认主问题修复，日志已精简为安装、最终结果和拒绝/异常，且隐私安全。 | 2026-09-19T05:55:11.199Z |



## 结论

A1-A13 全部通过；实机日志确认主问题修复，日志已精简为安装、最终结果和拒绝/异常，且隐私安全。
