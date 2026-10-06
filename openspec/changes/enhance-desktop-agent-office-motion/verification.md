# 验证记录

验收日期：2026-10-06，Windows x64，工作区 `N:/Users/shao/acecode.worktrees/agent-office-live`，分支 `claude/agent-office-motion`（基于 master `1f95b89f`）。

| 范围 | 结果 |
| --- | --- |
| C++ 定向 | `SessionActivityState.*`（含 3 条新增：默认模式流式正文、推理与正文分段、工具调用预览）、`DesktopPetLayout.*`（含浮层解析与命中判定 2 条新增）、`DesktopOfficeHttp.*`、`EventDispatcher*`、`RecentFixture.*`、`GlobalSessionCatalog.*` 全部通过。 |
| C++ 全量 fast | `run_fast_tests.py --profile fast`：执行 5001、失败 0、跳过 9。 |
| 构建 | Ninja/MSVC Release 构建 `acecode`、`acecode-desktop`、`acecode_unit_tests` 通过。构建目录 468 个缺少头文件依赖记录的目标文件已删除重编（初次构建 showIncludes 未记录依赖，改头文件后会链接到旧布局对象并在运行时崩溃）。 |
| 静态 | 临时索引含新文件后，分层、所有权 `--strict --final`、行数、文档路径检查均 0 findings；`git diff --check` 通过；OpenSpec strict 通过。 |
| Web | `pnpm test` 全部通过；`pnpm build` 通过；i18n 目录补 18 条英文。 |
| 浏览器 | `web/scripts/test-desktop-office.mjs` 40 项通过、无页面错误与外部请求：常驻气泡、气泡区域上报、回报信封与致谢、出门、走进门领任务、网状消息信封、减少动态效果降级、原有切换/跟随/溢出/尺寸/断线检查。 |
| Windows 原生 | 隔离用户目录 + 本机假模型（OpenAI 兼容流式，无真实模型调用）运行开发版 Desktop，经 WebView2 调试端口采样：主 agent 推理尾巴 → 撰写回复实时文字 → 派发任务、两名成员进门领「任务」信封入座 → 成员思考、流式回复 → 完成后递「回报」信封、主 agent「收到…的结果」→ zzz。窗口扩展样式保持 TOPMOST/NOACTIVATE/TOOLWINDOW；WindowFromPoint 确认气泡可点、透明角落穿透，注入的浮层矩形并入窗口区域、清除后恢复穿透。 |

未执行：macOS 编译与实机验收（本机为 Windows；`desktop_pet_mac.mm` 仅经代码审查）；Linux 保持无桌宠。

## 进出场过渡（2026-10-07 追加）

用户反馈新成员出现太突然、离场要快步出门并道别。改为：门先打开，人在门槛淡入后走进（跨门槛一步约 0.3 秒）；坐下后沿用原桌宠新人入职效果——跳一跳、冒星星、说「报到！」；离开时交完回报说「再见」，约 1.7 倍速走到门口在门槛淡出，门随后关上；减少动态效果时原地「报到！」/「再见」。浏览器自动化 47 项通过；本机假模型驱动的 Windows 原生运行采样确认开门、报到、再见、出门顺序。
