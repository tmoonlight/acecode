# 模型选择功能完善 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 ACECode 的 TUI 与 WebUI 共享 per-session 模型切换语义,补足 saved_models 注册表的增删改 UI 与 daemon 端点。

**Architecture:** 抽 `apply_model_to_session` 共享 helper 进 `acecode_testable`,daemon 的 `SessionRegistry::switch_model` 与 TUI 的 `/model` 命令都调它。saved_models 增删改走纯逻辑 editor 模块 + daemon HTTP 端点 + WebUI 设置面板。TUI 的 `ctx.provider_handle/provider_mu` 升级成 `ProviderSlot` 引用。

**Tech Stack:** C++ 17(daemon、TUI),FTXUI(TUI picker/表单),React 18 + Vite + Tailwind v4(WebUI),Crow(HTTP),GoogleTest(C++ 单测),Node 内置 test runner(JS 单测)。

参考设计:`docs/superpowers/specs/2026-05-09-model-selection-design.md`。

---

## Task 1: apply_model_to_session helper(TDD)

把 `SessionRegistry::switch_model` 内部的"创建 provider + 替换 slot + 更新 loop + 写 meta"逻辑抽成纯函数,放进 `acecode_testable`。本任务只创建 helper + 单测,不修改 daemon 调用方(下一任务做)。

**Files:**
- Create: `src/provider/apply_model_to_session.hpp`
- Create: `src/provider/apply_model_to_session.cpp`
- Create: `tests/provider/apply_model_to_session_test.cpp`
- Modify: `src/CMakeLists.txt`(加新 cpp 到 acecode_testable 源列表;若用 GLOB 收集则跳过)

- [ ] **Step 1: 写头文件**

```cpp
// src/provider/apply_model_to_session.hpp
// per-session 模型切换的所有副作用集中在这里。daemon 的 SessionRegistry 与
// TUI 的 /model 命令都调用这个函数,确保两端语义一致。
#pragma once

#include "../config/config.hpp"
#include "../config/saved_models.hpp"
#include "../session/session_client.hpp"  // for SessionModelState
#include "../session/session_registry.hpp"  // for SessionEntry::ProviderSlot

#include <stdexcept>
#include <string>

namespace acecode {

class SessionManager;
class AgentLoop;

struct ApplyModelResult {
    SessionModelState state;
    std::string warning;  // 非致命警告(如 Copilot silent_auth 失败、写 meta 失败)
};

struct ApplyModelDeps {
    SessionEntry::ProviderSlot* provider_slot = nullptr;  // 必填
    SessionManager*             sm = nullptr;             // 选填:TUI 早期可能没有
    AgentLoop*                  loop = nullptr;           // 选填:用于 set_context_window
    AppConfig*                  cfg = nullptr;            // 必填
};

// 失败时抛 std::runtime_error,内容形如:
//   - "config unavailable"        (cfg == nullptr)
//   - "provider slot unavailable" (provider_slot == nullptr)
//   - "provider create failed: <原因>"  (create_provider_from_entry 返回 null)
ApplyModelResult apply_model_to_session(const ModelProfile& profile,
                                         const ApplyModelDeps& deps);

} // namespace acecode
```

- [ ] **Step 2: 写失败测试**

```cpp
// tests/provider/apply_model_to_session_test.cpp
//
// 覆盖 src/provider/apply_model_to_session.cpp。两条调用方(daemon 的
// SessionRegistry::switch_model 与 TUI 的 /model 命令)都靠这一份逻辑,
// 任一分支退化都会让 per-session 切换语义破裂。
//
// 触发场景 / 期望:
//   - cfg 缺失 → 抛 runtime_error,slot.provider 不动(错误前置防止半改)
//   - slot 缺失 → 抛 runtime_error
//   - 正常切换:slot.provider 替换 + state.context_window > 0 + sm 调用
//   - sm/loop 任一为 null:不崩(覆盖 TUI 启动早期场景)

#include <gtest/gtest.h>

#include "provider/apply_model_to_session.hpp"

#include "config/config.hpp"
#include "config/saved_models.hpp"
#include "session/session_registry.hpp"

using acecode::AppConfig;
using acecode::ApplyModelDeps;
using acecode::apply_model_to_session;
using acecode::ModelProfile;
using acecode::SessionEntry;

namespace {

// 构造一个 copilot legacy cfg。create_provider_from_entry 对 copilot 不需要
// 网络访问,适合做单测的 happy path。
AppConfig make_copilot_cfg() {
    AppConfig cfg;
    cfg.provider = "copilot";
    cfg.copilot.model = "gpt-4o";
    cfg.context_window = 128000;
    return cfg;
}

ModelProfile make_copilot_profile(const std::string& model = "gpt-4o-mini") {
    ModelProfile p;
    p.name = "copilot-mini";
    p.provider = "copilot";
    p.model = model;
    return p;
}

} // namespace

// 场景:cfg 为 nullptr → 立刻抛,slot 不被触碰。
// 这是错误前置原则 — caller 给的 deps 残缺时不能让 slot 进入半成功状态。
TEST(ApplyModelToSession, ThrowsWhenCfgMissing) {
    SessionEntry::ProviderSlot slot;
    auto profile = make_copilot_profile();
    ApplyModelDeps deps;
    deps.provider_slot = &slot;
    deps.cfg = nullptr;
    EXPECT_THROW(apply_model_to_session(profile, deps), std::runtime_error);
    EXPECT_FALSE(slot.provider);  // 仍未设置
}

// 场景:slot 为 nullptr → 抛。caller 必须先把 slot 准备好。
TEST(ApplyModelToSession, ThrowsWhenSlotMissing) {
    auto cfg = make_copilot_cfg();
    auto profile = make_copilot_profile();
    ApplyModelDeps deps;
    deps.cfg = &cfg;
    deps.provider_slot = nullptr;
    EXPECT_THROW(apply_model_to_session(profile, deps), std::runtime_error);
}

// 场景:正常切换 copilot 模型 → slot.provider 被设;state 字段填好;无 warning。
// 触发:WebUI / TUI 用户切到一个 copilot 预设。
TEST(ApplyModelToSession, SwapsProviderAndPopulatesState) {
    auto cfg = make_copilot_cfg();
    SessionEntry::ProviderSlot slot;
    auto profile = make_copilot_profile();
    ApplyModelDeps deps;
    deps.cfg = &cfg;
    deps.provider_slot = &slot;
    deps.sm = nullptr;
    deps.loop = nullptr;

    auto result = apply_model_to_session(profile, deps);

    EXPECT_EQ(result.state.name, "copilot-mini");
    EXPECT_EQ(result.state.provider, "copilot");
    EXPECT_EQ(result.state.model, "gpt-4o-mini");
    EXPECT_GT(result.state.context_window, 0);
    EXPECT_FALSE(result.state.is_legacy);
    {
        std::lock_guard<std::mutex> lk(slot.mu);
        ASSERT_TRUE(slot.provider);
        EXPECT_EQ(slot.provider->name(), "copilot");
        EXPECT_EQ(slot.provider->model(), "gpt-4o-mini");
    }
}

// 场景:profile 是 (legacy) 合成 → state.is_legacy = true。回归:UI 层依赖
// is_legacy 标记禁用编辑入口。
TEST(ApplyModelToSession, MarksLegacyState) {
    auto cfg = make_copilot_cfg();
    SessionEntry::ProviderSlot slot;
    ModelProfile legacy;
    legacy.name = "(legacy)";
    legacy.provider = "copilot";
    legacy.model = "gpt-4o";
    ApplyModelDeps deps;
    deps.cfg = &cfg;
    deps.provider_slot = &slot;

    auto result = apply_model_to_session(legacy, deps);
    EXPECT_TRUE(result.state.is_legacy);
}
```

- [ ] **Step 3: 跑测试确认失败**

```bash
cmake --build build --target acecode_unit_tests
ctest --test-dir build -R "ApplyModelToSession" --output-on-failure
```
Expected: 编译失败(`apply_model_to_session.cpp` 还不存在)或链接失败。

- [ ] **Step 4: 写实现**

```cpp
// src/provider/apply_model_to_session.cpp
#include "apply_model_to_session.hpp"

#include "copilot_provider.hpp"
#include "model_context_resolver.hpp"
#include "model_resolver.hpp"
#include "provider_factory.hpp"
#include "../agent_loop.hpp"
#include "../session/session_manager.hpp"
#include "../utils/logger.hpp"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace acecode {

namespace {

AppConfig config_for_profile_context(const AppConfig& cfg,
                                     const ModelProfile& profile) {
    AppConfig c = cfg;
    c.provider = profile.provider;
    if (profile.provider == "openai") {
        c.openai.base_url = profile.base_url;
        c.openai.api_key = profile.api_key;
        c.openai.model = profile.model;
        c.openai.models_dev_provider_id = profile.models_dev_provider_id;
    } else {
        c.copilot.model = profile.model;
    }
    return c;
}

SessionModelState state_from_profile(const AppConfig& cfg,
                                      const ModelProfile& profile) {
    auto context_cfg = config_for_profile_context(cfg, profile);
    SessionModelState state;
    state.name = profile.name;
    state.provider = profile.provider;
    state.model = profile.model;
    state.context_window = resolve_model_context_window(
        context_cfg, profile.provider, profile.model, cfg.context_window);
    state.is_legacy = profile.name == "(legacy)";
    return state;
}

} // namespace

ApplyModelResult apply_model_to_session(const ModelProfile& profile,
                                         const ApplyModelDeps& deps) {
    if (!deps.cfg) throw std::runtime_error("config unavailable");
    if (!deps.provider_slot) throw std::runtime_error("provider slot unavailable");

    ApplyModelResult result;
    result.state = state_from_profile(*deps.cfg, profile);

    auto new_provider = create_provider_from_entry(profile);
    if (!new_provider) {
        throw std::runtime_error("provider create failed: factory returned null for '"
                                 + profile.name + "'");
    }

    if (new_provider->name() == "copilot") {
        if (auto cp = std::dynamic_pointer_cast<CopilotProvider>(new_provider)) {
            if (!cp->try_silent_auth()) {
                result.warning = "Copilot silent_auth failed; user may need /login "
                                 "before next request";
                LOG_WARN("[apply_model_to_session] " + result.warning);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(deps.provider_slot->mu);
        deps.provider_slot->provider = std::move(new_provider);
    }

    if (deps.loop && result.state.context_window > 0) {
        deps.loop->set_context_window(result.state.context_window);
    }

    if (deps.sm) {
        try {
            deps.sm->set_active_provider(result.state.provider,
                                          result.state.model,
                                          result.state.name);
        } catch (const std::exception& e) {
            // meta 写盘失败是非致命的:slot 已替换,用户下一发用新模型;
            // 唯一后果是 daemon 崩了恢复时显示旧模型,用户重切一次即可。
            std::string msg = std::string("meta persist failed: ") + e.what();
            if (result.warning.empty()) result.warning = msg;
            else result.warning += "; " + msg;
            LOG_WARN("[apply_model_to_session] " + msg);
        }
    }

    LOG_INFO("[apply_model_to_session] applied entry='" + profile.name +
             "' (" + result.state.provider + "/" + result.state.model +
             "), context_window=" + std::to_string(result.state.context_window));
    return result;
}

} // namespace acecode
```

- [ ] **Step 5: 跑测试确认通过**

```bash
cmake --build build --target acecode_unit_tests
ctest --test-dir build -R "ApplyModelToSession" --output-on-failure
```
Expected: 4 个 PASS。

- [ ] **Step 6: 跑所有 provider 测试确认无回归**

```bash
ctest --test-dir build -R "Provider|Model" --output-on-failure
```
Expected: 全 PASS,新增 4 个 ApplyModelToSession test。

- [ ] **Step 7: Commit**

