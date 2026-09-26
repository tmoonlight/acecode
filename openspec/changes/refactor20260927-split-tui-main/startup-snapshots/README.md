# P0-12 启动 conversation 快照

这里保存原始启动代码的观测程序和实际输出。`prepare_probe.py` 不修改生产文件：它验证工作树 `src/main.cpp` 与指定 ref 的 blob 完全相同，在独立构建目录生成一份只增加观测块的翻译单元，通过 CMake 的 project include/defer 在**这一个采集构建**中替换 main.cpp。

观测点位于原 6845 行 `CatchEvent` 创建之前。此前的 TUI 启动步骤真实执行；随后持 `state.mu` 复制前 16 条 conversation 消息，写入 JSON，采集进程退出。该进程专用于观测启动装配，不用于证明绘制、输入交互或关停正确。采集 CMake 参数不可放进正常构建 preset 或发布流程。

## 隔离和场景

- 每次创建全新的 scratch 路径。每个 child 使用自己的临时 `USERPROFILE`、APPDATA/LOCALAPPDATA 与 TEMP/TMP/TMPDIR，working directory 在该 profile 之下；真实用户环境和数据目录不变。
- 配置只含无凭据的 fixture provider、人工编写的两条 resume 消息。不会读取真实 GitHub token、会话历史或配置。
- 新进程有自己的隐藏 Windows 控制台和 stdin/stdout，因此执行原有 TTY 检查。没有通过 UI 输入命令、绕过 TTY 检查或修改业务状态来凑快照。
- `ordinary`：本机 OpenAI-compatible fixture，尚无对话。
- `resume`：`--resume 20260927-000000-0012`；恢复人工 fixture 的 user/assistant 两条消息及原程序追加的提示。
- `copilot-unauthenticated`：无 token 的 Copilot 配置；本机代理让 device-flow 请求保持未完成，采集初始认证提示。不会登录账户。
- `mcp-configured`：配置一个本机 stdio MCP 服务，接收 initialize 但保持响应待定，采集后台启动提示。

代理只监听 `127.0.0.1` 的系统分配端口，不转发流量。关闭远端模型目录刷新；其他启动期 HTTP 请求也留在本机代理。MCP helper 在 probe 退出关闭管道后读到 EOF 并结束。四份 JSON 均由运行时探针写出，Python runner 读取后逐字段校验固定 fixture 并计算 hash，不生成观测消息。

这些快照锁定“尚未收到异步服务结果”这一相同输入条件。B-12 比较时须使用相同配置、resume fixture、消息字段和观测点；必须逐字段比较消息顺序及内容。网络完成后的结果、中文候选窗、首帧显示、各类终端和所有退出路径仍按手工清单验证。

## 重现

在原始 main.cpp 未改写的独立 worktree 中、已经加载 MSVC 开发环境后：

```powershell
python openspec/changes/refactor20260927-split-tui-main/startup-snapshots/prepare_probe.py --repo . --out build-p0-12-probe-input --ref 3ddb7d43
```

用项目通常的 Ninja/Release/vcpkg 参数配置新的 `build-p0-12-probe`，额外指定：

```text
-DBUILD_TESTING=OFF
-DCMAKE_PROJECT_acecode_INCLUDE=<build-p0-12-probe-input/probe.cmake 的绝对路径>
```

```powershell
cmake --build build-p0-12-probe --target acecode --parallel 2
python openspec/changes/refactor20260927-split-tui-main/startup-snapshots/capture_windows.py --executable build-p0-12-probe/acecode.exe --scratch <全新的本机临时目录> --output <全新的采集目录> --source-metadata build-p0-12-probe-input/probe-source.json
```

`capture-manifest.json` 记录源 revision/blob SHA-256、探针 SHA-256、可执行文件 SHA-256、每个进程的退出码、耗时及快照 hash。采集失败、未知结构化 summary/hunks、非零进程退出码或缺输出都使本次采集失败，不回填快照。既有输出不会被静默覆盖。

