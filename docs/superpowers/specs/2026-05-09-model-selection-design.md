# 模型选择功能完善 — 设计

- 日期:2026-05-09
- 范围:让 ACECode 在 TUI 与 WebUI 两端共享 per-session 模型语义,并补足 saved_models 注册表的增删改 UI。
- 状态:已完成 brainstorming,待生成实施 plan。

## 1. 背景与动机

ACECode 当前的模型选择实现处在"半成品"状态:

- daemon 一侧的 per-session 模型已经基本到位 — 每个 SessionEntry 自带独立 Provider 实例,`SessionRegistry::switch_model` 能切某条会话的模型而不影响其他会话,SessionMeta 持久化 provider/model/model_preset,WebUI 的 `<ModelPicker>` 与 `/api/sessions/:id/model` 端点已接通。
- TUI 一侧的 `/model` 命令切的仍是**进程级 provider**(`ctx.provider_handle` 一个全局 shared_ptr),不是 per-session。原因是 TUI 的 main.cpp 直接拉一个 AgentLoop,没经过 SessionRegistry。
- saved_models 注册表(`saved_models` + `default_model_name`)只能通过手编 `~/.acecode/config.json` 维护,UI 没有增删改入口。

用户目标:

1. 支持多种模型(已有 saved_models)。
2. 设置全局默认模型(已有 default_model_name)。
3. 每个会话有自己的模型属性(daemon 已实现,TUI 还没)。
4. 聊天中途随时切换(WebUI 已有,TUI /model 切的是全局)。
5. 同一逻辑在 TUI 与 WebUI 间共享,daemon 工程提供共用接口。

## 2. 设计决策

经 brainstorming 与用户确认:

- **架构方向**:共享 lib + TUI 进程内直连(不让 TUI 改造成 daemon HTTP 客户端)。
- **TUI session 模型**:仍是单 session,这个 session 有自己的 model state。
- **saved_models 管理 UI**:WebUI 设置面板做完整增删改,TUI 加 `/model add|edit|rm|set-default` 子命令。
- **实现路径**:抽一个 `apply_model_to_session` 纯函数到 `acecode_testable`,daemon 的 `SessionRegistry::switch_model` 与 TUI 的 `/model` 命令都调它。

## 3. 架构总览

```
                  ┌─────────────────────────────────────┐
                  │  src/provider/  (shared lib)        │
                  │  ┌───────────────────────────────┐  │
                  │  │ model_resolver.cpp            │  │  纯函数(已存在)
                  │  │   resolve_effective_model     │  │
                  │  │   synth_legacy_entry          │  │
                  │  └───────────────────────────────┘  │
                  │  ┌───────────────────────────────┐  │
                  │  │ apply_model_to_session.{h,cpp}│  │  ← 新增,纯逻辑
                  │  └───────────────────────────────┘  │
                  │  ┌───────────────────────────────┐  │
                  │  │ saved_models_editor.{h,cpp}   │  │  ← 新增,纯逻辑
                  │  └───────────────────────────────┘  │
                  └────────┬───────────────────┬────────┘
                           │                   │
       ┌───────────────────┘                   └────────────────────┐
       ▼                                                            ▼
┌──────────────┐                                       ┌─────────────────────┐
│ src/main.cpp │                                       │ src/web/handlers/   │
│ ctx.provider_slot ◀── 升级版                         │   models_handler    │
│ /model 调 apply_model_to_session                     │   POST /api/models  │
│ /model add|edit|rm|set-default                       │   PUT/DELETE        │
│ FTXUI picker(占位 → 真选择器)                       │   /api/config/      │
└──────────────┘                                       │     default-model   │
                                                       │ /api/sessions/:id/  │
                                                       │   model → 调 helper │
                                                       └─────────────────────┘
                                                                  ▲
                                                       ┌─────────────────────┐
                                                       │ web/src/components/ │
                                                       │   ModelPicker.jsx   │ 已存在,补 toast
                                                       │   ModelManager.jsx  │ 新增设置面板
                                                       │   Settings.jsx      │ 新增容器
                                                       └─────────────────────┘
```

## 4. 核心组件接口

### 4.1 共享 helper: `apply_model_to_session`

`src/provider/apply_model_to_session.{hpp,cpp}`,进 `acecode_testable`。