```bash
git add src/provider/apply_model_to_session.hpp \
        src/provider/apply_model_to_session.cpp \
        tests/provider/apply_model_to_session_test.cpp
git commit -m "feat: add apply_model_to_session shared helper

抽出 per-session 模型切换的纯逻辑(创建 provider + 替换 slot + 更新 loop +
写 meta),给 daemon 与 TUI 后续共用。本提交只新增 helper + 测试,调用方
切换在后续任务里做。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: SessionRegistry::switch_model 切换到调 helper

把 `src/session/session_registry.cpp::switch_model` 内部"创建 provider + 替换 slot + ..."那段替换成调 `apply_model_to_session`。验证现有 daemon HTTP 行为零退化。

**Files:**
- Modify: `src/session/session_registry.cpp:345-395`(switch_model 实现)

- [ ] **Step 1: 改 switch_model 实现**

打开 `src/session/session_registry.cpp`,把 345-395 行的 `switch_model` 替换为:

```cpp
bool SessionRegistry::switch_model(const std::string& id,
                                   const ModelProfile& profile,
                                   SessionModelState* out,
                                   std::string* error) {
    if (!deps_.config) {
        if (error) *error = "config unavailable";
        return false;
    }

    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    if (it == entries_.end() || !it->second) {
        if (error) *error = "session not found";
        return false;
    }
    auto& entry = *it->second;
    if (!entry.provider_slot) {
        entry.provider_slot = std::make_shared<SessionEntry::ProviderSlot>();
    }

    ApplyModelDeps deps;
    deps.provider_slot = entry.provider_slot.get();
    deps.sm = entry.sm.get();
    deps.loop = entry.loop.get();
    deps.cfg = const_cast<AppConfig*>(deps_.config);

    try {
        auto result = apply_model_to_session(profile, deps);
        entry.model_state = result.state;
        entry.provider = result.state.provider;
        entry.model = result.state.model;
        if (out) *out = result.state;
        if (error && !result.warning.empty()) *error = result.warning;
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}
```

加 include(在文件顶部已有的 include 区):

```cpp
#include "../provider/apply_model_to_session.hpp"
```

注意 `deps_.config` 当前类型是 `const AppConfig*`,helper 要的是 `AppConfig*` — 用 `const_cast` 显式去掉 const,因为 helper 不会写 cfg(只读),但签名为了未来 editor 复用没加 const。

- [ ] **Step 2: 跑现有 SessionRegistry 测试确认未退化**

```bash
cmake --build build --target acecode_unit_tests
ctest --test-dir build -R "SessionRegistry" --output-on-failure
```
Expected: 全 PASS。如果有 fail,检查是否触及 `switch_model` 的边界(error 字符串 / state 字段)。原来"provider unavailable" 错误现已变成 "provider create failed: ...";若测试断言精确字符串需要更新。

- [ ] **Step 3: 跑 web models handler 测试**

```bash
ctest --test-dir build -R "ModelsHandler" --output-on-failure
```
Expected: 全 PASS(handler 路径只调 `registry.switch_model`,内部实现换不影响契约)。

- [ ] **Step 4: 把 resolve_from_profile 标记 deprecated 或删除**

打开 `src/session/session_registry.cpp` 看 `resolve_from_profile`(82-116 行)与 `state_from_profile` / `config_for_profile_context`(54-80 行 namespace 内 helper)是否还被引用。如果只剩 `make_entry_locked` 用 — 保留;如果完全没人用了,删除。本任务不强制删除,留作清理。

```bash
grep -n "resolve_from_profile\|state_from_profile\b\|config_for_profile_context" src/
```
Expected: 只剩 `session_registry.cpp` 内部使用。保留即可。

- [ ] **Step 5: Commit**

```bash
git add src/session/session_registry.cpp
git commit -m "refactor: SessionRegistry::switch_model delegates to shared helper

把 per-session 切换的副作用统一到 apply_model_to_session,daemon 路径
现在与未来的 TUI 路径走同一份代码。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: saved_models_editor 模块(TDD)

纯逻辑 add/update/remove,失败时不修改 cfg(回滚由 caller 负责)。复用 `validate_saved_models` 做最终校验。

**Files:**
- Create: `src/config/saved_models_editor.hpp`
- Create: `src/config/saved_models_editor.cpp`
- Create: `tests/config/saved_models_editor_test.cpp`

- [ ] **Step 1: 写头文件**

```cpp
// src/config/saved_models_editor.hpp
// saved_models 注册表的 add/update/remove 纯逻辑。HTTP handler / TUI 命令
// 都调用这里,失败返错误码,不写盘。
#pragma once

#include "config.hpp"
#include "saved_models.hpp"

#include <optional>
#include <string>

namespace acecode {

struct SavedModelDraft {
    std::string name;
    std::string provider;  // "openai" | "copilot"
    std::string model;
    std::string base_url;  // openai 必填
    std::string api_key;   // openai 必填
    std::optional<std::string> models_dev_provider_id;
};

enum class SavedModelEditError {
    OK,
    INVALID_NAME,         // 空字符串、含控制字符等
    RESERVED_NAME,        // 以 ( 开头(系统占用)
    NAME_TAKEN,           // 新增/改名时撞已有 name
    UNKNOWN_PROVIDER,     // 不是 openai/copilot
    MISSING_MODEL,
    MISSING_BASE_URL,     // openai 必填
    INVALID_API_KEY,      // openai 必填(空字符串触发)
    NOT_FOUND,            // update/remove 时 name 不存在
    IN_USE_AS_DEFAULT,    // remove/改名时该 name 是 cfg.default_model_name
};

const char* to_string(SavedModelEditError e);

// 校验 + 把新条目追加到 cfg.saved_models。OK 时 cfg 已修改(caller 负责
// save_config);非 OK 时 cfg 未变。
SavedModelEditError add_saved_model(AppConfig& cfg, const SavedModelDraft& d);

// 替换 cfg.saved_models 里 name == old_name 的条目为 d。改名(d.name !=
// old_name)走 delete + add 语义;若 old_name 是 default,返 IN_USE_AS_DEFAULT。
SavedModelEditError update_saved_model(AppConfig& cfg,
                                        const std::string& old_name,
                                        const SavedModelDraft& d);

// 删除 cfg.saved_models 里 name == name 的条目。若是 default 返
// IN_USE_AS_DEFAULT。
SavedModelEditError remove_saved_model(AppConfig& cfg, const std::string& name);

} // namespace acecode
```

- [ ] **Step 2: 写失败测试**

```cpp
// tests/config/saved_models_editor_test.cpp
//
// 覆盖 src/config/saved_models_editor.cpp。这是 saved_models 增删改的唯一
// 校验入口(daemon HTTP / TUI 命令都过这里),九个错误码每个都有 UI 文案
// 依赖,任一被绕过都会让坏数据进 cfg.json。
//
// 触发场景 / 期望:
//   - 各类无效 draft → 对应错误码 + cfg 不变(回滚验证)
//   - 成功路径:add / update / remove 后 cfg.saved_models 长度与字段正确
//   - update 改名走 delete+add;若 old_name 是 default → IN_USE_AS_DEFAULT
//   - remove default → IN_USE_AS_DEFAULT,cfg 不变

#include <gtest/gtest.h>

#include "config/saved_models_editor.hpp"

#include "config/config.hpp"
#include "config/saved_models.hpp"

using acecode::AppConfig;
using acecode::add_saved_model;
using acecode::ModelProfile;
using acecode::remove_saved_model;
using acecode::SavedModelDraft;
using acecode::SavedModelEditError;
using acecode::update_saved_model;

namespace {

AppConfig make_cfg_with_one_default() {
    AppConfig cfg;
    cfg.provider = "copilot";
    cfg.copilot.model = "gpt-4o";
    ModelProfile a;
    a.name = "copilot-fast";
    a.provider = "copilot";
    a.model = "gpt-4o";
    cfg.saved_models.push_back(a);
    cfg.default_model_name = "copilot-fast";
    return cfg;
}

SavedModelDraft good_openai_draft(const std::string& name = "local-lm") {
    SavedModelDraft d;
    d.name = name;
    d.provider = "openai";
    d.model = "llama-3";
    d.base_url = "http://localhost:1234/v1";
    d.api_key = "sk-x";
    return d;
}

} // namespace

// 场景:add 一个空名字 → INVALID_NAME,cfg 不变。
TEST(SavedModelsEditor, AddRejectsEmptyName) {
    auto cfg = make_cfg_with_one_default();
    auto before = cfg.saved_models.size();
    SavedModelDraft d = good_openai_draft("");
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::INVALID_NAME);
    EXPECT_EQ(cfg.saved_models.size(), before);
}

// 场景:add 名字以 ( 开头 → RESERVED_NAME。前缀 ( 给 (legacy)/(session:...) 留。
TEST(SavedModelsEditor, AddRejectsReservedNamePrefix) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("(my-fav)");
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::RESERVED_NAME);
}

// 场景:add 重名 → NAME_TAKEN。
TEST(SavedModelsEditor, AddRejectsNameTaken) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("copilot-fast");  // 已存在
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::NAME_TAKEN);
}

// 场景:add 未知 provider → UNKNOWN_PROVIDER。
TEST(SavedModelsEditor, AddRejectsUnknownProvider) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft();
    d.provider = "anthropic";
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::UNKNOWN_PROVIDER);
}

// 场景:add openai 但 model 空 → MISSING_MODEL。
TEST(SavedModelsEditor, AddRejectsMissingModel) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft();
    d.model = "";
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::MISSING_MODEL);
}

// 场景:add openai 但 base_url 空 → MISSING_BASE_URL。
TEST(SavedModelsEditor, AddRejectsMissingBaseUrl) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft();
    d.base_url = "";
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::MISSING_BASE_URL);
}

// 场景:add openai 但 api_key 空 → INVALID_API_KEY(沿用 validate_saved_models 规则)。
TEST(SavedModelsEditor, AddRejectsEmptyApiKey) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft();
    d.api_key = "";
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::INVALID_API_KEY);
}

// 场景:add 成功 → cfg.saved_models 增 1,字段正确。
TEST(SavedModelsEditor, AddAppendsValidDraft) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("local-lm");
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::OK);
    ASSERT_EQ(cfg.saved_models.size(), 2u);
    EXPECT_EQ(cfg.saved_models[1].name, "local-lm");
    EXPECT_EQ(cfg.saved_models[1].base_url, "http://localhost:1234/v1");
    EXPECT_EQ(cfg.saved_models[1].api_key, "sk-x");
}

// 场景:update 不存在的 name → NOT_FOUND,cfg 不变。
TEST(SavedModelsEditor, UpdateRejectsNotFound) {
    auto cfg = make_cfg_with_one_default();
    auto before = cfg.saved_models;
    auto d = good_openai_draft("nope");
    EXPECT_EQ(update_saved_model(cfg, "nope", d), SavedModelEditError::NOT_FOUND);
    EXPECT_EQ(cfg.saved_models.size(), before.size());
}

// 场景:update 改名时旧名是默认 → IN_USE_AS_DEFAULT。
TEST(SavedModelsEditor, UpdateRefusesRenamingDefault) {
    auto cfg = make_cfg_with_one_default();  // default = copilot-fast
    auto d = good_openai_draft("copilot-fast-v2");
    d.provider = "copilot";
    d.base_url.clear();
    d.api_key.clear();
    EXPECT_EQ(update_saved_model(cfg, "copilot-fast", d),
              SavedModelEditError::IN_USE_AS_DEFAULT);
    EXPECT_EQ(cfg.saved_models[0].name, "copilot-fast");  // 未变
}

// 场景:update 同名改字段(非 default 条目) → 字段被替换。
TEST(SavedModelsEditor, UpdateReplacesFieldsForSameName) {
    auto cfg = make_cfg_with_one_default();
    add_saved_model(cfg, good_openai_draft("local-lm"));
    SavedModelDraft d = good_openai_draft("local-lm");
    d.api_key = "sk-new";
    EXPECT_EQ(update_saved_model(cfg, "local-lm", d), SavedModelEditError::OK);
    EXPECT_EQ(cfg.saved_models[1].api_key, "sk-new");
}

// 场景:remove 默认条目 → IN_USE_AS_DEFAULT,cfg 不变。
TEST(SavedModelsEditor, RemoveRefusesDefault) {
    auto cfg = make_cfg_with_one_default();
    EXPECT_EQ(remove_saved_model(cfg, "copilot-fast"),
              SavedModelEditError::IN_USE_AS_DEFAULT);
    EXPECT_EQ(cfg.saved_models.size(), 1u);
}

// 场景:remove 非默认 → 长度 -1。
TEST(SavedModelsEditor, RemoveDeletesNonDefault) {
    auto cfg = make_cfg_with_one_default();
    add_saved_model(cfg, good_openai_draft("local-lm"));
    EXPECT_EQ(remove_saved_model(cfg, "local-lm"), SavedModelEditError::OK);
    EXPECT_EQ(cfg.saved_models.size(), 1u);
    EXPECT_EQ(cfg.saved_models[0].name, "copilot-fast");
}

// 场景:remove 不存在 → NOT_FOUND。
TEST(SavedModelsEditor, RemoveRejectsNotFound) {
    auto cfg = make_cfg_with_one_default();
    EXPECT_EQ(remove_saved_model(cfg, "ghost"), SavedModelEditError::NOT_FOUND);
}
```

- [ ] **Step 3: 跑测试确认失败**

```bash
cmake --build build --target acecode_unit_tests
ctest --test-dir build -R "SavedModelsEditor" --output-on-failure
```
Expected: 编译失败(`saved_models_editor.cpp` 不存在)。

- [ ] **Step 4: 写实现**

```cpp
// src/config/saved_models_editor.cpp
#include "saved_models_editor.hpp"

#include <algorithm>

namespace acecode {

const char* to_string(SavedModelEditError e) {
    switch (e) {
        case SavedModelEditError::OK:                return "OK";
        case SavedModelEditError::INVALID_NAME:      return "INVALID_NAME";
        case SavedModelEditError::RESERVED_NAME:     return "RESERVED_NAME";
        case SavedModelEditError::NAME_TAKEN:        return "NAME_TAKEN";
        case SavedModelEditError::UNKNOWN_PROVIDER:  return "UNKNOWN_PROVIDER";
        case SavedModelEditError::MISSING_MODEL:     return "MISSING_MODEL";
        case SavedModelEditError::MISSING_BASE_URL:  return "MISSING_BASE_URL";
        case SavedModelEditError::INVALID_API_KEY:   return "INVALID_API_KEY";
        case SavedModelEditError::NOT_FOUND:         return "NOT_FOUND";
        case SavedModelEditError::IN_USE_AS_DEFAULT: return "IN_USE_AS_DEFAULT";
    }
    return "UNKNOWN";
}

namespace {

SavedModelEditError validate_draft_basic(const SavedModelDraft& d) {
    if (d.name.empty()) return SavedModelEditError::INVALID_NAME;
    if (d.name.front() == '(') return SavedModelEditError::RESERVED_NAME;
    if (d.provider != "openai" && d.provider != "copilot")
        return SavedModelEditError::UNKNOWN_PROVIDER;
    if (d.model.empty()) return SavedModelEditError::MISSING_MODEL;
    if (d.provider == "openai") {
        if (d.base_url.empty()) return SavedModelEditError::MISSING_BASE_URL;
        if (d.api_key.empty()) return SavedModelEditError::INVALID_API_KEY;
    }
    return SavedModelEditError::OK;
}

ModelProfile to_profile(const SavedModelDraft& d) {
    ModelProfile p;
    p.name = d.name;
    p.provider = d.provider;
    p.model = d.model;
    if (d.provider == "openai") {
        p.base_url = d.base_url;
        p.api_key = d.api_key;
    }
    p.models_dev_provider_id = d.models_dev_provider_id;
    return p;
}

bool name_exists(const AppConfig& cfg, const std::string& name) {
    for (const auto& e : cfg.saved_models) if (e.name == name) return true;
    return false;
}

} // namespace

SavedModelEditError add_saved_model(AppConfig& cfg, const SavedModelDraft& d) {
    if (auto err = validate_draft_basic(d); err != SavedModelEditError::OK) return err;
    if (name_exists(cfg, d.name)) return SavedModelEditError::NAME_TAKEN;
    cfg.saved_models.push_back(to_profile(d));
    return SavedModelEditError::OK;
}

SavedModelEditError update_saved_model(AppConfig& cfg,
                                        const std::string& old_name,
                                        const SavedModelDraft& d) {
    auto it = std::find_if(cfg.saved_models.begin(), cfg.saved_models.end(),
                            [&](const ModelProfile& e) { return e.name == old_name; });
    if (it == cfg.saved_models.end()) return SavedModelEditError::NOT_FOUND;

    // 改名:必须不与 default 冲突,新名也要走完整校验且不能撞别的现有 name。
    const bool renaming = d.name != old_name;
    if (renaming && cfg.default_model_name == old_name) {
        return SavedModelEditError::IN_USE_AS_DEFAULT;
    }
    if (auto err = validate_draft_basic(d); err != SavedModelEditError::OK) return err;
    if (renaming) {
        if (name_exists(cfg, d.name)) return SavedModelEditError::NAME_TAKEN;
    }

    *it = to_profile(d);
    return SavedModelEditError::OK;
}

SavedModelEditError remove_saved_model(AppConfig& cfg, const std::string& name) {
    if (cfg.default_model_name == name) return SavedModelEditError::IN_USE_AS_DEFAULT;
    auto it = std::find_if(cfg.saved_models.begin(), cfg.saved_models.end(),
                            [&](const ModelProfile& e) { return e.name == name; });
    if (it == cfg.saved_models.end()) return SavedModelEditError::NOT_FOUND;
    cfg.saved_models.erase(it);
    return SavedModelEditError::OK;
}

} // namespace acecode
```

- [ ] **Step 5: 跑测试确认通过**

```bash
cmake --build build --target acecode_unit_tests
ctest --test-dir build -R "SavedModelsEditor" --output-on-failure
```
Expected: 14 个 PASS。

- [ ] **Step 6: Commit**

```bash
git add src/config/saved_models_editor.hpp \
        src/config/saved_models_editor.cpp \
        tests/config/saved_models_editor_test.cpp
git commit -m "feat: add saved_models_editor module

纯逻辑 add/update/remove for saved_models 注册表。九个错误码覆盖每个失
败分支,失败时 cfg 不变。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: daemon HTTP — saved_models 增删改 + 默认设置端点

把 editor 模块包装成 4 个新端点,handler 拆纯逻辑放 `models_handler.{hpp,cpp}` 便于单测。

**Files:**
- Modify: `src/web/handlers/models_handler.hpp`
- Modify: `src/web/handlers/models_handler.cpp`
- Modify: `src/web/server.cpp`(注册新路由)
- Modify: `tests/web/models_handler_test.cpp`

- [ ] **Step 1: 扩 models_handler.hpp**

在 `src/web/handlers/models_handler.hpp` 末尾(`}` 之前)加:

```cpp
// 把 SavedModelEditError 映射到 HTTP 状态码。
// 200 不触发(成功路径不调这个);其它分支:
//   - NOT_FOUND          → 404
//   - NAME_TAKEN / IN_USE_AS_DEFAULT → 409
//   - 其它(校验失败)   → 400
int http_status_for_edit_error(SavedModelEditError e);

