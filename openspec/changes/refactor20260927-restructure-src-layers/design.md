# Design: refactor20260927-restructure-src-layers

> **行号基准**:本系列文档中的 `文件:行号` 都基于 master `7942011b`,即 2026-09-26 的调研快照。开工前请按当时的代码重新核对;行号漂移不影响任务的边界。
>
> **本文同时是 refactor20260927 系列的总纲**:第 1–5 节讲分层与搬迁(本变更自身),第 6 节是提交与协作约定,第 7 节是验收闸门,第 8 节是系列路线图(跨 change 依赖与二期待办),第 9 节是决策登记。
>
> 旧路径到新路径的完整映射表见同目录的 [layout-map.md](layout-map.md)。

## Context

现状(调研实测,细节见 proposal.md - Why):

- src/ 下有 40 个平级目录,另有 7 个根级散文件。
- quoted include 共 2776 行,其中 `../` 相对写法 1124 行,分布在 366 个文件里(`../../` 占 173 行);同目录裸名 1004 行;只有约 600 行已经是根形式。tests/ 基本已是根形式:`dir/x.hpp` 1281 行,根级裸名 77 行(`permissions.hpp` 38、`agent_loop.hpp` 30、`tui_state.hpp` 7、两个 guard 头各 1),另有 16 行 `../` 与 37 行同目录 include 指向测试 helper。
- CMake 用 `GLOB_RECURSE src/*.cpp` 收集源文件,再用正则 `"/src/(tui|markdown)/"` 把 TUI 源剔出 `acecode_testable`。此外还有 5 份显式清单(NATIVE_BRIDGE 33 个、DESKTOP_SUPPORT 19 个、DESKTOP_BINARY_ONLY 7 个、NOTIFICATION 2 个、TESTABLE_TUI 41 个)、两处 `set_property(SOURCE)`(`upgrade/manifest.cpp` 的 `ACECODE_DEEPIN`、`channels/bridge.cpp` 的 `ACECODE_CHANNEL_ASSET_DIR`)、OBJCXX 属性和 11 个 `.mm`。**目录一搬,正则失配后 TUI 源会被静默编进测试库;testable 本来就链 ftxui,构建照样通过,边界就这样悄悄被打穿**——这是最危险的静默点。
- 7 个测试用固定层数的 `parent_path()` 推导仓库根;`channel_boundary_guard_test` 和 `bridge_test` 在路径失效时会空转或跳过。搬迁后它们会**静默失效**。
- 行尾:工作树里 447 个 CRLF 文件、443 个 LF 文件、87 个混排文件,索引里全是 LF。
- 并行分支:51 个 worktree、72 个本地分支,没有打开的 PR。28 个旧 worktree 停在 filter-repo 重写之前,都已与 master 补丁等价;真正带独有 src 改动的 ref 只有 9 个,每个最多 16 个 src 文件。风险主要是冻结窗口期间其它会话还在从旧 master 分叉改代码。

约束:

- 行为保持:CLAUDE.md 记录的不变量(见 §7.3)一律守住。
- 平台:Windows(MSVC)、macOS、Linux x64/arm、Deepin(GCC 8.3 容器)都要能构建。
- 多代理并行执行:本系列由多个代理协作完成,因此必须有认领、互斥和提交约定(§6)。

## Goals / Non-Goals

**Goals:**
- 目录层次一眼可见:同一级的目录颗粒度一致,组名本身就表达依赖方向。
- 分层可被机器检查,新增的反向依赖在 CI 上直接失败。
- 搬迁过程可重放、可验证、可回滚:脚本生成、R100 纯改名、target 快照逐元组比对。
- 对并行分支友好:整目录改名保住 git 的重命名跟随;提供自助迁移脚本。

**Non-Goals:**
- 不改任何运行期行为,也不修调研中发现的 bug(疑似 bug 归二期 P8)。
- 不做模块内部的再分组。例如 `tool/` 内部的 fs/shell/MCP 分组、`session/` 内部的持久化与 API 契约拆分,都留到以后。
- 不在本变更里拆巨型文件(由 split-agent-loop / split-tui-main 负责),也不做所有权整改(由 adopt-ownership-conventions 负责)。

## Decisions

### D2. 目标分层:6 个分组,组名即依赖方向(已定)

2026-09-27 用户拍板,原方案里的 `capability` 组更名为 **`adapters`**。