```cpp
namespace acecode {

struct ApplyModelResult {
    SessionModelState state;    // 最终 state(供 UI 回显)
    std::string warning;        // 非致命警告,如 Copilot silent_auth 失败
};

struct ApplyModelDeps {
    SessionEntry::ProviderSlot* provider_slot = nullptr;  // 必填
    SessionManager*             sm = nullptr;             // 选填:TUI 早期可能没有
    AgentLoop*                  loop = nullptr;           // 选填:用于 set_context_window
    AppConfig*                  cfg = nullptr;            // 必填:读 saved_models / models_dev
};

// 抛 std::runtime_error("provider unavailable" / "context unavailable")
// 表示 profile 无法落地。
ApplyModelResult apply_model_to_session(const ModelProfile& profile,
                                         const ApplyModelDeps& deps);

} // namespace acecode
```

实现内容(从 `session_registry.cpp::resolve_from_profile` + `switch_model` 抽出):

1. `state_from_profile(*cfg, profile)` 算 SessionModelState(含 context_window)。
2. `create_provider_from_entry(profile)` 造新 LlmProvider 实例。
3. 若是 copilot:`try_silent_auth()` — 失败只填 warning,不抛。
4. `lock(slot.mu) → slot.provider = std::move(new_provider)`。
5. `loop` 非空 → `loop->set_context_window(state.context_window)`。
6. `sm` 非空 → `sm->set_active_provider(provider, model, name)`(持久化到 SessionMeta)。

`SessionRegistry::switch_model` 改成壳子:lookup entry → 把 `&entry.provider_slot, entry.sm.get(), entry.loop.get(), deps_.config` 传进去。失败/成功路径不变。

### 4.2 TUI: 把 `ctx.provider_handle` 升级成 `provider_slot`

当前 `CommandContext` 有:

- `std::shared_ptr<LlmProvider>* provider_handle`
- `std::mutex* provider_mu`
- `LlmProvider& provider`(直接引用,会随 swap 失效)

改为:

- `SessionEntry::ProviderSlot* provider_slot`(必填)
- 删掉 `provider_handle` / `provider_mu` / `LlmProvider& provider`
- main.cpp 造一个进程级 `ProviderSlot slot; slot.provider = ...;`,所有 `ctx.provider_slot = &slot`。

`AgentLoop::ProviderAccessor` lambda 闭包从 `provider_handle` 切到 `&slot`,worker thread 拿快照的安全语义不变。

`/model <name>` → 调 `apply_model_to_session`。`--cwd` / `--default` 的写盘逻辑不动。

### 4.3 saved_models 编辑模块

`src/config/saved_models_editor.{hpp,cpp}`,纯逻辑(读 cfg → 改 cfg → 校验,落盘交给 caller)。

```cpp
struct SavedModelDraft {
    std::string name;              // 必填,不能 "(" 起头
    std::string provider;          // "openai" | "copilot"
    std::string model;             // 必填
    std::string base_url;          // openai 必填,copilot 忽略
    std::string api_key;           // openai 必填(空字符串触发 INVALID_API_KEY)
    std::optional<std::string> models_dev_provider_id;
};

enum class SavedModelEditError {
    OK, INVALID_NAME, RESERVED_NAME, NAME_TAKEN,
    UNKNOWN_PROVIDER, MISSING_MODEL, MISSING_BASE_URL, INVALID_API_KEY,
    NOT_FOUND, IN_USE_AS_DEFAULT
};

SavedModelEditError add_saved_model(AppConfig& cfg, const SavedModelDraft& d);
SavedModelEditError update_saved_model(AppConfig& cfg, const std::string& name,
                                        const SavedModelDraft& d);
SavedModelEditError remove_saved_model(AppConfig& cfg, const std::string& name);
```

特殊规则:

- `IN_USE_AS_DEFAULT` — 删除/改名时该名是当前默认 → 拒绝,要求 caller 先改默认。
- `RESERVED_NAME` — 名字以 `(` 起头(撞 `(legacy)` / `(session:...)`)。
- 改名(update 时 name 改了)走 delete + add,**禁止改名同时改字段**;同时改名也走 IN_USE_AS_DEFAULT 拒绝(若旧名是默认)。

### 4.4 HTTP 端点(daemon)

| 方法 | 路径 | body | 行为 |
|---|---|---|---|
| `POST` | `/api/models` | `SavedModelDraft` | add → editor → save_config → 200/4xx |
| `PUT` | `/api/models/<name>` | `SavedModelDraft` | update(body.name 必须等于 url.name) → 200/4xx |
| `DELETE` | `/api/models/<name>` | — | remove → 200/409(IN_USE_AS_DEFAULT) |
| `POST` | `/api/config/default-model` | `{name}` | 写 `cfg.default_model_name` + save_config → 200/404 |

`GET /api/models` 已有,无变化。`/api/sessions/:id/model` 已有,内部改成调 `apply_model_to_session`。