// 把 SavedModelDraft 序列化到 ModelProfile JSON 格式(api_key 字段总是省略)。
// 给 POST/PUT 成功响应用。
nlohmann::json profile_to_safe_json(const ModelProfile& entry);

// 解析 POST/PUT body 到 SavedModelDraft。失败返 nullopt + err 写错误说明。
std::optional<SavedModelDraft> parse_model_draft(const nlohmann::json& body,
                                                  std::string& err);
```

引入头(放头文件 includes 区):

```cpp
#include "../../config/saved_models_editor.hpp"
```

- [ ] **Step 2: 实现这三个 helper**

打开 `src/web/handlers/models_handler.cpp`,在 `model_state_to_json` 之后追加:

```cpp
int http_status_for_edit_error(SavedModelEditError e) {
    switch (e) {
        case SavedModelEditError::OK: return 200;
        case SavedModelEditError::NOT_FOUND: return 404;
        case SavedModelEditError::NAME_TAKEN:
        case SavedModelEditError::IN_USE_AS_DEFAULT: return 409;
        default: return 400;
    }
}

nlohmann::json profile_to_safe_json(const ModelProfile& entry) {
    nlohmann::json o;
    o["name"]      = entry.name;
    o["provider"]  = entry.provider;
    o["model"]     = entry.model;
    o["is_legacy"] = entry.name == "(legacy)";
    if (!entry.base_url.empty()) o["base_url"] = entry.base_url;
    if (entry.models_dev_provider_id.has_value()) {
        o["models_dev_provider_id"] = *entry.models_dev_provider_id;
    }
    // api_key intentionally omitted
    return o;
}

std::optional<SavedModelDraft> parse_model_draft(const nlohmann::json& body,
                                                  std::string& err) {
    if (!body.is_object()) { err = "body must be a JSON object"; return std::nullopt; }
    SavedModelDraft d;
    auto get_str = [&](const char* key, std::string& out, bool required) {
        if (!body.contains(key)) { if (required) err = std::string("missing field '") + key + "'"; return; }
        if (!body[key].is_string()) { err = std::string("field '") + key + "' must be string"; return; }
        out = body[key].get<std::string>();
    };
    get_str("name", d.name, true);
    get_str("provider", d.provider, true);
    get_str("model", d.model, true);
    get_str("base_url", d.base_url, false);
    get_str("api_key", d.api_key, false);
    if (body.contains("models_dev_provider_id") &&
        body["models_dev_provider_id"].is_string()) {
        std::string s = body["models_dev_provider_id"].get<std::string>();
        if (!s.empty()) d.models_dev_provider_id = std::move(s);
    }
    if (!err.empty()) return std::nullopt;
    return d;
}
```

并加 include:

```cpp
#include <stdexcept>
```

- [ ] **Step 3: 注册 Crow 路由**

打开 `src/web/server.cpp`,在 `register_models()`(约 1725 行)的 GET /api/models 之后追加 OPTIONS preflight + POST/PUT/DELETE/默认端点。在 `register_models` 体内添加:

```cpp
        CROW_ROUTE(app, "/api/models").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            json body;
            try { body = json::parse(req.body); }
            catch (...) { return json_error(400, "BAD_JSON", "invalid JSON body"); }
            std::string err;
            auto draft = parse_model_draft(body, err);
            if (!draft) return json_error(400, "BAD_REQUEST", err);
            auto rc = add_saved_model(deps.config_mut, *draft);
            if (rc != SavedModelEditError::OK) {
                return json_error(http_status_for_edit_error(rc), to_string(rc),
                                   "saved_models add rejected");
            }
            try { save_config(deps.config_mut); }
            catch (const std::exception& e) {
                deps.config_mut.saved_models.pop_back();  // 回滚
                return json_error(500, "PERSIST_FAILED", e.what());
            }
            crow::response r(200);
            r.set_header("Content-Type", "application/json");
            r.body = profile_to_safe_json(deps.config_mut.saved_models.back()).dump();
            return cors_apply(r, req);
        });

        CROW_ROUTE(app, "/api/models/<string>").methods(crow::HTTPMethod::Put)
        ([this](const crow::request& req, const std::string& url_name) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            json body;
            try { body = json::parse(req.body); }
            catch (...) { return json_error(400, "BAD_JSON", "invalid JSON body"); }
            std::string err;
            auto draft = parse_model_draft(body, err);
            if (!draft) return json_error(400, "BAD_REQUEST", err);
            // 把改前快照存好,save_config 失败时整体回滚。
            auto snapshot = deps.config_mut.saved_models;
            auto rc = update_saved_model(deps.config_mut, url_name, *draft);
            if (rc != SavedModelEditError::OK) {
                return json_error(http_status_for_edit_error(rc), to_string(rc),
                                   "saved_models update rejected");
            }
            try { save_config(deps.config_mut); }
            catch (const std::exception& e) {
                deps.config_mut.saved_models = std::move(snapshot);
                return json_error(500, "PERSIST_FAILED", e.what());
            }
            crow::response r(200);
            r.set_header("Content-Type", "application/json");
            for (const auto& e : deps.config_mut.saved_models) {
                if (e.name == draft->name) { r.body = profile_to_safe_json(e).dump(); break; }
            }
            return cors_apply(r, req);
        });

        CROW_ROUTE(app, "/api/models/<string>").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req, const std::string& url_name) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto snapshot = deps.config_mut.saved_models;
            auto rc = remove_saved_model(deps.config_mut, url_name);
            if (rc != SavedModelEditError::OK) {
                return json_error(http_status_for_edit_error(rc), to_string(rc),
                                   "saved_models remove rejected");
            }
            try { save_config(deps.config_mut); }
            catch (const std::exception& e) {
                deps.config_mut.saved_models = std::move(snapshot);
                return json_error(500, "PERSIST_FAILED", e.what());
            }
            crow::response r(200);
            r.set_header("Content-Type", "application/json");
            r.body = json{{"ok", true}}.dump();
            return cors_apply(r, req);
        });

        CROW_ROUTE(app, "/api/config/default-model").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            json body;
            try { body = json::parse(req.body); }
            catch (...) { return json_error(400, "BAD_JSON", "invalid JSON body"); }
            if (!body.is_object() || !body.contains("name") || !body["name"].is_string()) {
                return json_error(400, "BAD_REQUEST", "expected {name: string}");
            }
            std::string name = body["name"].get<std::string>();
            // 校验 name 必须存在(saved_models 或 (legacy))
            bool found = (name == "(legacy)");
            if (!found) {
                for (const auto& e : deps.config_mut.saved_models) {
                    if (e.name == name) { found = true; break; }
                }
            }
            if (!found) return json_error(404, "NOT_FOUND", "no such model name");

            std::string before = deps.config_mut.default_model_name;
            deps.config_mut.default_model_name = name;
            try { save_config(deps.config_mut); }
            catch (const std::exception& e) {
                deps.config_mut.default_model_name = before;
                return json_error(500, "PERSIST_FAILED", e.what());
            }
            crow::response r(200);
            r.set_header("Content-Type", "application/json");
            r.body = json{{"default_model_name", name}}.dump();
            return cors_apply(r, req);
        });