runner 还逐字段校验四种人工 fixture 的完整结果：普通场景必须为空，resume 必须依次包含两条人工消息和原程序的成功提示，Copilot/MCP 必须各自只含一条初始待定提示。错误恢复、缺失或额外消息、截断计数、未知字段、字段类型变化都拒绝验收；原始观测 JSON 保留，不删除消息、不改写值来满足校验。该校验只适用于此处固定的待定输入条件，不是通用启动文案白名单。

纯函数检查不启动 TUI/GUI：

```powershell
python -B -m unittest discover -s openspec/changes/refactor20260927-split-tui-main/startup-snapshots -p test_capture_windows.py -v
```

## 当前结果

2026-09-27，MSVC 19.38 / Ninja / Release：探针构建 495/495 成功。工作树生产 C++ 与 `3ddb7d43` 一致，`src/` 范围唯一新增文件是分层元数据 `layers.tsv`。

| 场景 | 实际消息数 | 采集退出码 | 独立冷启动复验 |
|---|---:|---:|---|
| [ordinary](ordinary.json) | 0 | 0 | 逐字节相同 |
| [resume](resume.json) | 3 | 0 | 逐字节相同 |
| [copilot-unauthenticated](copilot-unauthenticated.json) | 1 | 0 | 逐字节相同 |
| [mcp-configured](mcp-configured.json) | 1 | 0 | 逐字节相同 |

两轮分别使用全新的配置与数据目录；源、二进制和快照 hash 见 [capture-manifest.json](capture-manifest.json) 与 [repeat-manifest.json](repeat-manifest.json)。没有删掉实际消息、改写消息文本或归一化动态字段。

交叉审查修正了 runner 的 cwd hash：原来在 Python 字符串上 `.lower()` 会把 `É` 改成 `é`，与原 C++ 对 UTF-8 字节进行 ASCII 小写转换的行为不同。新实现先编码再折叠字节，纯 fixture 明确断言两种路径的 hash 分别为 `a7132c056e8f0a63` 与 `b9b81a91d65e7dc3`。已存两轮使用 ASCII 临时路径，四份 JSON 和两份 manifest 保持原字节；全部 9 项纯函数检查通过，并直接验证这些归档观测仍满足严格四场景校验。

串行窗口中额外尝试了实际 `Ω` scratch（本机 ACP936 可表示且存在非 ASCII 大小写差异）。同一个探针在首个 ordinary 场景以 `0xC0000409` 退出，尚未到观测点，没有写出快照；runner 返回 1，后三场景没有运行。启动日志只到第一行，cwd 中的 Ω 为 ACP936 原字节 `A6 B8`；没有堆栈能证明具体 fast-fail 位置，不能将推断写成根因。该失败及原始日志字节另存 [unicode-attempt/result.json](unicode-attempt/result.json) 和同目录，不覆盖既有成功结果，也不声称 Unicode 四场景通过。原 TUI 的 ANSI cwd 行为仍按设计保留，本任务没有修改生产路径逻辑。

随后用修正后的 runner 和同一原始探针，在全新的短 ASCII scratch `p012-ascii-76cf3083` 完整复采四场景。四个进程均退出 0，消息数仍为 0/3/1/1，通过完整字段校验；四份 JSON 与原归档逐字节相同，源、探针和二进制 hash 不变。整个 control 用时 13.125 秒，调用记录、原始输出、manifest 和逐字节比较见 [ascii-control/comparison.json](ascii-control/comparison.json) 及同目录。该结果验证修正后的 runner 对既有 ASCII 场景仍有效，未改变上述 Unicode 失败边界。

首次准备时 Copilot fixture 错误地设置了自定义 base_url，原程序按 managed-provider 规则拒绝配置并退出 1，没有生成该场景快照。修正 fixture 为不指定自定义 endpoint 后，重新从四份全新数据目录完整采集；失败尝试的本机产物仍保留在 `build-p0-12-captures`，未作为成功结果使用。

启动表经过两人语义复核和连续区间检查：82 段无遗漏、无重叠，覆盖原 5259–6845 全部 1587 行。脚本通过 Python 语法检查，累计实际执行了 12 次成功的原始 TUI 启动。生产文件未修改。Linux CI、完整单测通用 gate 与实操终端清单仍未由此记录代替，任务保持未勾选。