错误响应统一 `{"error": "<error_code>", "message": "<人话>"}`。`api_key` 在响应里**永不返回**。请求体里收到的 `api_key` 写入 cfg.json,前端编辑界面默认不显示已有 api_key。

### 4.5 UI 入口

**WebUI**:

- `ModelPicker.jsx` 已有,补两点:① 切完成功 toast 显示 `name (provider/model)`;② 失败 toast 通过 error_code 查 i18n 文案。
- 新增 `ModelManager.jsx`(设置面板):Sidebar 底部齿轮 → 抽屉,左侧 saved_models 列表(默认带星标),右侧编辑表单。增/删/改/设默认全在这里。
- 新增 `Settings.jsx` 容器,先挂"模型"一节。
- 新增 `lib/errors.js`:error_code → 中文文案字典,Picker 与 Manager 共用,未识别码退到 error.message 原文。
- 新增 `lib/modelPicker.js` 辅助:`isCurrentValueOrphaned` / `buildOptionsWithOrphan`,检测当前值不在列表时插一条 disabled 灰条提示。
- 新增 `lib/modelManager.js`:`validateModelDraft` 给前端提交前快速校验。

**TUI**:

- `/model`(无参) — 升级文本占位为 FTXUI picker(`/skills` picker 风格)。当前 effective 行打 `*`。
- `/model <name>` / `/model --cwd <name>` / `/model --default <name>` — 行为不变。
- `/model add` / `/model edit <name>` / `/model rm <name>` / `/model set-default <name>` — 新增子命令。`add` / `edit` 走 FTXUI 多行表单。

## 5. 数据流

### 5.1 对话中途换模型(WebUI)

用户点下拉选新模型:

1. `ModelPicker.onChange` → `POST /api/sessions/<sid>/model {name}`。
2. handler:`require_auth` → `find_model_by_name` → `registry.switch_model(sid, profile)`。
3. `SessionRegistry::switch_model`(壳子):lookup entry → `apply_model_to_session(profile, {slot, sm, loop, cfg})`。
4. helper 内部:算 state → 创建 Provider → (Copilot)silent_auth → 替换 slot → 更新 loop 窗口 → 写 meta。
5. 返 200 + state JSON;前端 toast"已切换"。

**正在跑的那一轮不会被打断**:worker thread 在 turn 开始已强引用 Provider,slot 替换不让旧实例失效。下一轮才用新模型。这是项目现有的安全设计,helper 直接继承。

如果 swap 成功但 meta 写盘失败:helper 不回滚内存(用户已看到流式输出在用新模型),透出 warning。后果只是"daemon 崩了之后这条会话恢复时显示旧模型",用户重切一次即可。

### 5.2 对话中途换模型(TUI)

用户敲 `/model gpt-4o-fast`:

1. 命令解析 → `lookup_entry_by_name` → ModelProfile(或报"未知模型名")。
2. 调 `apply_model_to_session(profile, {ctx.provider_slot, ctx.sm, ctx.agent_loop, &ctx.config})`。
3. `announce_switch`:push system 消息,刷状态行 + token 计数条。

没有 HTTP,没有事件队列。`--cwd` / `--default` 的写盘逻辑跑在 helper 之后。

### 5.3 saved_models 增删改(WebUI)

ModelManager 提交表单 → `POST /api/models` → handler 调 `add_saved_model` → OK 则 `save_config(cfg)` → 200。前端拉 `listModels()` 刷新。

`save_config` 抛异常时,handler **回滚内存**(把刚加的 pop / 改前快照写回)再 500。

PUT/DELETE 同形。`POST /api/config/default-model` 校验 name 必须在 saved_models 里(或 `(legacy)`),否则 404。

**改默认对正在跑的会话不产生任何影响**。新默认只影响后续新建/恢复。

### 5.4 新建会话用哪个模型

resolution chain(已实现,本次设计不动):

1. `SessionOptions::model_name` 显式指定 → 用之。
2. resume 路径 → 看 SessionMeta(详 5.5)。
3. 都没有 → `resolve_effective_model(cfg, cwd_override, ∅)` → cwd_model_override.json → default_model_name → legacy 兜底。

WebUI 的"新建会话"按钮**不**带模型参数,走第 3 步;用户进会话后用下拉切。**"新建时弹模型选择" YAGNI 不做**。

### 5.5 恢复会话 — 找不着原模型怎么办

举例:meta 记 `model_preset="prod-fast"`,但用户改名成 `prod-fast-v2`。

`resolve_effective_model` 顺序找:

1. 按 `(provider, model)` 二元组在 saved_models 里匹配 → 命中即用。
2. 看 legacy synth 兜底能不能 (provider, model) 撞上 → 用之。
3. 都不行 → 合成 `(session:abc12345)` ad-hoc entry,base_url/api_key 借 cfg.openai。

UI 顶栏下拉的当前值显示 `(session:abc12345)`,不在列表里。`<select>` 显示 orphan value 的行为不一致 → 前端检测后插一条 disabled `prod-fast (已被改名/删除)` 灰条提示。用户切到任何正经条目后灰条消失,meta 自动写新名。

**这段已实现**。

### 5.6 改全局默认对现有会话

**啥事没有。** 每会话定下模型后稳定使用,直到用户主动切。

## 6. 错误处理

### 6.1 增/改 saved_models 校验失败

| 错误码 | 触发 | HTTP | 前端表现 |
|---|---|---|---|
| `INVALID_NAME` | 名字空 / 含非法字符 | 400 | 字段下方红字 |
| `RESERVED_NAME` | `(` 开头 | 400 | 红字"以 ( 开头是系统保留" |
| `NAME_TAKEN` | 重名 | 409 | 红字"已存在同名条目" |
| `UNKNOWN_PROVIDER` | provider 不是 openai/copilot | 400 | 字段下方红字 |
| `MISSING_MODEL` / `MISSING_BASE_URL` / `INVALID_API_KEY` | 必填空 | 400 | 对应字段红字 |
| `NOT_FOUND` | PUT/DELETE name 不存在 | 404 | toast"该模型已不存在,刷新一下" |
| `IN_USE_AS_DEFAULT` | 删/改名命中默认 | 409 | toast"先去改默认再删" |

`save_config` 抛异常 → handler 回滚 cfg 内存改动 → 500 + 原文。

### 6.2 切模型时 Provider 创建失败

helper 五步:

- 步骤 1-2 抛异常 → helper 抛 runtime_error,**slot 没动**。handler 500。前端 setValue(prev) + 错误 toast。
- 步骤 3(Copilot silent_auth)失败 → 非致命,填 warning 不抛。**slot 已替换**,前端 toast"已切换,但 Copilot 登录未生效,可能需要 /login"。
- 步骤 4(set_context_window)不会失败。
- 步骤 5(写 meta)失败 → 填 warning 不回滚内存(用户已看到流式输出用新模型),后果可控。

### 6.3 切模型撞正在跑的轮次

**不冲突**。worker 在 turn 开始已强引用 Provider,slot 替换不让旧实例失效。当前轮用旧模型跑完,下一轮用新模型。**不**做"切换自动打断"的隐式行为 — 用户想立刻打断要先点中止再切。

### 6.4 saved_models 被改/删时已活会话

- **活着的 SessionEntry**:手里捏自己的 Provider + model_state 副本,不查 saved_models,完全无感。下拉显示 orphan value(参 5.5),前端插 disabled 灰条提示。
- **磁盘历史**:打开走 5.5 的兜底链。
- **default 引用**:删/改名命中 default 直接 IN_USE_AS_DEFAULT 拒。

### 6.5 config.json 损坏

`load_config` 启动期严格校验(已实现),早 fail。`POST /api/config/default-model` 校验 name 必须存在 → handler 不能制造坏 config。

### 6.6 并发:WebUI 与 TUI 同时改

WebUI 是单例(同一时间只有一个 WebUI 实例),不存在两 WebUI 并发。WebUI + TUI 同时切某条会话的模型 → 顺序执行,**最后一次写覆盖前一次**。前端各自下次 GET 时同步到最终态。**不加锁、不加版本号**。

`save_config` 整文件覆盖,WebUI + TUI 同时改 saved_models 时最坏丢一次修改 — v1 接受。

## 7. 测试覆盖

### 7.1 纯逻辑(`acecode_unit_tests`)

`tests/test_apply_model_to_session.cpp`:

- OpenAI → OpenAI 切换:slot 替换、context_window 更新、sm 调用。
- OpenAI → Copilot:silent_auth 失败填 warning 但成功返回。
- sm/loop 任一为空:不崩(TUI 早期场景)。
- Provider 工厂抛异常:helper 抛 runtime_error,slot 不动。
- cfg 为空:抛 runtime_error("config unavailable")。

`tests/test_saved_models_editor.cpp`:

- 九个错误码每个一例。
- 成功路径:add / update 字段值正确;update 改名走 delete+add;remove 后长度 -1。
- **回滚验证**:add 校验失败时 cfg.saved_models 长度不变。

每个 TEST 中文注释,标场景 / 期望 / 触发原因。