```

注意:`deps.config_mut` 是 `AppConfig&` 风格的 mutable 引用 — 看现有 server.cpp 里其它端点(如 `/api/skills`)是怎么取 mutable cfg 的,沿用同名访问。如果当前 deps 只有 const config 引用,需要扩 `WebServerDeps` 加 `AppConfig& config_mut`(在 `worker.cpp::run_worker` 里传 `cfg`)。

`json_error` 是 server.cpp 里现有的 helper(查一下 spelling — 也可能叫 `make_error_response` 之类)。统一用 `{"error": "<code>", "message": "<msg>"}` body 格式。

`save_config` 是 `src/config/config.cpp` 的现有函数,声明在 `config.hpp`。

需要的 include 在 server.cpp 顶部加:

```cpp
#include "../config/saved_models_editor.hpp"
```

- [ ] **Step 4: 扩 models handler 单测**

在 `tests/web/models_handler_test.cpp` 末尾追加:

```cpp
// ------------------- 增删改 helper 测试 -------------------

#include "config/saved_models_editor.hpp"

using acecode::add_saved_model;
using acecode::SavedModelDraft;
using acecode::SavedModelEditError;
using acecode::web::http_status_for_edit_error;
using acecode::web::parse_model_draft;
using acecode::web::profile_to_safe_json;

namespace {

// 把 SavedModelEditError 映射到 HTTP 状态码 — 这层映射跟前端 toast 文案
// 强相关,任一改错都会让"删默认"这种 UX 走偏(比如本来要 409 变成 500
// 用户看不到"先去改默认"提示)。
TEST(ModelsHandler, ErrorToHttpStatusMapping) {
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::NOT_FOUND), 404);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::NAME_TAKEN), 409);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::IN_USE_AS_DEFAULT), 409);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::INVALID_NAME), 400);
    EXPECT_EQ(http_status_for_edit_error(SavedModelEditError::INVALID_API_KEY), 400);
}

// 场景:profile_to_safe_json 永远不带 api_key 字段。这是 spec 的安全契约,
// 回归就泄露 key。
TEST(ModelsHandler, ProfileToSafeJsonOmitsApiKey) {
    ModelProfile p;
    p.name = "local";
    p.provider = "openai";
    p.model = "llama-3";
    p.base_url = "http://localhost/v1";
    p.api_key = "sk-secret";
    auto j = profile_to_safe_json(p);
    EXPECT_FALSE(j.contains("api_key"));
    EXPECT_EQ(j["base_url"], "http://localhost/v1");
}

// 场景:parse_model_draft 漏字段 → 错误信息明确指出哪个字段。
TEST(ModelsHandler, ParseDraftReportsMissingField) {
    nlohmann::json body = {{"name", "x"}};
    std::string err;
    auto d = parse_model_draft(body, err);
    EXPECT_FALSE(d.has_value());
    EXPECT_NE(err.find("provider"), std::string::npos);
}

// 场景:parse_model_draft 完整 body → 全部字段就绪。
TEST(ModelsHandler, ParseDraftAcceptsFullBody) {
    nlohmann::json body = {
        {"name", "local"}, {"provider", "openai"}, {"model", "llama-3"},
        {"base_url", "http://localhost/v1"}, {"api_key", "sk-x"},
    };
    std::string err;
    auto d = parse_model_draft(body, err);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->name, "local");
    EXPECT_EQ(d->base_url, "http://localhost/v1");
    EXPECT_EQ(d->api_key, "sk-x");
    EXPECT_TRUE(err.empty());
}

} // namespace
```

- [ ] **Step 5: 跑测试确认通过**

```bash
cmake --build build --target acecode_unit_tests
ctest --test-dir build -R "ModelsHandler" --output-on-failure
```
Expected: 全 PASS,新增 4 个测试。

- [ ] **Step 6: 跑 daemon smoke 测试 + 手动 curl 验证**

```bash
ctest --test-dir build -R "WebServerSmoke" --output-on-failure
```
Expected: PASS(端点注册不影响 smoke)。

启动 daemon:`./build/acecode daemon start`,然后:

```bash
# add
curl -X POST http://127.0.0.1:28080/api/models -H "Content-Type: application/json" \
     -d '{"name":"test-lm","provider":"openai","model":"llama-3","base_url":"http://localhost:1234/v1","api_key":"sk-x"}'
# expect 200, response 不含 api_key

# repeat → 409 NAME_TAKEN
curl -X POST http://127.0.0.1:28080/api/models -H "Content-Type: application/json" \
     -d '{"name":"test-lm","provider":"openai","model":"llama-3","base_url":"http://localhost:1234/v1","api_key":"sk-x"}'

# delete
curl -X DELETE http://127.0.0.1:28080/api/models/test-lm
# expect 200

# set default to non-existent → 404
curl -X POST http://127.0.0.1:28080/api/config/default-model -H "Content-Type: application/json" \
     -d '{"name":"ghost"}'
```

- [ ] **Step 7: Commit**

```bash
git add src/web/handlers/models_handler.hpp \
        src/web/handlers/models_handler.cpp \
        src/web/server.cpp \
        tests/web/models_handler_test.cpp
git commit -m "feat: daemon endpoints for saved_models CRUD + default-model

POST/PUT/DELETE /api/models 与 POST /api/config/default-model;失败时
内存回滚保证 config.json 不会半保存。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: TUI provider_slot 升级

把 `main.cpp` 的 `std::shared_ptr<LlmProvider> provider + std::mutex provider_mu` 改成 `SessionEntry::ProviderSlot`,把 `CommandContext` 的 `provider_handle/provider_mu` 替换为 `provider_slot`。**只改字段类型,不改业务行为** — model_command 与 builtin_commands 仍走旧的 `swap_provider_if_needed`,留在下一任务换成 helper。

**Files:**
- Modify: `src/commands/command_registry.hpp:33-37`(CommandContext 字段)
- Modify: `main.cpp:1269-1274`(provider/provider_mu 定义 → ProviderSlot)
- Modify: `main.cpp:1743`(swap_provider_if_needed 调用,适配新签名 — 见 step 3 的临时桥)
- Modify: `main.cpp:1948+2607`(CommandContext 构造)
- Modify: `src/commands/model_command.cpp:158-166`(swap_provider_if_needed 调用)
- Modify: `src/commands/builtin_commands.cpp:570-577`(swap_provider_if_needed + set_active_provider)

- [ ] **Step 1: 改 CommandContext 字段定义**

`src/commands/command_registry.hpp`,把 32-36 行的:

```cpp
    LlmProvider& provider;
    std::shared_ptr<LlmProvider>* provider_handle = nullptr;
    std::mutex* provider_mu = nullptr;
```

替换为:

```cpp
    LlmProvider& provider;
    // ProviderSlot 持有一个 shared_ptr + mutex。/model 等切换命令用它做 swap。
    // 命令体内的只读访问仍走 provider 引用(dispatch 时已 lock+deref 过)。
    SessionEntry::ProviderSlot* provider_slot = nullptr;
```

加 include:

```cpp
#include "../session/session_registry.hpp"
```

- [ ] **Step 2: 改 main.cpp 的 provider 创建代码**

`main.cpp:1269-1274` 替换为:

```cpp
    SessionEntry::ProviderSlot provider_slot;
    {
        std::lock_guard<std::mutex> lk(provider_slot.mu);
        provider_slot.provider = create_provider_from_entry(effective_entry);
    }
    auto provider_accessor = [&provider_slot]() -> std::shared_ptr<LlmProvider> {
        std::lock_guard<std::mutex> lk(provider_slot.mu);
        return provider_slot.provider;
    };
```

并加 include(顶部 include 区,如果没有):

```cpp
#include "session/session_registry.hpp"
```

- [ ] **Step 3: dispatch 处构造 CommandContext 时改字段名**

`main.cpp:2607` 附近的 CommandContext 构造,把 `*provider, &provider, &provider_mu,` 改为:

```cpp
                state, agent_loop, *provider_accessor(), &provider_slot,
```

(留意 `*provider_accessor()` — 在 dispatch 时 deref 出当前 shared_ptr 指向的实例,引用稳定到这次 dispatch 结束;这就是 spec 4.2 描述的"snapshot at command dispatch time"。)

`main.cpp:1948` 附近的 `CatchEvent` lambda capture list 删掉 `&provider, &provider_mu`,加 `&provider_slot`(如果该 lambda 内部不需要直接访问 provider 就只删,加是为了保留对 provider_slot 的访问;实际看那段代码用了什么)。检查方法:

```bash
grep -n "provider\b" main.cpp | head -40
```

凡是直接读 `provider->xxx()` 的调点,都改成 `provider_accessor()->xxx()`(每次访问都拿一个 shared_ptr 副本,deref)。直接 `*provider` 形式同样改成 `*provider_accessor()`。

- [ ] **Step 4: 改 swap_provider_if_needed 的调用方(临时兼容)**

`provider_swap.hpp` 现有签名:

```cpp
void swap_provider_if_needed(std::shared_ptr<LlmProvider>& handle,
                             std::mutex& mu,
                             const ModelProfile& entry,
                             AppConfig& cfg);
```

我们**不改这个签名**(下一任务直接弃用它),只在 caller 把 `provider_slot.provider` 与 `provider_slot.mu` 传进去:

`main.cpp:1743`(--resume 路径):

```cpp
                swap_provider_if_needed(provider_slot.provider, provider_slot.mu,
                                         resumed_entry, config);
```

`src/commands/model_command.cpp:160`:

```cpp
    if (ctx.provider_slot) {
        swap_provider_if_needed(ctx.provider_slot->provider,
                                  ctx.provider_slot->mu, *entry, ctx.config);
    } else {
        ctx.provider.set_model(entry->model);
        ctx.config.context_window = resolve_model_context_window(
            ctx.config, ctx.provider.name(), ctx.provider.model(),
            ctx.config.context_window);
    }
```

`src/commands/builtin_commands.cpp:570-577`:

```cpp
    if (target && ctx.provider_slot &&
        !target->provider.empty() && !target->model.empty()) {
        ModelProfile resumed_entry;
        // ... 原有 resumed_entry 构造逻辑 ...
        swap_provider_if_needed(ctx.provider_slot->provider,
                                  ctx.provider_slot->mu, resumed_entry, ctx.config);
        ctx.session_manager->set_active_provider(
            ctx.provider_slot->provider->name(),
            ctx.provider_slot->provider->model());
    }
```

(原代码上下文要保留,只把 `*ctx.provider_handle` / `*ctx.provider_mu` 替换为 `ctx.provider_slot->provider` / `ctx.provider_slot->mu`。)

- [ ] **Step 5: 编译 + 跑全量单测**

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: 全 PASS。如果 CommandContext 测试桩(`tests/commands/`)失败,把它们的构造也加 `provider_slot` 字段(传 nullptr,旧代码路径走 `else` 分支)。

- [ ] **Step 6: 手动 TUI smoke**

```bash
./build/acecode
# 进入 TUI,试 /info 看 model 字段正常显示
# 退出
```
Expected: TUI 正常启动,/info 显示当前 provider/model。

- [ ] **Step 7: Commit**