```
src/
├── layers.tsv        唯一事实源:路径前缀 → 模块 → 分组 → rank → 禁止规则 → 例外;lint 与 CMake 共用。src 根下只允许这一个文件
├── base/        L0 基础与平台:不含 ACECode 业务流程;桌面壳只允许依赖这一组
│   ├── utils/        编码、路径(含 get_*_dir)、日志、原子写、哈希、diff、frontmatter、semver;
│   │                 RAII 原语 joining_thread / lifetime_token / scope_exit / abandonable_call / abort_signal
│   ├── image/        图片探测与归一化(stb 移到仓库根 external/stb)
│   ├── platform/     OS 封装:process/(process_runner、piped_process、which、os_process、unique_handle、file_lock)、
│   │                 terminal/、native_ui/(文件夹选择、通知、toast、strings)、clipboard、open_url、power_inhibitor、
│   │                 locale、utf8_command_line
│   ├── config/       AppConfig 读写与恢复、saved_models、mcp_config、builtin_model_catalog、models_dev_catalog(纯部分);
│   │                 vocab/ 放零依赖枚举:permission_mode、theme_id、pointer_appearance
│   ├── network/      proxy_resolver、tcp_probe、http(原 upgrade/http)
│   ├── ipc/          跨进程文件协议:runtime_files、guid、open_request、daemon_protocol、agent_browser_runtime
│   ├── workspace/    workspace_registry、files_handler(纯函数)
│   ├── pty/          原 web/pty 整个目录
│   └── environment/  工具链探测、终端解析、shell 命令行、数据目录迁移与写闸门
├── domain/      L1 领域核心:数据、策略、注册表、持久化。不调模型、不执行工具、没有 UI
│   ├── llm/          对话协议根:llm_provider.hpp、tool_result.hpp、tool_protocol_names、model_family、tool_icons、
│   │                 token_estimate、message_predicates、context_thresholds、context_usage、text_preamble_tags
│   ├── permissions/  PermissionManager、path_validator、shell_write_guard、default_rules、interaction_mode(原 headless_mode)
│   ├── security/  sandbox/  experts/  project_instructions/  memory/  history/  connectors/
│   ├── hooks/        + hook_seeds
│   ├── skills/       + opencode_command、skill_command_expander、default_skill_startup
│   ├── worktree/     只保留纯 git 操作
│   ├── gitinfo/
│   └── session/      会话持久化、会话 API 契约(session_client、event_dispatcher、prompter)、附件、thread_repair、
│                     各类 store、tool_result_storage、token_tracker、session_title_text、composer_attachments
├── adapters/    L2 适配层:对接外部系统的实现,包括模型服务、工具执行、LSP、升级与主题服务等
│   ├── upgrade/  themes/  feedback/  computer_use/  lsp/(+ lsp_status_text)
│   ├── pa/           PA 内网兼容,保持可整块删除;+ pa_rescue_driver
│   ├── provider/     OpenAI/Anthropic/Copilot/Codex/Grok、auth/、codex/、retry_policy、模型与上下文窗口解析、
│   │                 SessionModelBinding、models_dev_catalog_cache
│   └── tool/         ToolExecutor、tool_rewrites、内置工具、MCP(mcp_manager、mcp_runtime)、web_search/、
│                     agent_browser/、image_generate/、workspace_tools、safe_text_write、file_state_restore、question_policy
├── engine/      L3 单会话回合引擎
│   ├── tool_preamble/  prompt/(+ init_prompt、prompt_environment)
│   └── agent/        AgentLoop 门面 + request/ turn/ worker/ control/ transcript/ approval/ boundary/ hook_bridge/
│                     progress/ model_step/ recovery/ compaction/ goal/ side_question/ tool_exec/ guards/
│                     event_payload/ detail/(子目录由 split-agent-loop 形成)
├── host/        L4 多会话宿主与后台服务
│   ├── session_host/ SessionRegistry、LocalSessionClient、ThreadService、TaskSuggestionService、自动标题、
│   │                 apply_model_to_session;tools/(spawn_subagent、thread、task_suggestion 三类编排工具)
│   ├── loop/  remote_control/  channels/        (后两者自带 Crow 小服务,已在 R7 登记)
│   └── app_runtime/  三入口共用组合根(二期预留,本期不创建)
└── apps/        L5 界面与进程入口:tui / web / headless / desktop 四者互不依赖
    ├── cli/          acecode 可执行入口:main.cpp(≤80 行)、command_dispatch、pre_tui_commands、interactive_options、
    │                 configure/、channels_cli
    ├── tui/          app/ render/ input/ overlays/ chat/ composer/ term/ model/(不含 ftxui 的纯逻辑)
    │                 commands/ markdown/ settings/ resume/ path_reference/;tui_state.hpp
    ├── web/          Crow HTTP/WS:handlers/ routes/ server*,内部不动
    ├── headless/  daemon/(cli、worker、service_win、heartbeat、startup_diagnostics)
    └── desktop/      桌面壳

tests/<module>/       按模块名镜像,不加分组层;测试 helper 统一放 tests/test_support/<area>/
external/stb/         vendored stb 头
cmake/version.hpp.in  生成的头名 generated/version.hpp 不变
```

**rank 全序**(以 `src/layers.tsv` 为准,只能 include rank 更小的模块,或者本模块内部):

| 分组 | 模块(rank) |
|---|---|
| base | utils 0 < image 1 < platform 2 < config 3 < network 4 < ipc 5 < workspace 6 < pty 7 < environment 8 |
| domain | llm 10 < permissions 11 < security 12 < sandbox 13 < experts 14 < project_instructions 15 < memory 16 < history 17 < connectors 18 < hooks 19 < skills 20 < worktree 21 < gitinfo 22 < session 23 |
| adapters | upgrade 24 < themes 25 < feedback 26 < computer_use 27 < lsp 28 < pa 29 < provider 30 < tool 31 |
| engine | tool_preamble 40 < prompt 41 < agent 42 |
| host | session_host 50 < loop 51 < remote_control 52 < channels 53 < app_runtime 54(二期预留) |
| apps | tui 60 < web 61 < headless 62 < desktop 63 < daemon 64 < cli 65(apps 内另有平级规则 R5) |

**为什么这样分**

- 用户要求同一级的颗粒度一致。6 个分组正好对应 6 种粒度:基础库 → 领域数据 → 对外适配 → 单回合引擎 → 多会话宿主 → 界面入口。具体模块都放在组内。
- 三套备选方案比选后,胜者是「最小搬动」。它的核心机制是:旧目录大多 1:1 整体改名挂到组下,模块名不变。git 的目录重命名检测没有歧义,并行分支对旧路径的修改和新增文件都能自动跟到新位置。在此基础上嫁接了「严格分层」的两样东西:组名表达方向,以及守卫体系(R3/R6/R7/R11)。
- rank 取的是「需要切断的边最少」的全序。关键链是 `llm < … < session < … < provider < tool`:provider→session 的 12 条边、tool→session 的 16 条边按这个顺序本来就合法。原型脚本在 977 个文件、2757 条 include 边上跑出 0 违规。**但这是在草案命名、只检查 rank 与平级规则的条件下得到的**。P0-03 用正式 `src/layers.tsv` 与初版映射表实测:1473 个 src/tests C++ 文件、2753 条可解析的 src 内 include 边,共 **1326 项违规**(同一行触发不同规则分别计数):R1=18、R2=11、R3=5、R5=6、R8=1267、R9=10、R10=1、R14=8;R6/R7/R11/R12/R13=0。原设计没有定义 R4,工具明确将其保留。这里是 P0–P2 的迁移模式:当前文件按映射表获得正式模块归属,并对当前 include 文本与目录写法检查;冻结后的 final 模式另外禁止旧根回流。原始结果与采集范围见 [baseline/p0-tools/README.md](baseline/p0-tools/README.md),不能把这次源码基线当作四平台 G0 构建验收。
- P0-03 行数实测修正:当前有 **38** 个超过 1000 行的 src 文件,其中 web 4 个与 stb 3 个按约定豁免,其余 **31** 个全部进入行数棘轮;不沿用任务中的「37」估计。include 调研则精确复现 **1124 行 `../`、366 个文件**;加上子目录相对写法,src 规范化共涉及 1152 行、370 个文件。P1 只启用 `--enforce-parent-includes`,完整 R8 的裸根头与 version 歧义检查随 P2/P3 收紧。
- 已知的妥协(不作为本期问题处理):
  - include 里看不出分组;
  - 组内的 rank 全序不等于真实 DAG,比如 upgrade/themes 排在 session 之上只是为了线性化;
  - domain 内的 14 个模块规模悬殊;
  - `llm_provider.hpp` 整头下沉,编译依赖并没有变轻;
  - `engine/agent/event_payload` 让引擎知道 Web 载荷的格式。