### 7.2 daemon HTTP handler

`tests/test_models_handlers.cpp` 扩:

- `POST /api/models` 成功 200(响应不含 api_key)。
- `POST /api/models` `INVALID_API_KEY` 400。
- `PUT` URL/body name 不一致 400。
- `DELETE` `IN_USE_AS_DEFAULT` 409。
- `POST /api/config/default-model` name 不存在 404。
- `POST /api/sessions/:id/model` session 不存在 404。
- 切换成功后 GET 该 session model 返回新值 + meta 文件已更新(临时目录真落盘)。

### 7.3 TUI 命令

`tests/test_model_command.cpp` 扩:

- `/model` 无参 picker 打开。
- `/model <name>` 命中:slot 真换、状态行/token 条更新。
- `/model unknown`:系统消息提示。
- `/model add` 表单:editor 调用 + cfg 持久化。
- `/model rm <name>`:cfg 持久化,saved_models 减一。
- `/model rm <currentDefault>`:IN_USE_AS_DEFAULT,cfg 不动。

### 7.4 前端单测(Node)

- `lib/errors.js`:error_code 查表 + 退化原文。
- `lib/modelPicker.js`:`isCurrentValueOrphaned` / `buildOptionsWithOrphan`。
- `lib/modelManager.js`:`validateModelDraft` 空字段 / 保留前缀。

### 7.5 手动端到端

CI 不跑,实现完成时手过:

1. WebUI 切模型 + meta 文件落盘验证。
2. TUI + WebUI 联动:TUI 切 → WebUI GET 到新值 → WebUI 切回 → TUI 状态栏刷新。
3. 流式中切:长任务流到一半切 → 当前轮不打断,下一发用新模型。
4. 删默认拒绝:toast 提示 → 改默认后删成功。
5. saved_models 改名后已活会话:disabled 灰条 → 切到新名 → 灰条消失。
6. Copilot silent_auth 失败:删 token → 切 Copilot → toast warning → /login 后正常。

## 8. YAGNI 边界 — 这次不做

- TUI 改造成 daemon HTTP 客户端。
- TUI 多 session。
- "新建会话时弹模型选择" UX。
- saved_models 改名 + 改字段一次完成的合并操作(强制分两次)。
- WebUI 多实例并发改的版本号/乐观锁(单例,不需要)。
- saved_models / config 写盘的进程内并发锁(WebUI + TUI 撞车的概率低,最后一次写覆盖即可)。
- `api_key` 加密存储(沿用 config.json 明文)。

## 9. 关联文件

新增:

- `src/provider/apply_model_to_session.{hpp,cpp}`
- `src/config/saved_models_editor.{hpp,cpp}`
- `web/src/components/ModelManager.jsx`
- `web/src/components/Settings.jsx`
- `web/src/lib/errors.js`
- `web/src/lib/modelPicker.js`
- `web/src/lib/modelManager.js`
- `tests/test_apply_model_to_session.cpp`
- `tests/test_saved_models_editor.cpp`

修改:

- `src/main.cpp` — `ProviderSlot slot;` + 调整 ProviderAccessor 闭包。
- `src/commands/command_registry.hpp` — `CommandContext` 字段从 `provider_handle/provider_mu/provider` 改为 `provider_slot`。
- `src/commands/model_command.cpp` — picker 改 FTXUI;新增 add/edit/rm/set-default 子命令;切换调 apply_model_to_session。
- `src/session/session_registry.cpp` — `switch_model` 改成调 helper 的壳子,删重复实现。
- `src/web/server.cpp` — 注册 POST/PUT/DELETE `/api/models`、`POST /api/config/default-model`。
- `src/web/handlers/models_handler.{hpp,cpp}` — 加 add/update/remove handler 纯逻辑。
- `web/src/components/ModelPicker.jsx` — 接 errors.js + modelPicker.js,失败 toast i18n,orphan value disabled 灰条。
- `web/src/components/Sidebar.jsx`(或其他设置入口宿主)— 加齿轮 → 弹 Settings 抽屉。
- `web/src/lib/api.js` — 加 `addModel` / `updateModel` / `removeModel` / `setDefaultModel`。
- `tests/test_models_handlers.cpp`、`tests/test_model_command.cpp` — 扩。

## 10. 后续可能的扩展(out of scope)

- 模型分组(按 provider / 自定义 tag)。
- 模型探测:输入 base_url + api_key 后端尝试一次 /models 列出可用模型给用户挑。
- 全局 fallback chain:首选模型不可用时自动切下一个。
- saved_models 配额/价格/限速元数据展示(集成 models.dev)。