```bash
git add src/commands/command_registry.hpp \
        main.cpp \
        src/commands/model_command.cpp \
        src/commands/builtin_commands.cpp
git commit -m "refactor(tui): replace provider_handle/provider_mu with ProviderSlot

CommandContext 与 main.cpp 升级到 SessionEntry::ProviderSlot。本提交不改
切换语义,/model 与 resume 仍走 swap_provider_if_needed,下一提交切到
apply_model_to_session。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: TUI /model 切换走 apply_model_to_session

把 model_command.cpp 与 builtin_commands.cpp 的 `swap_provider_if_needed` 调用换成 `apply_model_to_session`。这样 TUI 与 daemon 共用同一份切换逻辑;`provider_swap.{hpp,cpp}` 可以删除。

**Files:**
- Modify: `src/commands/model_command.cpp`
- Modify: `src/commands/builtin_commands.cpp`
- Delete: `src/provider/provider_swap.hpp`
- Delete: `src/provider/provider_swap.cpp`
- Modify: `tests/commands/`(若有)的 model_command 测试

- [ ] **Step 1: 改 model_command.cpp 调 helper**

`src/commands/model_command.cpp` 的 `cmd_model` 函数,把 `swap_provider_if_needed` 调用替换为:

```cpp
    if (ctx.provider_slot) {
        ApplyModelDeps deps;
        deps.provider_slot = ctx.provider_slot;
        deps.sm = ctx.session_manager;
        deps.loop = &ctx.agent_loop;
        deps.cfg = &ctx.config;
        try {
            auto result = apply_model_to_session(*entry, deps);
            ctx.config.context_window = result.state.context_window;
            if (!result.warning.empty()) {
                std::lock_guard<std::mutex> lk(ctx.state.mu);
                ctx.state.conversation.push_back({"system", "Warning: " + result.warning, false});
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(ctx.state.mu);
            ctx.state.conversation.push_back({"system",
                std::string("Switch failed: ") + e.what(), false});
            ctx.state.chat_follow_tail = true;
            return;
        }
    } else {
        // ProviderSlot 缺失(测试桩场景):仅 set_model 兜底。
        ctx.provider.set_model(entry->model);
        ctx.config.context_window = resolve_model_context_window(
            ctx.config, ctx.provider.name(), ctx.provider.model(),
            ctx.config.context_window);
    }
```

调整 includes,删掉 `#include "../provider/provider_swap.hpp"`,加:

```cpp
#include "../provider/apply_model_to_session.hpp"
```

- [ ] **Step 2: 改 builtin_commands.cpp resume 路径**

打开 `src/commands/builtin_commands.cpp`,找到 568-577 行的 resume swap 段,替换为:

```cpp
    if (target && ctx.provider_slot &&
        !target->provider.empty() && !target->model.empty()) {
        ModelProfile resumed_entry;
        // ... 保留原有 resumed_entry 构造 ...
        ApplyModelDeps deps;
        deps.provider_slot = ctx.provider_slot;
        deps.sm = ctx.session_manager;
        deps.loop = &ctx.agent_loop;
        deps.cfg = &ctx.config;
        try {
            auto result = apply_model_to_session(resumed_entry, deps);
            ctx.config.context_window = result.state.context_window;
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[/resume] model switch failed: ") + e.what());
        }
    }
```

includes:删 `provider_swap.hpp`,加 `apply_model_to_session.hpp`。

- [ ] **Step 3: 删 provider_swap.{hpp,cpp}**

```bash
git rm src/provider/provider_swap.hpp src/provider/provider_swap.cpp
```

如果 CMake 用 GLOB 跳过;如果是显式列表,改 `src/CMakeLists.txt` 删两行。

- [ ] **Step 4: 跑全量单测**

```bash
cmake --build build --target acecode_unit_tests
ctest --test-dir build --output-on-failure
```
Expected: 全 PASS。如果有依赖 `swap_provider_if_needed` 的测试,改成调 `apply_model_to_session`。

- [ ] **Step 5: 手动 TUI 切换验证**

```bash
./build/acecode --resume <一个旧 session id>
# /model
# /model copilot-fast
# 验证:状态行更新,token 计数条按新窗口重画,输入新消息用新模型回应
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(tui): /model & /resume use apply_model_to_session

TUI 与 daemon 现在共用同一份切换 helper,provider_swap 模块删除。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: TUI /model FTXUI picker

把 `render_model_picker`(目前 push 一条 system 文本)替换为 FTXUI Modal 选择器,样式参考已有的 `/skills` picker(`src/tui/skills_picker.{hpp,cpp}` 之类)。

**Files:**
- Create: `src/tui/model_picker.hpp`
- Create: `src/tui/model_picker.cpp`
- Modify: `src/commands/model_command.cpp`(`render_model_picker` 改成 open modal)
- Modify: `src/tui_state.hpp` 或类似(加 `model_picker_open` flag + 选项数据)
- Modify: `main.cpp`(渲染主循环挂 modal)

- [ ] **Step 1: 找现有 picker 参考实现**

```bash
grep -rn "Modal\|picker_open" src/tui/ src/commands/ | head -30
```

记录一个相似的现有 picker(skills 或 configure)的:① state 结构、② Modal 渲染挂载点、③ 选中回调路径。新 model picker 照抄结构。

- [ ] **Step 2: 写 model_picker.{hpp,cpp}**

```cpp
// src/tui/model_picker.hpp
#pragma once

#include "../config/config.hpp"
#include "../config/saved_models.hpp"

#include <ftxui/component/component.hpp>

#include <functional>
#include <string>
#include <vector>

namespace acecode {

struct ModelPickerOption {
    std::string name;        // saved_models name 或 "(legacy)"
    std::string provider;
    std::string model;
    bool        is_current = false;
};

// 构造选项列表:cfg.saved_models 顺序 + 末尾追加 (legacy);current_name
// 命中的行 is_current=true。
std::vector<ModelPickerOption> build_model_picker_options(
    const AppConfig& cfg, const std::string& current_name);

// 创建一个 FTXUI 选择器组件。on_pick(name) 在用户 Enter 时被调,Esc 关闭。
// 返回 Component 让 main 渲染主循环包到 Modal 里。
ftxui::Component make_model_picker_component(
    const std::vector<ModelPickerOption>& options,
    std::function<void(const std::string&)> on_pick,
    std::function<void()> on_cancel);

} // namespace acecode
```

实现:

```cpp
// src/tui/model_picker.cpp
#include "model_picker.hpp"

#include "../provider/model_resolver.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

namespace acecode {

std::vector<ModelPickerOption> build_model_picker_options(
    const AppConfig& cfg, const std::string& current_name) {
    std::vector<ModelPickerOption> out;
    bool legacy_in_saved = false;
    for (const auto& e : cfg.saved_models) {
        ModelPickerOption o;
        o.name = e.name;
        o.provider = e.provider;
        o.model = e.model;
        o.is_current = (e.name == current_name);
        if (e.name == "(legacy)") legacy_in_saved = true;
        out.push_back(std::move(o));
    }
    if (!legacy_in_saved) {
        auto legacy = synth_legacy_entry(cfg);
        ModelPickerOption o;
        o.name = legacy.name;
        o.provider = legacy.provider;
        o.model = legacy.model;
        o.is_current = (legacy.name == current_name);
        out.push_back(std::move(o));
    }
    return out;
}

ftxui::Component make_model_picker_component(
    const std::vector<ModelPickerOption>& options,
    std::function<void(const std::string&)> on_pick,
    std::function<void()> on_cancel) {
    using namespace ftxui;
    auto selected = std::make_shared<int>(0);
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (options[i].is_current) { *selected = static_cast<int>(i); break; }
    }

    auto labels = std::make_shared<std::vector<std::string>>();
    for (const auto& o : options) {
        labels->push_back((o.is_current ? "* " : "  ") + o.name +
                          " (" + o.provider + "/" + o.model + ")");
    }

    auto menu = Menu(labels.get(), selected.get());

    return CatchEvent(menu, [=](Event ev) {
        if (ev == Event::Return) {
            if (*selected >= 0 && *selected < (int)options.size()) {
                on_pick(options[*selected].name);
            }
            return true;
        }
        if (ev == Event::Escape) { on_cancel(); return true; }
        return false;
    });
}

} // namespace acecode
```

- [ ] **Step 3: TuiState 加 picker open flag + 选项**

`src/tui_state.hpp` 加(在合适分组):

```cpp
    // /model picker 状态
    bool                                model_picker_open = false;
    std::vector<ModelPickerOption>      model_picker_options;
```

include `tui/model_picker.hpp`。

- [ ] **Step 4: 改 render_model_picker 改成 open modal**

`src/commands/model_command.cpp:render_model_picker` 替换为:

```cpp
void render_model_picker(CommandContext& ctx) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    ctx.state.model_picker_options =
        build_model_picker_options(ctx.config, current_effective_name(ctx));
    ctx.state.model_picker_open = true;
    if (ctx.post_event) ctx.post_event();
}
```

加 include `#include "../tui/model_picker.hpp"`。

- [ ] **Step 5: main.cpp 把 picker 挂到主渲染器**

在 main.cpp 的主 Component 组合处,Modal 挂载点(参考 skills picker 的 Modal 用法),加:

```cpp
auto model_picker_modal = make_model_picker_component(
    state.model_picker_options,
    [&](const std::string& name) {
        // 关 modal,模拟用户输入 /model <name>
        state.model_picker_open = false;
        std::string cmd = "/model " + name;
        cmd_registry.dispatch(cmd, ctx);
    },
    [&]() { state.model_picker_open = false; });

// 在主 Renderer 里:
return state.model_picker_open
    ? Render(content_with_modal_overlay(model_picker_modal))
    : Render(content);
```

具体挂载语法依现有 skills picker 写法调整。

- [ ] **Step 6: 编译 + 手动验证**

```bash
cmake --build build
./build/acecode
# /model
# 看到 modal,↑↓ 选,Enter 切换
# Esc 关闭
```

- [ ] **Step 7: Commit**

```bash
git add src/tui/model_picker.hpp src/tui/model_picker.cpp \
        src/tui_state.hpp src/commands/model_command.cpp main.cpp
git commit -m "feat(tui): replace /model text list with FTXUI picker

/model 无参时弹出 modal 选择器,↑↓ + Enter 切换,Esc 关闭。当前模型行
打 *。复用 build_model_picker_options 纯逻辑给单测覆盖。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: TUI /model add|edit|rm|set-default 子命令

扩 `cmd_model` 的 args 解析,加四个子命令。`add` / `edit` 走最简文本输入(`/model add name=local-lm provider=openai model=llama-3 base_url=http://localhost:1234/v1 api_key=sk-x` 这种 key=val 风格,避免 FTXUI 多步表单的复杂度);`rm` / `set-default` 直接执行。

**Files:**
- Modify: `src/commands/model_command.cpp`
- Modify: `tests/commands/`(新增 `tests/commands/model_command_test.cpp` 如果没有)

- [ ] **Step 1: 写 args 解析单测(失败)**

`tests/commands/model_command_test.cpp`(新建):

```cpp
// 覆盖 src/commands/model_command.cpp 的 args 解析子命令分支。
// 新加的 add / edit / rm / set-default 每个都有独立的写盘副作用,解析失
// 败应当不动 cfg。

#include <gtest/gtest.h>

#include "commands/model_command.hpp"  // 暴露 parse_subcommand 给测试
#include "config/config.hpp"

namespace {

// 暴露给单测的纯解析函数(在 model_command.cpp 里 unnamed namespace 里加
// 这个 + 在 hpp 里 forward declare)。
} // namespace

namespace acecode {
struct ParsedModelSub {
    std::string sub;     // "" / "add" / "edit" / "rm" / "set-default"
    std::string flag;    // "" / "--cwd" / "--default"
    std::string name;    // 无 sub 时是直接 name;有 sub 时是 sub 的目标
    std::map<std::string, std::string> kvs;  // key=val 列表
};
bool parse_model_subcommand(const std::string& raw, ParsedModelSub& out);
}

// 场景:/model add name=foo provider=openai model=llama base_url=http://x api_key=k
TEST(ModelCommandParse, ParsesAddSubcommand) {
    acecode::ParsedModelSub p;
    EXPECT_TRUE(acecode::parse_model_subcommand(
        "add name=foo provider=openai model=llama base_url=http://x api_key=k", p));
    EXPECT_EQ(p.sub, "add");
    EXPECT_EQ(p.kvs["name"], "foo");
    EXPECT_EQ(p.kvs["provider"], "openai");
    EXPECT_EQ(p.kvs["api_key"], "k");
}

// 场景:/model rm foo
TEST(ModelCommandParse, ParsesRmSubcommand) {
    acecode::ParsedModelSub p;
    EXPECT_TRUE(acecode::parse_model_subcommand("rm foo", p));
    EXPECT_EQ(p.sub, "rm");
    EXPECT_EQ(p.name, "foo");
}

// 场景:/model set-default foo
TEST(ModelCommandParse, ParsesSetDefault) {
    acecode::ParsedModelSub p;
    EXPECT_TRUE(acecode::parse_model_subcommand("set-default foo", p));
    EXPECT_EQ(p.sub, "set-default");
    EXPECT_EQ(p.name, "foo");
}

// 场景:/model gpt-4o(无 sub) → sub 空,name = "gpt-4o"。
TEST(ModelCommandParse, ParsesBareNameWithoutSubcommand) {
    acecode::ParsedModelSub p;
    EXPECT_TRUE(acecode::parse_model_subcommand("gpt-4o", p));
    EXPECT_EQ(p.sub, "");
    EXPECT_EQ(p.name, "gpt-4o");
}

// 场景:/model --cwd gpt-4o
TEST(ModelCommandParse, ParsesCwdFlag) {
    acecode::ParsedModelSub p;
    EXPECT_TRUE(acecode::parse_model_subcommand("--cwd gpt-4o", p));
    EXPECT_EQ(p.flag, "--cwd");
    EXPECT_EQ(p.name, "gpt-4o");
}
```