### D1. include 根:6 个分组目录各自作为 include 根(已定)

- CMake 新增 INTERFACE 目标 `acecode_include_roots`,包含 `src/base`、`src/domain`、`src/adapters`、`src/engine`、`src/host`、`src/apps`,外加 `${CMAKE_BINARY_DIR}/generated`。`external/stb` 以 SYSTEM include 的形式只挂在 base 上。所有目标删掉原来的 `${CMAKE_SOURCE_DIR}/src` 根,改为链接 `acecode_include_roots`。
- 优点:冻结时整目录改名,include 字符串一个字都不用动,M1 能做到全部 R100。
- 代价:include 里看不出分组;模块名必须全局唯一;子目录不得与模块同名。这两条都由 R9 lint 强制,否则会出现多根下先命中者静默胜出、MSVC 按父包含者目录查找造成的平台差异。
- 备选方案是单一 `src/` 根(`"domain/session/x.hpp"`)。没选它,因为每次搬迁都要改全部 include 字符串,冻结提交无法做到 R100,对并行分支最不友好。

### 依赖规则 R1–R15(由 `scripts/layers/check_layers.py` 逐条检查)

1. **R1/R2 方向**:分组顺序为 `base < domain < adapters < engine < host < apps`。每个模块有一个 rank,只能 include rank 更小的模块,或者本模块内部的文件。
2. **R3 语义禁止**:rank 合法,但语义上不该有的边。
   - domain 不准引用 `<cpr/...>` 或 provider/*;
   - tool 与 provider 不准引用 feedback/* 或 upgrade/*,`theme_create_tool` 对 themes 的依赖登记为允许;
   - pa 只允许被「PA 接触点表」里的文件引用(表在 split-agent-loop design.md,与 R11 共用一份);
   - tool_preamble 只允许 `engine/agent/progress` 引用;它的标签剥离器已下沉到 `domain/llm/text_preamble_tags`。
3. **R5 apps 平级**:tui、web、headless、desktop 四者互不 include。daemon 可以依赖 web;cli 可以依赖 tui、daemon、headless、web。「谁创建谁、初始化与关停顺序」只允许写在组合根里,即 apps/tui/app、apps/daemon/worker、apps/headless/headless_runner、apps/cli,以及二期的 host/app_runtime。
4. **R6 收窄**:
   - apps/desktop 只能依赖 base 组,把「acecode-desktop 不链 acecode_testable」固化为规则;
   - `config/vocab` 只能 include 标准库;
   - `apps/tui/model/` 禁止 `<ftxui/...>`;
   - `adapters/computer_use` 里 helper 可执行文件的源(helper_main*、native_*、macos_*.mm、pointer_*)只能依赖 base 组与 computer_use 契约头。
5. **R7 第三方封闭**:
   - ftxui 只在 apps/tui、apps/cli;
   - Crow 只在 apps/web、apps/daemon、host/channels、host/remote_control;
   - webview 只在 apps/desktop;
   - cpp-mcp 只在 adapters/tool;
   - winpty 头只在 base/pty;
   - stb 只在 base/image。
6. **R8 include 写法**:项目头只能写 `"<模块>/…"`,或同目录裸名。禁止 `../`,禁止带分组前缀(如 `"domain/session/x.hpp"`),禁止用尖括号引项目头。每条 include 在「包含者目录 + 6 个根 + generated + tests 根」中必须恰好解析到一个文件。
7. **R9 命名**:
   - 模块名全局唯一;
   - 任何子目录名都不得等于模块名或分组名,所以 agent 下用 `hook_bridge/` 而不是 `hooks/`,tui 下用 `term/` 而不是 `platform/`;
   - 分组目录下只能有模块目录,不能直接放文件;
   - src 下禁止出现名为 `version.hpp` 的文件。
8. **R10 归属**:src/ 与 tests/ 下每个 `.cpp/.hpp/.h/.mm` 都必须落在 `layers.tsv` 登记的模块里。冻结后旧目录(如 `src/session/`)里再出现文件,lint 直接失败,专门拦截旧分支把文件带回来、又被 GLOB 静默编译的情况。
9. **R11 单出口白名单**:以下符号只能出现在登记过的文件里。
   - `model_facing_provider_messages` 的定义与 provider 消息构造;
   - 工具审批决策;
   - `audit_gate` / `record_audit`;
   - `set_model_tool_name_mappings`(tool_rewrites 与 tests);
   - `AgentLoop::messages_` 的写入;
   - PA 判定符号;
   - engine/agent 内的 `chat` / `chat_stream` 调用点,按实际位置登记:provider_stream_collector、side_question_service、side_question/side_chat、compaction/compact、PA rescue。

   具体文件由 split-agent-loop 落地后回填 `layers.tsv`。
10. **R12 行数棘轮**:新文件不超过 1000 行;存量超限文件写入 `scripts/layers/size_baseline.txt`,行数只能下降。`src/apps/web/**` 标注「用户约束豁免」,`external/stb` 标注「第三方」,都不计入基线。
11. **R13 例外到期**:例外必须在 `layers.tsv` 的 exceptions 段登记 from、to、owner、到期日,到期未清理 lint 就失败。P2 的转发头一律登记在这里,冻结当天到期。
12. **R14 tests**:tests 视为 rank 100,可以 include 任何层;目录必须镜像模块名;测试 helper 只能放 `tests/test_support/`。
13. **R15 所有权棘轮**:由 `check_ownership.py` 统计,src/apps/web 豁免。统计五类指标:裸 `new/delete`、`.detach()`、`std::thread` 成员或局部变量、存进长寿对象或跨线程的回调里的 `[&]`/`[this]`、`set_*(T*)` 延迟注入。数量只能下降。目标值见 adopt-ownership-conventions。

### D15. tests 镜像规则(已定)

- `tests/<模块名>/`,不加分组层。测试随源文件一起 `git mv`:P2 的每个重定位 PR 都带上对应测试;P3 的 M1 同步镜像。
- `tests/agent_loop/` 改名为 `tests/agent/`。根级的 `permissions_test.cpp` 移到 `tests/permissions/`,`skill_registry_test.cpp` 移到 `tests/skills/`。`smoke_test.cpp` 按被测对象归入对应模块目录,在 P2-08 时确认。
- 测试 helper 头共 7 个,统一收进 `tests/test_support/<area>/`:`agent_loop/stub_provider.hpp`、`channels/test_support.hpp`、`computer_use/helpers/*.hpp` 共 3 个、`sandbox/test_support.hpp`、`themes/theme_test_resources.hpp`。`tests/CMakeLists.txt` 把 `${CMAKE_SOURCE_DIR}/tests` 加为 include 根,写法是 `"test_support/<area>/x.hpp"`。lint 保证 tests/ 下的 `.hpp` 只出现在 `test_support/` 里,否则 tests 根会遮蔽 src 里的同名头。

### D21. src/web 的例外范围(已定)

用户约束是「web 这部分先不动」。执行口径如下:

- web/ 前端的 React 代码完全不动。
- src/web 内部不重构:只改 include 前缀,以及被移出文件的调用点;`WebServerDeps` 保持原样。
- 允许把**被下层借用的** web 文件移出去。依据:下层引用 web 是反向依赖,不移出就无法分层。清单如下:
  - `web/{message_payload,tool_event_payload}.*` → `engine/agent/event_payload/`;
  - `web/handlers/files_handler.*` → `base/workspace/`;
  - `web/handlers/skill_command_expander.*` → `domain/skills/`;
  - `web/pty/` → `base/pty/`;
  - `web/handlers/pinned_sessions_handler.cpp` 只有 1 行,是空壳,删除;头文件保留。
  - `routes_files.cpp:325` 只把调用点改到 `tool/safe_text_write`。
- 所有权整改在 src/web 里只允许两处最小点修,都是单独提交:opencode 导入线程与 WS listener 的 UAF,见 adopt-ownership-conventions 的 O-08(D5)。

### D4. 前端架构测试里的 C++ 路径(按推荐执行)

9 个 `*Architecture*.test.js` 会读取 C++ 源文件路径。P0-06 把这些路径收敛到 `tests/cpp_source_paths.json`,之后搬迁只改这一张表。这属于「只改 web 测试、不改 React 代码」的例外。其中 `desktopCloseDialogArchitecture.test.js:82` 用 `path.resolve` 分段拼接路径,正则扫不到,必须人工核对。

### 迁移机制:先规范化,再重定位,冻结只做纯改名,先搬后拆

1. **P0 护栏**:保证搬错时构建会大声失败,并删除死代码。死代码要先删,否则会被当作要搬、要拆的对象。
2. **P1 include 规范化**:把 1124 行 `../` 与子目录相对写法改成 `"<模块>/…"`。相对路径以包含者所在目录为基准,文件一搬就会指错;改成模块根形式后,整目录改名不再影响 include 字符串。同目录裸名暂时保留:同一模块内的文件是一起搬的,只有 P2 把某个文件移到别的模块时,那个文件的裸名 include 才需要改,而且编译会大声失败。
3. **P2 冻结前重定位**(约 12–15 个小 PR,不冻结):文件换模块、拆头、切反向依赖边。
   - 新模块先以 `src/<模块>/` 的形式建立,include 写法和冻结后完全相同;
   - 旧路径留 2 行转发头,登记进 `layers.tsv` exceptions,冻结当天到期;
   - 对应的测试一起 `git mv`。
   - **所有改内容的动作都在冻结前分散做完。**
4. **P3 冻结窗口**(半天):约 44 个模块目录 `git mv` 到 6 个分组下(M1,全部 R100,改动 0 行),再机械修正 CMake 与文档(M2)。
5. **P6 先搬后拆**:两个巨型文件在搬迁时保持 R100,其它分支对它们的改动能跟过去;先拆会破坏重命名检测。拆出来的文件直接落在最终目录。

**转发头只在 P2 使用,冻结当天全部删除。** 不批量保留,原因有三:转发头等于把旧目录重建出来,会被 GLOB 收进去;它救不了被搬走文件内部的 `../`;需要迁移的分支不超过 9 个,自助迁移的成本远低于维护转发头。

### CMake 护栏与搬迁

- **P0-04**:
  - 正则拆成两个变量:`ACECODE_TUI_DIRS`(哪些目录属于 TUI)和 `ACECODE_TUI_TESTABLE_SUBSETS`(TUI 目录下仍要进 acecode_testable 的子目录或文件,预先写入 commands/、resume/、path_reference/、markdown/,以及 drag_scroll、text_input_ops、skill_commands)。TUI 源集合为空时 `FATAL_ERROR`。
  - 新增 `cmake/acecode_source_guards.cmake`,提供 `acecode_require_sources`、`acecode_set_source_define`、`acecode_assert_known_roots`。覆盖:全部显式清单、11 个 .mm、REMOVE_ITEM、两处 set_property、OBJCXX 属性。
  - computer_use 的相对写法统一为 `${CMAKE_SOURCE_DIR}`。
  - winpty、deepin、desktop 三处 cmake 改为共用变量。
  - `tests/CMakeLists.txt` 中 EXCLUDE_FROM_ALL 冒烟目标的源路径,改为引用根 CMake 导出的变量。
- **P3 的 M2**:
  - 建 6 根 `acecode_include_roots`;
  - TUI 目录变量改为 `apps/tui`,并在同一个提交里带上空集断言;
  - 显式清单按映射表做带边界的前缀替换:前缀必须带尾部斜杠并锚定路径边界,避免 `tool/` 误伤 `tool_preamble/`、`src/web/` 误伤 `web/src/`、`session/` 误伤 `session_*`;
  - 同步更新按源设置的属性与 .mm 路径。
- **等价性证明**:`cmake_target_snapshot.py` 通过 CMake File API 导出每个 target 的 (源文件, 语言, 编译定义, 编译选项) 元组。按映射表换算回旧路径后,必须与 G0 基线逐元组相同,Windows / Linux / macOS / Deepin 四份都要比。

### 文档

- **机械替换**(随 P3 的 M2):CLAUDE.md、ARCHITECTURE.md、AGENTS.md、AGENT.md、README、tests/README、docs/ 下已跟踪的文件。help 站点先改 `docs/help-source/group*.py`,再跑 `build_help.py` 重新生成,不能直接改生成物 `sources.json`、`search-index.js`。
- **seed**:SKILL.md 的 6 处路径单独提交(M2b),同时 bump `assets/seed/seed.version`、MANIFEST 的 `bundle_version` 与对应 `skill_md_sha256`,以及 `tests/skills/default_skill_seeder_test.cpp` 里硬编码的 bundle 版本。
- **叙述性重写**(P4-02):
  - 新增 `docs/architecture/src-layout.md`,包含层定义和「新文件放哪」的决策表;
  - ARCHITECTURE.md 的结构章节改为引用它;
  - 更正 AGENTS.md / AGENT.md 里过时的「根目录 main.cpp」;
  - CLAUDE.md 顶部加一段分层说明。
- **openspec**:archive 与 specs 不改;进行中的 change 在 design.md 顶部加一行映射说明。
- **仓库外**:用户的自动记忆里约 16 处路径,由用户手工更新。
- **防漂移**:`check_doc_paths.py` 检查已跟踪文档里的 `src/`、`tests/` 路径都真实存在。

## Risks / Trade-offs

- **[正则失配导致 TUI 源静默并入 testable]** → P0-04 改目录变量 + 空集 FATAL;target 快照逐元组比对。
- **[搬迁后测试静默跳过或空转]** → P0-05 用 `find_repo_root()` 修 7 个文件共 17 处;gtest 用例清单与 SKIP 清单必须与 G0 相同,`bridge_test`、`channel_boundary_guard_test` 不得由通过变成跳过。
- **[平台 #if 块里的 include 在 Linux CI 上看不到]** → lint 做成纯文本检查,覆盖 38 个文件、90 行平台块 include;涉及 win/mac 专属文件的 PR 手动 dispatch refactor-matrix 或 package.yml。
- **[脚本误改嵌套 worktree]** → 所有脚本的文件清单只取 `git ls-files`,禁止从仓库根 `os.walk` 或 `grep -r`。`.claude/worktrees`、`.worktrees`、`.acecode/worktrees` 都嵌在仓库根下。
- **[行尾被改坏]** → 按字节逐行读写,保留 CRLF/LF;numstat 验收「增加 = 删除 = 改动行数」。
- **[冻结窗口期间 master 被推进]** → 不 rebase 已生成的提交,丢弃后在新 master 上重跑脚本(脚本是确定性的)。
- **[重名文件合并到同一目录]** → `validate_map.py` 报告重名:`runtime.hpp/.cpp` 3 份、`main.cpp` 2 份、`pointer_overlay.cpp` 2 份(`tool/agent_browser/pointer_overlay.cpp` 冻结前改名为 `browser_pointer_overlay.cpp`)。
- **[多根 include 的遮蔽]** → R8 唯一解析 + R9 子目录不与模块或分组同名,两条都由 lint 硬检查。
- **[P2 期间树先变差再变好]**:约 2 周内会临时多出 llm、permissions、platform、ipc、workspace、pty、session_host、agent 等顶层目录和转发头 → 接受;每个 PR 都要求 lint 违规数单调下降,P2-08 完成时为 0。
- **[git log / blame 断链]** → 机械提交写进 `.git-blame-ignore-revs`;文档说明用 `git log --follow` 与 `git blame -C -C -M`。
- **[历史被改写导致分支失去共同祖先]** → 搬迁前后禁止 `strip-ai-attribution` 或 filter-repo 这类改写历史的操作。

## Migration Plan

- P0–P2 每个 PR 都可单独 revert。
- P3 在冻结前后各打 tag:`pre-src-layout` / `post-src-layout`。若需整体回滚,revert M3 → M2b → M2 → M1(或 reset 到 `pre-src-layout` 后重新合入冻结后的新提交);回滚后仍需重跑 G0 闸门。
- 回滚与重新生成都用同一套脚本,不手工修补。

---

## 6. 提交与协作约定(系列通用,所有代理必须遵守)

1. **提交信息前缀**:
   - 一律写成 `refactor20260927(<change>/<task-id>): <中文摘要>`,`<change>` 取 `layers` / `agent-loop` / `tui-main` / `ownership`。例如 `refactor20260927(layers/P1-01): include 改为模块根形式`。
   - 机械或纯搬迁的提交在摘要末尾加 `[mechanical]`;冻结的 M1 加 `[no-build]`。
   - 行为变更提交在摘要前加 `[行为变更 Dn]`,例如 `refactor20260927(ownership/O-04): [行为变更 D6] daemon 先停会话再停 MCP/LSP`。
2. **一个任务一组独立提交**:
   - 行为变更单独提交,可以单独 revert;
   - 纯搬迁与内容修改分开提交;
   - 禁止 squash、禁止 filter-repo;
   - 提交不写 Co-Authored-By,pre-push 钩子会拒。
3. **认领**:
   - 开工前,在对应 change 的 tasks.md 里给该任务行末尾追加 `〔认领: <代理名> <日期>〕`,单独提交到 master(提交信息 `refactor20260927(<change>/<task-id>): 认领`),避免重复开工;
   - 完成并合入后改为 `[x]`,保留认领标记以便追溯;
   - 前置任务未勾选的,不得开工。
4. **工作方式**:
   - 每个任务在独立 worktree 的分支 `refactor20260927/<task-id>` 上完成;
   - 合入前先把最新 master 合进分支,重跑该任务的全部验证,再以 `--no-ff` 或 fast-forward 合入 master;
   - 拆分类任务当天合入,缩短其它分支需要 rebase 的窗口。
5. **热点文件**:分两档。
   - **严格互斥**:同一时刻只允许一个进行中的任务修改。以 tasks.md 的认领标记为准。
     - agent_loop.hpp / agent_loop.cpp,以及 P6A 期间 engine/agent 下正在拆的文件;
     - main.cpp,以及 P6B 期间 apps/tui/app 下正在拆的文件。
   - **合入串行**:CMakeLists.txt、tests/CMakeLists.txt。
     - 可以并行开发,但合入 master 要一个接一个。
     - 后合入的一方先 rebase 到最新 master,再重跑 cmake_target_snapshot 与相关构建。
     - 显式清单只改自己那几行,不顺手重排。
   - **会碰到同一文件的跨 change 任务**:下列任务开工前,先看对方是否正在进行,避免同时修改。
     - A-02 / A-14 与 O-02 / O-03 都改 session_registry;
     - A-07 与 O-03 都改 thread_service;
     - A-14 与 O-05 都改 subagent_host。
6. **构建目录**:
   - 不要用主仓 `build/`,它可能正被其它会话并发构建;每个 worktree 用自己的全新构建目录。
   - Windows 构建前,确认没有运行中的 acecode / acecode-desktop 进程锁住 exe。
   - 搬迁或拆分后一律全新目录构建,避免旧 .obj 残留引发 LNK2019 `__std_*`。
7. **验证记录**:提交说明(或 PR 描述)写明执行过的验证命令和结果,并逐条列出触碰到的不变量守护测试(见 §7.3)。
8. **单测规范**:新增或修改的测试一律写中文注释,写清触发场景和期望行为,回归测试附上 bug 的表现。进程级状态(PA 学习器、工具名映射等)用 RAII 的 Scoped 守卫恢复。
9. **文档同步**:改到 CLAUDE.md 里描述过的机制时,同一任务内同步更新 CLAUDE.md 对应段落与行号锚点。

## 7. 验收闸门

### 7.1 基线 G0(P0-07 采集)

在 base 提交上用全新构建目录采集以下内容,作为之后所有闸门的对照组:

1. `cmake_target_snapshot`:Windows MSVC、Linux、macOS、Deepin 各一份,并包含 EXCLUDE_FROM_ALL 冒烟目标;
2. `gtest_inventory`:全量用例清单、实际 SKIP 清单、ctest 注册的测试名;
3. 分层、行数、所有权、doc-paths 四类 lint 的基线。

### 7.2 各阶段验收

| 阶段 | 验收标准 |
|---|---|
| P0 | D1/D2/D15/D21/D22 已写进 `layers.tsv` 初版;正式规则下的违规数已记录;test.yml 全绿;refactor-matrix 基线已采集;cmake_target_snapshot(含冒烟目标)等于 G0;故意改坏路径时 configure 报 FATAL;TUI 源集合非空;gtest 清单与 SKIP 清单已归档;四类 lint 基线已接入 PR |
| P1 | numstat 满足每个文件「增加 = 删除 = 改动的 include 行数」;第二次运行 0 diff;src 下不再有 `../`;Win/Linux/mac/arm/Deepin 全新目录构建通过;用例清单与 SKIP 清单等于 G0 |
| P2(每个 PR) | lint 违规数下降;转发头已登记;按映射换算后每个文件所属的 target 不变;测试已随源文件移动;三平台构建通过;用例清单不变;P2-08 完成时违规数为 0,冒烟目标都能构建 |
| P3 | M1 全部 R100;M2 之后,target 快照按映射换算回旧路径后与 G0 逐元组相同(四个平台,含冒烟目标);include / known-roots / doc-paths 三个 lint 为 0;用例清单与 SKIP 清单相同;package.yml 全平台通过,Deepin 产物的 `current_target()` 为 linux-deepin;`pnpm test` 通过;Windows 全新目录全量构建 + verify-package;冒烟:TUI 一轮对话、`acecode -p`、daemon + Web 打开会话、acecode-desktop 打开 workspace、ConPTY 与 winpty 控制台 |
| P4 | strict 模式下 exceptions 为空;check_doc_paths 为 0;help 站点重新生成后 diff 只涉及路径;tests/README 镜像表已更新 |

split-agent-loop、split-tui-main、adopt-ownership-conventions 的验收见各自 design.md。**一期整体验收**:四个 change 的验收全部满足,每个【行为变更】都有拍板记录和单独的提交。

### 7.3 系列全局不变量(所有代理都要守住)

1. **Prompt cache 前缀字节稳定**:
   - 注入到最后一条真实 user 消息之前的内容,只能由输入内容决定;
   - 静态 system prompt 只包含 cwd 和按天的日期;
   - `cached_context_for_api` 按 cache_key 钉住内容;
   - 工具表按 `std::map` 的顺序。

   守护测试:`RequestPrefixIsByteStableAcrossIterationsInATurn`、system_prompt 的 byte-stable 用例、`NonGptModelStateIsByteIdenticalToLegacyPrompt`。
2. **provider 历史出口**:主请求与 side question 都只经过 `model_facing_provider_messages`。压缩目前直接传入原始 messages_,本系列保持原样。
3. **工具名映射单出口**:只有 tool_rewrites 调用 `set_model_tool_name_mappings`;给模型看的文案里工具名一律动态获取;不对工具输出的正文做重写。
4. **审批门唯一入口**:决策链顺序逐字不变。goal 下越权一律 Forbidden。审计只在「决定已经作出」的分支记恰好一条。
5. **单写者**:messages_ 只有一个写入口;TuiState 只在 `state.mu` 下修改;Web transcript 状态不在本系列范围内。
6. **锁序**:
   - `active_turn_mu_ → queue_mu_ → AskUserQuestionPrompter` 内部锁;
   - 跨 loop 只允许 `source.queue → target.queue`;
   - `state.mu →` 其它内部锁;
   - 叶子锁持锁期间不 emit、不回调。
7. **PA 可以整块删除**:只有 PA 接触点表里登记的位置能接触 PA。
8. **路径**:cwd 一律用 UTF-8 的 `std::string` 传递;`workspace_hash` 手工截取最后一段,不经过 `fs::path`。
9. **构建边界**:
   - acecode-desktop 不链 acecode_testable,也不链 domain 及以上;
   - `ACECODE_DEEPIN`、`ACECODE_CHANNEL_ASSET_DIR` 仍挂在正确的源文件上;
   - OBJCXX 属性与 REMOVE_ITEM 仍生效;
   - 每个文件所属的 target 不变;
   - EXCLUDE_FROM_ALL 冒烟目标都能构建。

## 8. 系列路线图

### 8.1 阶段与 change 归属

| 阶段 | 期 | 内容 | 所属 change | 冻结? |
|---|---|---|---|---|
| P0 护栏与清理 | 一期 | P0-01~P0-08 | restructure-src-layers | 否 |
| P0 巨型文件前置 | 一期 | P0-10 agent_loop 死代码、P0-11 表征测试 | split-agent-loop | 否 |
| P0 巨型文件前置 | 一期 | P0-09 main.cpp 孪生 helper 去重、P0-12 TUI 手工回归清单 | split-tui-main | 否 |
| P1 include 规范化 | 一期 | P1-01~P1-02 | restructure-src-layers | 否 |
| P2 冻结前重定位 | 一期 | P2-02~P2-09 | restructure-src-layers | 否 |
| P2 RAII 原语 | 一期 | P2-01(必须在 P3 前合入) | adopt-ownership-conventions | 否 |
| P3 冻结窗口 | 一期 | P3-01~P3-03 | restructure-src-layers | **半天** |
| P4 收尾 | 一期 | P4-01~P4-03 | restructure-src-layers | 否 |
| P6A 拆 agent_loop | 一期 | A-01~A-14、A-17 | split-agent-loop | 串行 |
| P6B 拆 main.cpp | 一期 | B-01~B-13 | split-tui-main | 串行 |
| P7-O 所有权整改 | 一期 | O-01~O-11(O-10 原计划编号 A-15,O-11 原 A-16) | adopt-ownership-conventions | 否 |

### 8.2 全局依赖图

```
P0-01(决策定稿)
 ├→ P0-02 → P0-08 → P0-09
 ├→ P0-03 → {P0-04, P0-07, P2-09}
 ├→ P0-05、P0-06
 ├→ P0-10、P0-11、P0-12(随时可做;P0-09/10/11 必须在 P2-08 之前合入)
 ├→ P2-01(随时可做,必须在 P3 前合入)
 └→ P1(单个 PR,前置:P0-03/04/05/07)
     └→ P2:{02,03,04} 可并行 → 05(需 02)→ 06 → 07 → 08(另需 P0-09/10/11);09 全程并行
         └→ P3:演练 P3-01(另需 P2-01)→ 冻结搬迁 P3-02 → 分支迁移 P3-03
             ├→ P4:P4-01、P4-02、P4-03
             ├→ P6A:A-01 … A-13 串行 ─┐
             ├→ P6B:B-01 … B-13 串行 ─┼→ A-14(需 A-13 + B-13)→ A-17
             │   (B-11 需 A-01)         ├→ O-10(需 A-14)
             │                          ├→ O-11(需 A-14 + O-03)
             │                          └→ O-05(需 O-01 + O-02 + B-13)
             └→ P7-O:{O-01, O-02, O-07, O-09} 立即并行;
                      O-01 → {O-06, O-08};
                      O-02 → O-03 → O-04;
                      O-07 的 TUI 部分需 B-12
                        └→ 各 change 验收 → 一期验收
```

**适合交给子代理或 Codex 独立完成的任务**(输入明确,验收能由机器检查):P0-02/03/05/06/07/10/11、P1、P2-01/03/04/06/09、P3-03、P4 全部、A-02、A-17、B-01、B-02、B-04、O-01、O-06、O-09。其余任务判断密集,或者要改热点文件,由负责该 change 的主代理串行完成。

### 8.3 执行波次与并行泳道

冻结窗口(P3)把一期切成前后两段:
- **冻结前**:以 restructure 为主线,其它三个 change 只做「前置」任务;
- **冻结后**:四条泳道并行。

**冻结前(波次 0–4)**

| 波次 | 可同时进行的任务 | 进入条件 | 说明 |
|---|---|---|---|
| 0 | P0-01 | 无 | 唯一起点,由主代理完成 |
| 1 | P0-02、P0-03、P0-05、P0-06、P0-10、P0-11、P0-12、P2-01 | P0-01 | 8 项互不依赖,可同时开工 |
| 2 | P0-04、P0-07、P2-09(需 P0-03);P0-08(需 P0-02) | 见括号 | P0-04 与 P0-08 都改 CMakeLists.txt,按「合入串行」处理 |
| 3 | P0-09(需 P0-08);P1-01 → P1-02(需 P0-03/04/05/07) | 见括号 | P1 是脚本生成的提交,建议等 P0-08/09/10 合入后,在最新 master 上生成 |
| 4 | P2-02、P2-03、P2-04 并行 → P2-05 → P2-06 → P2-07 → P2-08 | P1 完成 | P2-08 另需 P0-09/10/11 已合入;P2-09 全程并行 |

**冻结(波次 5)**:P3-01 演练 → P3-02 正式搬迁 → P3-03 分支迁移。
- 进入条件:P2 全部完成,且 P2-01 已合入。
- P3-02 的半天窗口内,src/、tests/、CMake 暂停合入。

**冻结后(波次 6,四条泳道并行)**

| 泳道 | 顺序 | 与其它泳道的交汇点 |
|---|---|---|
| L 收尾 | P4-01、P4-02、P4-03 可并行;P3-03 同期进行 | 无 |
| A agent_loop | A-01 → A-02 → … → A-13 严格串行 → A-14 → A-17 | A-01 完成后 B-11 才能开工;A-14 要等 B-13 |
| B TUI main | B-01 → B-02 → … → B-13 严格串行 | B-12 完成后,O-07 的 TUI 部分可以做;B-13 完成后,A-14 与 O-05 可以做 |
| O 所有权 | {O-01, O-02, O-07, O-09} 立即并行;O-01 → {O-06, O-08};O-02 → O-03 → O-04 | O-05 等 B-13;O-10 等 A-14;O-11 等 A-14 与 O-03 |

收尾:A-17、O-10、O-11 完成 → 四个 change 各自验收 → 一期验收。

**关键路径**:

P0-01 → P0-03 → P0-04 → P1-01 → P2-02 → P2-05 → P2-06 → P2-07 → P2-08 → P3-01 → P3-02 → A 泳道 A-01 … A-13 → A-14 → {A-17, O-10, O-11} → 验收

- B 泳道 B-01 … B-13 与 A-01 … A-13 并行,A-14 要等两条都走完,所以实际取两者中较长的一条。
- 想缩短工期,只能压缩这条链;其余任务尽量排进关键路径的空档。

**建议的代理配置**:
- **冻结前**:
  - 1 个主代理走关键路径,负责【主】任务:P0-01、P0-04、P0-08、P0-09、P2-02、P2-05、P2-07、P2-08、P3;
  - 3–5 个子代理并行做【子】任务;波次 1 最多可同时开 8 个。
- **冻结后**:
  - 3 个主代理,分别负责 A、B、O 三条泳道;
  - 另外 1 个子代理负责 L 泳道,以及 O 泳道中的【子】任务(O-01、O-06、O-09)。

### 8.4 二期待办(各自独立 change,本期只登记,不排期)

- **P5 按组建库(D17)**:
  - 建 base_core、base_host、domain、adapters、engine、host(链接 Crow)、web 等 STATIC 库,`acecode_testable` 改为 INTERFACE 聚合目标;
  - apps/tui 整体做成 STATIC 库,删除 TESTABLE_TUI 清单与正则。
  - 需要 Linux 与 MSVC 全量链接验证。
- **P6C 其它超限文件**:每个文件一个 change,串行。
  - apps/desktop:agent_browser_host_mac.mm 3157、web_host.cpp 2889、main.cpp 2860、agent_browser_host.cpp 2832、tray_icon_win.cpp 2261;
  - base/config/config.cpp 2875;
  - apps/tui:settings/settings_center.cpp 2842、markdown/mermaid_renderer.cpp 2484、settings/management_center.cpp 2146、commands/builtin_commands.cpp 2060;
  - host/session_host:session_registry.cpp 2466、thread_service.cpp 1211;
  - adapters/provider:openai_provider.cpp 2025、text_tool_call_recovery.cpp 1387、anthropic_provider.cpp 1326;
  - domain:experts/expert_registry.cpp 1882、session/session_manager.cpp 1866、skills/default_skill_seeder.cpp 1577、sandbox/command_classifier.cpp 1013;
  - adapters 其余:computer_use/native_windows.cpp 1545、tool/agent_browser/browser_tools.cpp 1422、tool/file_read_tool.cpp 1083、tool/mcp_manager.cpp 1024、themes/theme_store.cpp 1022;
  - engine/prompt/system_prompt.cpp 1383:单独立项,门槛是 byte-stable 用例;
  - host/remote_control/session_channel_binder.cpp 1236;
  - base/platform/native_ui/custom_toast_win.cpp 1230;
  - 豁免:apps/web 的 4 个文件、external/stb。
- **P7-C/I 组合根与 ConfigStore**:
  - ConfigStore v1,WebServerDeps 的现有字段传 legacy 访问器,web 内部零改动;
  - host/app_runtime(SurfaceProfile 按现状填写,D10);
  - 租约化 lsp 与 web_search;
  - CoreServices / SessionHost;
  - SessionRegistryDeps 拆分;
  - 三个入口依次迁移;
  - TUI 主会话并入 registry(D16)排在更后。
- **P8 行为修复(D12)**:每项单独提交。
  - 进度 key 用 `"\0"` 拼接;
  - `stop_hook_active_` 跨回合残留;
  - 回合收尾用了 `goal_store()` 而不是 `existing_goal_store()`;
  - 被 hook 拦截的回合不发 BusyChanged(true);
  - run_shell 不发 BusyChanged(true);
  - GPT 模型的上下文估算多算了 edit/write;
  - worktree 下 transcript_path 错误;
  - `/sandbox` 在 HTTP 线程直接改会话;
  - 压缩请求是否改走 model_facing_provider_messages;
  - TUI 的 cwd 改为 UTF-8(需提供旧 hash 回退)等。

## 9. 决策登记

| # | 决策 | 结论 | 状态 | 影响 |
|---|---|---|---|---|
| D1 | include 根 | 6 个分组各自作为 include 根 | 已定(按推荐,用户无异议) | P0-03、P1、P3 |
| D2 | 分组命名与划分 | base / domain / **adapters** / engine / host / apps | **已定(用户 2026-09-27 确认,capability 更名为 adapters)** | 全部 |
| D3 | 冻结窗口;旧 worktree 清理 | 半天窗口,提前 1–2 天公告;28 个补丁等价的旧 worktree 由用户确认后自行清理 | 按推荐执行 | P0-02、P3 |
| D4 | 9 个前端架构测试中的 C++ 路径 | 允许,收敛到 `tests/cpp_source_paths.json`,只改测试 | 按推荐执行 | P0-06 |
| D5 | src/web 内两处 UAF 最小点修 | 允许,单独提交 | 按推荐执行 | O-08 |
| D6 | 关停顺序:先停会话,再停 MCP/LSP | 一期做 | **已定(用户 2026-09-27 确认)** | O-04、O-05 |
| D7 | 会话销毁与退出时真正释放会话(结束会话、删写者租约、清空排队的控制任务) | 一期做 | **已定(用户 2026-09-27 确认)** | O-02、O-11 |
| D8 | 提示词相关配置改为回合级快照 | 一期做 | **已定(用户 2026-09-27 确认)** | O-10 |
| D9 | 退出时对可放弃的后台工作做有界等待 | 最多 2 秒,三个入口都生效;一期做 | **已定(用户 2026-09-27 确认)** | O-07 |
| D10 | 三个入口之间的能力差异 | 本期保持现状,只修正文档 | 按推荐执行 | 全部 |
| D11 | side question 的线程模型 | 一个请求一个线程,线程表改为可回收 | 按推荐执行 | A-09 |
| D12 | 调研发现的疑似 bug | 放二期 P8,逐项单独提交 | 按推荐执行 | P8 |
| D13 | prompter 与 EventDispatcher 的构造循环 | 两个 prompter 保留「只能在 start() 前调用」的 setter | 按推荐执行 | A-14 |
| D14 | AskUserQuestionPrompter 归属 | AgentLoop 独占,SessionEntry 只持借用别名 | 按推荐执行 | A-14 |
| D15 | tests 镜像规则 | `tests/<模块名>/`,不加分组层,随源文件一起搬;`tests/agent_loop` 改名为 `tests/agent` | 已定(按推荐,用户无异议) | P2、P3、P4-02 |
| D16 | TUI 主会话并入 SessionRegistry | 二期之后再做 | 按推荐执行 | — |
| D17 | 按组建 STATIC 库 | 放二期 | 按推荐执行 | P5 |
| D18 | ToolContext 彻底改造 | 以后再做 | 按推荐执行 | — |
| D19 | 并行只读线程上的 hook 副作用 | 只加锁,不改时序 | 按推荐执行 | A-08 |
| D20 | openspec 组织 | 一期 4 个 change,统一带 `refactor20260927-` 前缀;二期按需另开 | **已定(用户 2026-09-27 确认)** | P0-01 |
| D21 | src/web 例外范围 | 见上文 D21 | 已定(按推荐,用户无异议) | P2、O-08 |
| D22 | 本期范围 | 一期 = P0–P4 + P6A/B + P7-O(含 D6–D9);其余放二期 | 已定(按推荐;D6–D9 由用户确认纳入一期) | 全部 |

「按推荐执行」的决策可以在对应任务开工前推翻;推翻后需同步修改本表和受影响任务的描述。