forward declare 放 `src/commands/model_command.hpp`:

```cpp
// 暴露给单测的纯解析函数。raw 是 /model 后面的字符串(已 trim)。
struct ParsedModelSub {
    std::string sub;
    std::string flag;
    std::string name;
    std::map<std::string, std::string> kvs;
};
bool parse_model_subcommand(const std::string& raw, ParsedModelSub& out);
```

(把 model_command.hpp 加 `#include <map>` `#include <string>`。)

- [ ] **Step 2: 跑测试确认失败**

```bash
ctest --test-dir build -R "ModelCommandParse" --output-on-failure
```
Expected: 编译失败(parse_model_subcommand 未实现)。

- [ ] **Step 3: 实现 parse_model_subcommand**

`src/commands/model_command.cpp` 在 unnamed namespace **外**(让 hpp 声明能 link)实现:

```cpp
bool parse_model_subcommand(const std::string& raw, ParsedModelSub& out) {
    out = {};
    auto s = trim(raw);
    if (s.empty()) return true;  // 无参 → picker

    // sub 关键字:add / edit / rm / set-default
    static const std::vector<std::string> SUBS = {"add", "edit", "rm", "set-default"};
    for (const auto& k : SUBS) {
        if (s.rfind(k + " ", 0) == 0 || s == k) {
            out.sub = k;
            std::string rest = s.size() > k.size() ? trim(s.substr(k.size())) : std::string{};
            // edit/rm/set-default 第一个 token 是 name;add 没有 bare name(全 kv)。
            if (k == "add") {
                // rest 是若干 key=val(空白分隔)
                std::istringstream iss(rest);
                std::string tok;
                while (iss >> tok) {
                    auto eq = tok.find('=');
                    if (eq == std::string::npos) return false;
                    out.kvs[tok.substr(0, eq)] = tok.substr(eq + 1);
                }
            } else {
                // 第一个 token 是 name,剩下是 key=val
                std::istringstream iss(rest);
                std::string tok;
                if (!(iss >> tok)) return false;
                out.name = tok;
                while (iss >> tok) {
                    auto eq = tok.find('=');
                    if (eq == std::string::npos) return false;
                    out.kvs[tok.substr(0, eq)] = tok.substr(eq + 1);
                }
            }
            return true;
        }
    }

    // 不是已知 sub → fall back 到旧的 flag/name 解析
    std::string flag, name;
    if (!parse_model_args(raw, flag, name)) return false;
    out.flag = flag;
    out.name = name;
    return true;
}
```

加 include `#include <sstream>`。

- [ ] **Step 4: 跑解析测试确认通过**

```bash
cmake --build build --target acecode_unit_tests
ctest --test-dir build -R "ModelCommandParse" --output-on-failure
```
Expected: 5 个 PASS。

- [ ] **Step 5: 把子命令接到 cmd_model**

在 `cmd_model` 函数里替换原解析:

```cpp
void cmd_model(CommandContext& ctx, const std::string& args) {
    ParsedModelSub p;
    if (!parse_model_subcommand(args, p)) {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back({"system",
            "Usage: /model | /model <name> | /model --cwd <name> | /model --default <name>\n"
            "       /model add name=X provider=openai model=Y base_url=Z api_key=K\n"
            "       /model edit <name> [field=value ...]\n"
            "       /model rm <name>\n"
            "       /model set-default <name>", false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    if (p.sub == "add") return cmd_model_add(ctx, p);
    if (p.sub == "edit") return cmd_model_edit(ctx, p);
    if (p.sub == "rm") return cmd_model_rm(ctx, p);
    if (p.sub == "set-default") return cmd_model_set_default(ctx, p);

    // 无 sub:沿用原 picker / name / --cwd / --default 路径
    if (p.flag.empty() && p.name.empty()) {
        render_model_picker(ctx);
        return;
    }
    // ... 原来 cmd_model 后半段不变(lookup_entry_by_name + apply_model_to_session + 持久化)
}
```

实现 cmd_model_add / edit / rm / set_default(仍在 unnamed namespace):

```cpp
SavedModelDraft draft_from_kvs(const std::map<std::string, std::string>& kvs,
                                const std::string& default_name = "") {
    SavedModelDraft d;
    auto get = [&](const char* k, std::string& out) {
        auto it = kvs.find(k); if (it != kvs.end()) out = it->second;
    };
    get("name", d.name); if (d.name.empty()) d.name = default_name;
    get("provider", d.provider);
    get("model", d.model);
    get("base_url", d.base_url);
    get("api_key", d.api_key);
    return d;
}

void announce_editor_result(CommandContext& ctx, SavedModelEditError rc,
                              const std::string& ok_msg) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    if (rc == SavedModelEditError::OK) {
        ctx.state.conversation.push_back({"system", ok_msg, false});
    } else {
        ctx.state.conversation.push_back({"system",
            std::string("/model failed: ") + to_string(rc), false});
    }
    ctx.state.chat_follow_tail = true;
}

void cmd_model_add(CommandContext& ctx, const ParsedModelSub& p) {
    auto d = draft_from_kvs(p.kvs);
    auto rc = add_saved_model(ctx.config, d);
    if (rc == SavedModelEditError::OK) {
        try { save_config(ctx.config); }
        catch (const std::exception& e) {
            ctx.config.saved_models.pop_back();
            announce_editor_result(ctx, SavedModelEditError::OK,
                                     std::string("write failed: ") + e.what());
            return;
        }
    }
    announce_editor_result(ctx, rc, "Added: " + d.name);
}

void cmd_model_edit(CommandContext& ctx, const ParsedModelSub& p) {
    if (p.name.empty()) {
        announce_editor_result(ctx, SavedModelEditError::INVALID_NAME, "");
        return;
    }
    auto d = draft_from_kvs(p.kvs, p.name);
    // 缺省字段 fallback 到现有条目(纯 patch 语义):
    for (const auto& e : ctx.config.saved_models) {
        if (e.name == p.name) {
            if (d.provider.empty()) d.provider = e.provider;
            if (d.model.empty()) d.model = e.model;
            if (d.base_url.empty()) d.base_url = e.base_url;
            if (d.api_key.empty()) d.api_key = e.api_key;
            break;
        }
    }
    auto snapshot = ctx.config.saved_models;
    auto rc = update_saved_model(ctx.config, p.name, d);
    if (rc == SavedModelEditError::OK) {
        try { save_config(ctx.config); }
        catch (const std::exception& e) {
            ctx.config.saved_models = std::move(snapshot);
            announce_editor_result(ctx, SavedModelEditError::OK,
                                     std::string("write failed: ") + e.what());
            return;
        }
    }
    announce_editor_result(ctx, rc, "Updated: " + d.name);
}

void cmd_model_rm(CommandContext& ctx, const ParsedModelSub& p) {
    if (p.name.empty()) {
        announce_editor_result(ctx, SavedModelEditError::INVALID_NAME, "");
        return;
    }
    auto snapshot = ctx.config.saved_models;
    auto rc = remove_saved_model(ctx.config, p.name);
    if (rc == SavedModelEditError::OK) {
        try { save_config(ctx.config); }
        catch (const std::exception& e) {
            ctx.config.saved_models = std::move(snapshot);
            announce_editor_result(ctx, SavedModelEditError::OK,
                                     std::string("write failed: ") + e.what());
            return;
        }
    }
    announce_editor_result(ctx, rc, "Removed: " + p.name);
}

void cmd_model_set_default(CommandContext& ctx, const ParsedModelSub& p) {
    if (p.name.empty()) {
        announce_editor_result(ctx, SavedModelEditError::INVALID_NAME, "");
        return;
    }
    bool found = (p.name == "(legacy)");
    if (!found) {
        for (const auto& e : ctx.config.saved_models) if (e.name == p.name) { found = true; break; }
    }
    if (!found) {
        announce_editor_result(ctx, SavedModelEditError::NOT_FOUND, "");
        return;
    }
    std::string before = ctx.config.default_model_name;
    ctx.config.default_model_name = p.name;
    try { save_config(ctx.config); }
    catch (const std::exception& e) {
        ctx.config.default_model_name = before;
        announce_editor_result(ctx, SavedModelEditError::OK,
                                 std::string("write failed: ") + e.what());
        return;
    }
    announce_editor_result(ctx, SavedModelEditError::OK,
                             "Default: " + p.name);
}
```

includes:

```cpp
#include "../config/saved_models_editor.hpp"
```

- [ ] **Step 6: 跑全量单测**

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: 全 PASS。

- [ ] **Step 7: 手动验证**

```bash
./build/acecode
# /model add name=test-lm provider=openai model=llama-3 base_url=http://localhost:1234/v1 api_key=sk-x
# 看 system 消息 "Added: test-lm"
# /model
# 看 picker 列表里有 test-lm
# /model rm test-lm
# 看 "Removed: test-lm"
```

- [ ] **Step 8: Commit**

```bash
git add src/commands/model_command.hpp src/commands/model_command.cpp \
        tests/commands/model_command_test.cpp
git commit -m "feat(tui): /model add|edit|rm|set-default subcommands

支持在 TUI 内增删改 saved_models 与设置全局默认。失败时 cfg 内存与磁盘
保持同步(回滚)。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: WebUI lib helpers + Node 单测

`web/src/lib/` 加三个纯逻辑模块,有 Node 单测 — 这层先做完,后面 React 组件组装时直接用。

**Files:**
- Create: `web/src/lib/errors.js`
- Create: `web/src/lib/modelPicker.js`
- Create: `web/src/lib/modelManager.js`
- Create: `web/src/lib/__tests__/errors.test.mjs`
- Create: `web/src/lib/__tests__/modelPicker.test.mjs`
- Create: `web/src/lib/__tests__/modelManager.test.mjs`

- [ ] **Step 1: 看现有 lib 测试约定**

```bash
ls web/src/lib/__tests__/ 2>/dev/null || ls web/src/lib/*.test.* 2>/dev/null
cat web/package.json | grep -A 5 scripts
```

按现有约定决定文件名(`*.test.mjs` / `*.test.js` / `__tests__/`)与运行命令。

- [ ] **Step 2: 写 errors.js**

```javascript
// web/src/lib/errors.js
// 后端 SavedModelEditError 与 HTTP 共用错误码 → 中文文案。
// 未识别码退到原始 message。

const TABLE = {
  INVALID_NAME:      '名字不能为空',
  RESERVED_NAME:     '名字以 ( 开头是系统保留',
  NAME_TAKEN:        '已存在同名条目',
  UNKNOWN_PROVIDER:  '不支持的 provider 类型',
  MISSING_MODEL:     '请填写 model',
  MISSING_BASE_URL:  'OpenAI 类必须填写 base_url',
  INVALID_API_KEY:   'OpenAI 类的 API key 不能为空',
  NOT_FOUND:         '该模型已不存在,请刷新',
  IN_USE_AS_DEFAULT: '该条是当前默认,先去改默认再操作',
  PERSIST_FAILED:    '配置写盘失败',
  BAD_JSON:          '请求格式错误',
  BAD_REQUEST:       '请求参数错误',
};

export function lookupErrorMessage(code, fallback) {
  if (code && Object.prototype.hasOwnProperty.call(TABLE, code)) return TABLE[code];
  return fallback || code || '未知错误';
}

export const ERROR_CODES = Object.keys(TABLE);
```

- [ ] **Step 3: 写 modelPicker.js**

```javascript
// web/src/lib/modelPicker.js
// 当顶栏下拉的当前值不在 list 里(saved_models 改名/删除导致),插一条
// disabled 灰条显式提示用户。

export function isCurrentValueOrphaned(currentName, list) {
  if (!currentName) return false;
  if (!Array.isArray(list)) return false;
  return list.findIndex((m) => m && m.name === currentName) === -1;
}

export function buildOptionsWithOrphan(currentName, list) {
  if (!Array.isArray(list)) return [];
  const out = [...list];
  if (isCurrentValueOrphaned(currentName, list)) {
    out.unshift({
      name: currentName,
      provider: '?',
      model: '?',
      is_legacy: false,
      orphan: true,
      label: `${currentName} (已被改名/删除)`,
    });
  }
  return out;
}
```

- [ ] **Step 4: 写 modelManager.js**

```javascript
// web/src/lib/modelManager.js
// 提交前的快速校验,避免没必要的 4xx 往返。规则与后端 saved_models_editor
// 保持一致;后端是真值源,前端不重复实现复杂分支。

export function validateModelDraft(draft) {
  if (!draft || typeof draft !== 'object') return { ok: false, code: 'BAD_REQUEST' };
  const { name, provider, model, base_url, api_key } = draft;
  if (!name || typeof name !== 'string' || name.length === 0)
    return { ok: false, code: 'INVALID_NAME' };
  if (name.startsWith('(')) return { ok: false, code: 'RESERVED_NAME' };
  if (provider !== 'openai' && provider !== 'copilot')
    return { ok: false, code: 'UNKNOWN_PROVIDER' };
  if (!model) return { ok: false, code: 'MISSING_MODEL' };
  if (provider === 'openai') {
    if (!base_url) return { ok: false, code: 'MISSING_BASE_URL' };
    if (!api_key) return { ok: false, code: 'INVALID_API_KEY' };
  }
  return { ok: true };
}
```

- [ ] **Step 5: 写测试**

```javascript
// web/src/lib/__tests__/errors.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { lookupErrorMessage } from '../errors.js';

test('已知 code 返中文', () => {
  assert.equal(lookupErrorMessage('NAME_TAKEN'), '已存在同名条目');
});

test('未识别 code 退 fallback', () => {
  assert.equal(lookupErrorMessage('UNKNOWN_X', '后端原文'), '后端原文');
});

test('null code + fallback', () => {
  assert.equal(lookupErrorMessage(null, '兜底'), '兜底');
});
```

```javascript
// web/src/lib/__tests__/modelPicker.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { isCurrentValueOrphaned, buildOptionsWithOrphan } from '../modelPicker.js';

test('当前值在列表里 → 不 orphan', () => {
  assert.equal(isCurrentValueOrphaned('a', [{ name: 'a' }, { name: 'b' }]), false);
});

test('当前值不在列表里 → orphan', () => {
  assert.equal(isCurrentValueOrphaned('ghost', [{ name: 'a' }]), true);
});

test('orphan 时插 disabled 灰条', () => {
  const out = buildOptionsWithOrphan('ghost', [{ name: 'a' }]);
  assert.equal(out.length, 2);
  assert.equal(out[0].orphan, true);
  assert.match(out[0].label, /已被改名/);
});
```

```javascript
// web/src/lib/__tests__/modelManager.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { validateModelDraft } from '../modelManager.js';

test('空名字 → INVALID_NAME', () => {
  assert.equal(validateModelDraft({ name: '', provider: 'copilot', model: 'gpt-4o' }).code,
                'INVALID_NAME');
});

test('保留前缀 → RESERVED_NAME', () => {
  assert.equal(validateModelDraft({ name: '(x)', provider: 'copilot', model: 'gpt-4o' }).code,
                'RESERVED_NAME');
});

test('OpenAI 缺 api_key → INVALID_API_KEY', () => {
  assert.equal(validateModelDraft({
    name: 'lm', provider: 'openai', model: 'l', base_url: 'http://x',
  }).code, 'INVALID_API_KEY');
});

test('合法 copilot 草稿 → ok', () => {
  assert.deepEqual(validateModelDraft({
    name: 'cp', provider: 'copilot', model: 'gpt-4o',
  }), { ok: true });
});
```

- [ ] **Step 6: 跑测试确认通过**

```bash
cd web && node --test src/lib/__tests__/
```
Expected: 全 PASS。如果项目用 vitest,改 `pnpm test`。

- [ ] **Step 7: Commit**

```bash
git add web/src/lib/errors.js web/src/lib/modelPicker.js web/src/lib/modelManager.js \
        web/src/lib/__tests__/
git commit -m "feat(web): error code i18n + model picker/manager helpers

errors.js / modelPicker.js / modelManager.js 是 ModelPicker 与 ModelManager
组件的纯逻辑底座,Node 单测覆盖。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 10: WebUI api.js 增加 model 管理方法

往 `apiClient` 上挂 4 个方法:`addModel` / `updateModel` / `removeModel` / `setDefaultModel`。

**Files:**
- Modify: `web/src/lib/api.js`

- [ ] **Step 1: 加方法**

打开 `web/src/lib/api.js`,在 `switchModel` 之后插入:

```javascript
    addModel:         (draft)        => request('POST',   '/api/models', draft, base),
    updateModel:      (name, draft)  => request('PUT',    `/api/models/${encodeURIComponent(name)}`, draft, base),
    removeModel:      (name)         => request('DELETE', `/api/models/${encodeURIComponent(name)}`, undefined, base),
    setDefaultModel:  (name)         => request('POST',   '/api/config/default-model', {name}, base),
```

- [ ] **Step 2: 跑前端 build 验证语法**

```bash
cd web && pnpm build
```
Expected: build 成功,产物落 `web/dist/`。

- [ ] **Step 3: Commit**

```bash
git add web/src/lib/api.js
git commit -m "feat(web): api client methods for saved_models CRUD

addModel / updateModel / removeModel / setDefaultModel 给 ModelManager
组件用。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 11: ModelPicker.jsx — toast i18n + orphan 灰条

让 ModelPicker 在切换失败时通过 errors.js 给出中文文案,在当前值是 orphan 时插一条 disabled 灰条。

**Files:**
- Modify: `web/src/components/ModelPicker.jsx`

- [ ] **Step 1: 改 ModelPicker.jsx**

替换文件内容为:

```jsx
import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { toast } from './Toast.jsx';
import { lookupErrorMessage } from '../lib/errors.js';
import { buildOptionsWithOrphan } from '../lib/modelPicker.js';

export function ModelPicker({ sessionId, currentName = '', apiClient = api }) {
  const [models, setModels] = useState([]);
  const [value, setValue] = useState(currentName);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    let off = false;
    apiClient.listModels()
      .then((list) => { if (!off) setModels(Array.isArray(list) ? list : []); })
      .catch(() => {});
    return () => { off = true; };
  }, [apiClient]);

  useEffect(() => { setValue(currentName); }, [currentName]);

  const onChange = async (e) => {
    if (!sessionId) { e.preventDefault(); return; }
    const next = e.target.value;
    const prev = value;
    setValue(next); setBusy(true);
    try {
      const result = await apiClient.switchModel(sessionId, next);
      const label = result && result.name
        ? `${result.name} (${result.provider}/${result.model})`
        : next;
      toast({ kind: 'ok', text: '已切换至 ' + label });
    } catch (err) {
      setValue(prev);
      const code = err && err.code;
      const msg = lookupErrorMessage(code, err && err.message);
      toast({ kind: 'err', text: '切换失败:' + msg });
    } finally {
      setBusy(false);
    }
  };

  const options = buildOptionsWithOrphan(value, models);

  return (
    <select
      value={value}
      onChange={onChange}
      disabled={busy || !sessionId}
      className="h-7 px-2 pr-6 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-60 disabled:cursor-not-allowed"
    >
      {options.length === 0 && <option value="">—</option>}
      {options.map((m) => (
        <option key={m.name} value={m.name} disabled={m.orphan}>
          {m.label || (m.name + (m.is_legacy ? ' (legacy)' : ''))}
        </option>
      ))}
    </select>
  );
}
```

注意:`request()` 在 api.js 里捕获到 4xx/5xx 时抛 `Error`,需要确认它的 `code` 字段是否被保留(若不是,改 api.js 的 error throw 路径附带后端 `error` 字段)。看现有 request 实现,如果 throw `new Error(json.message)` 但丢了 code,就在 throw 前 attach:`const e = new Error(...); e.code = json.error; throw e;`

- [ ] **Step 2: 验证 ChatView 传 currentName**

```bash
grep -n "ModelPicker" web/src/components/ChatView.jsx
```

如果 ChatView 没传 `currentName`,加:

```jsx
<ModelPicker sessionId={sid} currentName={modelState?.name || ''} />
```

(具体属性名以 ChatView 内 `modelState` 实际字段为准。)

- [ ] **Step 3: 前端 build + 浏览器手测**

```bash
cd web && pnpm build
cd .. && cmake --build build --target acecode
./build/acecode daemon start
# 浏览器访问 127.0.0.1:28080,新建会话,验证下拉显示 + 切换 + 改 saved_models 后的 orphan 灰条
```

- [ ] **Step 4: Commit**

```bash
git add web/src/components/ModelPicker.jsx web/src/lib/api.js
git commit -m "feat(web): model picker i18n toasts + orphan placeholder

切换失败时用 errors.js 文案;当前值不在列表时插 disabled 灰条提示用户。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 12: ModelManager.jsx + Settings.jsx + Sidebar 齿轮入口

设置面板的"模型"分区:左侧列表(default 带星标),右侧编辑表单(新增 / 编辑切换);删除按钮 + 设默认按钮。

**Files:**
- Create: `web/src/components/Settings.jsx`
- Create: `web/src/components/ModelManager.jsx`
- Modify: `web/src/components/Sidebar.jsx`(齿轮按钮)
- Modify: `web/src/App.jsx`(挂 Settings drawer 状态)

- [ ] **Step 1: 看 Sidebar 现有结构**

```bash
grep -n "Sidebar\|onSettings\|⚙" web/src/components/Sidebar.jsx | head -10
```

记录:Sidebar 顶/底栏的按钮区在哪一段 JSX。

- [ ] **Step 2: 写 Settings.jsx 容器**

```jsx
// web/src/components/Settings.jsx
// 设置抽屉容器。本期只挂 ModelManager;后续可扩 MCP / Skills。
import { ModelManager } from './ModelManager.jsx';

export function Settings({ open, onClose }) {
  if (!open) return null;
  return (
    <div className="fixed inset-0 z-200 bg-black/40 flex justify-end" onClick={onClose}>
      <div
        className="w-[640px] h-full bg-surface text-fg shadow-xl overflow-auto"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex items-center justify-between px-4 py-3 border-b border-border">
          <div className="text-sm font-semibold">设置</div>
          <button className="px-2 py-1 hover:bg-surface-alt rounded" onClick={onClose}>关闭</button>
        </div>
        <div className="p-4 space-y-6">
          <section>
            <h2 className="text-sm font-semibold mb-2">模型</h2>
            <ModelManager />
          </section>
        </div>
      </div>
    </div>
  );
}
```

- [ ] **Step 3: 写 ModelManager.jsx**

```jsx
// web/src/components/ModelManager.jsx
// 左侧 saved_models 列表(默认带星);右侧编辑表单。增 / 改 / 删 / 设默认。
import { useEffect, useMemo, useState } from 'react';
import { api } from '../lib/api.js';
import { toast } from './Toast.jsx';
import { lookupErrorMessage } from '../lib/errors.js';
import { validateModelDraft } from '../lib/modelManager.js';

const EMPTY_DRAFT = {
  name: '', provider: 'openai', model: '',
  base_url: '', api_key: '',
};

export function ModelManager({ apiClient = api }) {
  const [models, setModels] = useState([]);
  const [defaultName, setDefaultName] = useState('');
  const [editingName, setEditingName] = useState(null); // null = 新增
  const [draft, setDraft] = useState(EMPTY_DRAFT);
  const [busy, setBusy] = useState(false);

  const refresh = async () => {
    const list = await apiClient.listModels();
    setModels(Array.isArray(list) ? list : []);
    // default 不在 list 接口返回里,得另外拉。这里假定 GET /api/config 暴露
    // 它,若不存在,先靠 list 中 is_legacy 之外的"is_default"暗示 — 见下文。
    try {
      const cfg = await apiClient.getHealth?.();
      setDefaultName((cfg && cfg.default_model_name) || '');
    } catch {}
  };

  useEffect(() => { refresh(); }, []);

  const onSelect = (m) => {
    if (m.is_legacy) {
      // (legacy) 是合成行,不可编辑/删除。
      setEditingName(null);
      setDraft(EMPTY_DRAFT);
      return;
    }
    setEditingName(m.name);
    setDraft({
      name: m.name, provider: m.provider, model: m.model,
      base_url: m.base_url || '', api_key: '',  // api_key 不回显
    });
  };

  const submit = async () => {
    const v = validateModelDraft(draft);
    if (!v.ok) { toast({ kind: 'err', text: lookupErrorMessage(v.code) }); return; }
    setBusy(true);
    try {
      if (editingName) await apiClient.updateModel(editingName, draft);
      else             await apiClient.addModel(draft);
      toast({ kind: 'ok', text: editingName ? '已更新' : '已新增' });
      setEditingName(null); setDraft(EMPTY_DRAFT);
      await refresh();
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err && err.code, err && err.message) });
    } finally { setBusy(false); }
  };

  const removeOne = async (name) => {
    setBusy(true);
    try {
      await apiClient.removeModel(name);
      toast({ kind: 'ok', text: '已删除 ' + name });
      await refresh();
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err && err.code, err && err.message) });
    } finally { setBusy(false); }
  };

  const setDefault = async (name) => {
    setBusy(true);
    try {
      await apiClient.setDefaultModel(name);
      setDefaultName(name);
      toast({ kind: 'ok', text: '默认: ' + name });
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err && err.code, err && err.message) });
    } finally { setBusy(false); }
  };

  return (
    <div className="grid grid-cols-[260px_1fr] gap-4">
      <div className="border border-border rounded-md overflow-hidden">
        {models.map((m) => (
          <div
            key={m.name}
            className={
              'flex items-center justify-between px-2 py-1.5 cursor-pointer hover:bg-surface-alt ' +
              (editingName === m.name ? 'bg-surface-alt' : '')
            }
            onClick={() => onSelect(m)}
          >
            <span className="truncate text-[12px]">
              {defaultName === m.name ? '★ ' : '  '}
              {m.name}{m.is_legacy ? ' (legacy)' : ''}
            </span>
            {!m.is_legacy && (
              <span className="flex gap-1">
                {defaultName !== m.name && (
                  <button
                    className="px-1.5 py-0.5 text-[11px] hover:underline"
                    onClick={(e) => { e.stopPropagation(); setDefault(m.name); }}
                    disabled={busy}
                  >
                    设为默认
                  </button>
                )}
                <button
                  className="px-1.5 py-0.5 text-[11px] text-red-500 hover:underline"
                  onClick={(e) => { e.stopPropagation(); removeOne(m.name); }}
                  disabled={busy}
                >
                  删
                </button>
              </span>
            )}
          </div>
        ))}
        <div
          className="px-2 py-2 text-[12px] hover:bg-surface-alt cursor-pointer border-t border-border"
          onClick={() => { setEditingName(null); setDraft(EMPTY_DRAFT); }}
        >
          + 新增模型
        </div>
      </div>

      <div className="space-y-2">
        <div className="text-[12px] font-semibold">{editingName ? '编辑 ' + editingName : '新增模型'}</div>
        <Field label="名字"><input value={draft.name} onChange={(e) => setDraft({...draft, name: e.target.value})} disabled={!!editingName} /></Field>
        <Field label="provider">
          <select value={draft.provider} onChange={(e) => setDraft({...draft, provider: e.target.value})}>
            <option value="openai">openai</option>
            <option value="copilot">copilot</option>
          </select>
        </Field>
        <Field label="model"><input value={draft.model} onChange={(e) => setDraft({...draft, model: e.target.value})} /></Field>
        {draft.provider === 'openai' && (
          <>
            <Field label="base_url"><input value={draft.base_url} onChange={(e) => setDraft({...draft, base_url: e.target.value})} /></Field>
            <Field label="api_key"><input type="password" value={draft.api_key}
                                            placeholder={editingName ? '留空保留旧值' : ''}
                                            onChange={(e) => setDraft({...draft, api_key: e.target.value})} /></Field>
          </>
        )}
        <button className="px-3 py-1 bg-accent text-white rounded" onClick={submit} disabled={busy}>
          {editingName ? '保存' : '新增'}
        </button>
      </div>
    </div>
  );
}

function Field({ label, children }) {
  return (
    <label className="flex items-center gap-2 text-[12px]">
      <span className="w-20 text-right">{label}</span>
      <span className="flex-1 [&>input]:w-full [&>input]:px-2 [&>input]:py-1 [&>input]:border [&>input]:border-border [&>input]:rounded [&>select]:px-2 [&>select]:py-1 [&>select]:border [&>select]:border-border [&>select]:rounded">{children}</span>
    </label>
  );
}
```

注意 `apiClient.getHealth` — 看 api.js 是否存在;若不存在改写新端点拉 default 或在 listModels 里附带 default 名字。**简化**:改后端 `GET /api/models` 在响应里加一个 `defaults: {name: "..."}` 元字段,前端读它。这是 Task 4 的小补丁:

```cpp
// 在 list_models 返回值改成对象 {entries: [...], default_name: cfg.default_model_name}
// 或者另开 GET /api/config endpoint
```

为不阻塞,简单做法:加一个 `GET /api/config/default-model` 端点(只读 cfg.default_model_name → JSON),前端拉它。本任务 step 0 加这个端点(改 server.cpp + 一个测试)。

实际让 step 0 先做这个补丁:

```cpp
// 在 server.cpp register_models() 里加:
CROW_ROUTE(app, "/api/config/default-model").methods(crow::HTTPMethod::Get)
([this](const crow::request& req) {
    if (auto rej = require_auth(req)) return std::move(*rej);
    crow::response r(200);
    r.set_header("Content-Type", "application/json");
    r.body = json{{"name", deps.config_mut.default_model_name}}.dump();
    return cors_apply(r, req);
});
```

api.js 加:`getDefaultModel: () => request('GET', '/api/config/default-model', undefined, base),`

ModelManager.jsx 用 `apiClient.getDefaultModel()` 替代 `getHealth?.()` 调用。

- [ ] **Step 4: Sidebar 加齿轮按钮**

打开 `web/src/components/Sidebar.jsx`,在合适位置(顶栏 / 底栏)加:

```jsx
// 顶部 import
import { useState } from 'react';
import { Settings } from './Settings.jsx';

// 在组件 state 里:
const [settingsOpen, setSettingsOpen] = useState(false);

// 在 JSX 里(底部按钮区):
<button
  className="px-2 py-1.5 hover:bg-surface-alt rounded text-[12px]"
  onClick={() => setSettingsOpen(true)}
  title="设置"
>
  ⚙
</button>
<Settings open={settingsOpen} onClose={() => setSettingsOpen(false)} />
```

(若 Sidebar 已经有 `onSettings` prop 走 App.jsx 集中管,改成调用 prop。)

- [ ] **Step 5: 前端 build + 浏览器手测**

```bash
cd web && pnpm build
cd .. && cmake --build build --target acecode
./build/acecode daemon start
# 浏览器:
# 1. 点齿轮 → 抽屉打开,看到 saved_models 列表,默认条目带 ★
# 2. 选一条 → 右侧表单填充
# 3. 点"+ 新增模型" → 表单清空,填 name=test-lm provider=openai 等 → 新增 → 列表多一条
# 4. 点"删" → 列表少一条
# 5. 点删默认条目 → toast"该条是当前默认,先去改默认再操作"
# 6. 点别的"设为默认" → ★ 跑过去,默认不能再删原来那条变成可删
```

- [ ] **Step 6: Commit**

```bash
git add web/src/components/Settings.jsx web/src/components/ModelManager.jsx \
        web/src/components/Sidebar.jsx web/src/lib/api.js \
        src/web/server.cpp
git commit -m "feat(web): saved_models management drawer

Sidebar 齿轮 → 设置抽屉,左侧列表(默认带 ★),右侧表单。增/删/改/设默
认全在前端。

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 13: 手动端到端验证 checklist

按 spec § 7.5 走一遍。每条勾选,失败开 issue 修。

- [ ] **场景 1: WebUI 切模型 + meta 文件落盘**

```bash
./build/acecode daemon start
# 浏览器进会话 → 切到不同模型
ls ~/.acecode/projects/<hash>/<sid>-*.json
# cat 查看 meta,确认 provider/model/model_preset 已更新
```

- [ ] **场景 2: TUI + WebUI 联动**

```bash
# 终端 A:./build/acecode (TUI)
# 浏览器:同一目录的 daemon UI(如果 daemon 已启)
# TUI: /model copilot-fast
# 浏览器:GET /api/sessions/<id>/model 应返 copilot-fast
# 浏览器:换一个 → TUI 状态栏应在下次 dispatch 时反映新模型
```

注意:TUI 与 daemon 共享同一份 cfg.json,但 TUI session 与 daemon session **不是**同一个 SessionEntry(TUI 是进程内单 session,daemon 是 SessionRegistry 内的另一个)。这场景验证的是 **cfg 共享 + saved_models 共享**,不是 SessionEntry 共享。

- [ ] **场景 3: 流式中切**

发一个长 prompt(让模型回 30 秒+),流到一半在浏览器切到不同模型 → 当前轮继续用旧模型回完;再发一发 → 用新模型。

- [ ] **场景 4: 删默认拒绝 + 改默认后删成功**

设置面板找当前默认,按"删"→ toast "该条是当前默认,先去改默认再操作"。点别的"设为默认"→ 默认换;原来那条点删 → 200。

- [ ] **场景 5: saved_models 改名后已活会话**

会话 X 用 prod-fast 跑,设置面板把 prod-fast 改名(或删除)→ 会话 X 顶栏下拉显示 disabled 灰条 "prod-fast (已被改名/删除)" → 选另一条 → 灰条消失,meta 写新名。

- [ ] **场景 6: Copilot silent_auth 失败**

```bash
mv ~/.acecode/copilot_token.json ~/.acecode/copilot_token.json.bak
./build/acecode daemon start
# 浏览器切到 copilot 模型 → toast "已切换,警告:Copilot silent_auth failed"
# 发消息 → Provider 报"session token unavailable"
mv ~/.acecode/copilot_token.json.bak ~/.acecode/copilot_token.json
# /login(TUI 或浏览器登录路径)→ 再发 → 正常
```

- [ ] **场景 7: 写 spec 没列出来但应当稳定的**

- 启动 daemon → 浏览器 → GET /api/models 返 list + (legacy) 行 → list 顺序与 cfg.saved_models 一致。
- POST /api/models 加一条 → 立即 GET /api/models 看到新条目。
- 关 daemon → 看 ~/.acecode/config.json saved_models 里有刚加的条目(写盘验证)。

- [ ] **Step 8: 把 checklist 结果记下,如有 fail 开 issue**

```bash
# 假设全部 PASS:
git commit --allow-empty -m "chore: e2e checklist passed for model selection"
```

---

## 自检结果

**Spec 覆盖检查:**

| Spec 段 | Plan 任务 |
|---|---|
| § 4.1 apply_model_to_session helper | Task 1 |
| § 4.2 TUI provider_slot 升级 | Task 5 |
| § 4.3 saved_models 编辑模块 | Task 3 |
| § 4.4 HTTP 端点 | Task 4(+ Task 12 Step 3 补 GET default-model) |
| § 4.5 UI 入口 — WebUI ModelPicker | Task 11 |
| § 4.5 UI 入口 — WebUI ModelManager + Settings | Task 12 |
| § 4.5 UI 入口 — TUI picker | Task 7 |
| § 4.5 UI 入口 — TUI add/edit/rm/set-default | Task 8 |
| § 4.5 UI 入口 — WebUI lib helpers | Task 9 |
| § 4.5 UI 入口 — api.js 方法 | Task 10 |
| § 5 数据流(切换 / 增删改 / 新建 / 恢复) | Task 1+2+4+5+6 实现路径;Task 13 端到端 |
| § 6 错误处理 | Task 1/3/4 内置回滚 + Task 11/12 toast i18n |
| § 7 测试 | Task 1/3/4/8/9 各自的单测步骤 + Task 13 手动 |
| § 8 YAGNI 边界 | 不出现在任务里(plan 不做)|

无遗漏。

**Placeholder 扫描:** 无 TBD/TODO/"add appropriate"等。

**类型一致性:** `ProviderSlot` / `ApplyModelDeps` / `SavedModelDraft` / `SavedModelEditError` 在 Task 1/3/4/5/6/8 的引用一致;`apiClient` 方法名 `addModel/updateModel/removeModel/setDefaultModel` 在 Task 10/11/12 一致;前端 `lookupErrorMessage` 与 `validateModelDraft` 在 Task 9/11/12 一致。

---

## 执行选项

**Plan complete and saved to `docs/superpowers/plans/2026-05-09-model-selection.md`. Two execution options:**

1. **Subagent-Driven(推荐)** — 每任务派一个新 subagent,我在两阶段 review 中切换,迭代快
2. **Inline Execution** — 在当前会话顺序执行,带 checkpoint review

**Which approach?**
