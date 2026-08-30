// tests/provider/apply_model_to_session_test.cpp
//
// 覆盖 src/provider/apply_model_to_session.cpp。两条调用方(daemon 的
// SessionRegistry::switch_model 与 TUI 的 /model 命令)都靠这一份逻辑,
// 任一分支退化都会让 per-session 切换语义破裂。
//
// 触发场景 / 期望:
//   - cfg 缺失 → 抛 runtime_error,binding provider 不动(错误前置防止半改)
//   - binding 缺失 → 抛 runtime_error
//   - 正常切换:binding provider 替换 + state.context_window > 0 + sm 调用
//   - sm/loop 任一为 null:不崩(覆盖 TUI 启动早期场景)

#include <gtest/gtest.h>

#include "provider/apply_model_to_session.hpp"

#include "config/config.hpp"
#include "config/saved_models.hpp"
#include "provider/session_model_binding.hpp"

using acecode::AppConfig;
using acecode::ApplyModelDeps;
using acecode::apply_model_to_session;
using acecode::ModelProfile;
using acecode::SessionModelBinding;

namespace {

// 构造一个 copilot cfg。create_provider_from_entry 对 copilot 不需要
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

// 场景:cfg 为 nullptr → 立刻抛,binding 不被触碰。
// 这是错误前置原则 — caller 给的 deps 残缺时不能让 binding 进入半成功状态。
TEST(ApplyModelToSession, ThrowsWhenCfgMissing) {
    SessionModelBinding binding;
    auto profile = make_copilot_profile();
    ApplyModelDeps deps;
    deps.model_binding = &binding;
    deps.cfg = nullptr;
    EXPECT_THROW(apply_model_to_session(profile, deps), std::runtime_error);
    EXPECT_FALSE(binding.provider_snapshot());  // 仍未设置
}

// 场景:binding 为 nullptr → 抛。caller 必须先把 binding 准备好。
TEST(ApplyModelToSession, ThrowsWhenBindingMissing) {
    auto cfg = make_copilot_cfg();
    auto profile = make_copilot_profile();
    ApplyModelDeps deps;
    deps.cfg = &cfg;
    deps.model_binding = nullptr;
    EXPECT_THROW(apply_model_to_session(profile, deps), std::runtime_error);
}

// 场景:正常切换 copilot 模型 → binding provider 被设;state 字段填好;无 warning。
// 触发:WebUI / TUI 用户切到一个 copilot 预设。
TEST(ApplyModelToSession, SwapsProviderAndPopulatesState) {
    auto cfg = make_copilot_cfg();
    SessionModelBinding binding;
    auto profile = make_copilot_profile();
    ApplyModelDeps deps;
    deps.cfg = &cfg;
    deps.model_binding = &binding;
    deps.sm = nullptr;
    deps.loop = nullptr;

    auto result = apply_model_to_session(profile, deps);

    EXPECT_EQ(result.state.name, "copilot-mini");
    EXPECT_EQ(result.state.provider, "copilot");
    EXPECT_EQ(result.state.model, "gpt-4o-mini");
    EXPECT_GT(result.state.context_window, 0);
    const auto provider = binding.provider_snapshot();
    ASSERT_TRUE(provider);
    EXPECT_EQ(provider->name(), "copilot");
    EXPECT_EQ(provider->model(), "gpt-4o-mini");

    // TUI adapter 初始化后第一次等 revision submit 必须命中原子 fast path;
    // resolver 不应运行,否则每条输入都会重复 profile lookup/构造。
    int resolver_calls = 0;
    const auto current_revision = binding.applied_revision();
    const auto reload = binding.ensure_current(
        false,
        [current_revision] { return current_revision; },
        [&](const std::string&) {
            ++resolver_calls;
            return acecode::SessionModelResolvedTarget{};
        });
    EXPECT_TRUE(reload.ok);
    EXPECT_EQ(reload.outcome,
              acecode::SessionModelReloadOutcome::AlreadyCurrent);
    EXPECT_EQ(resolver_calls, 0);
    EXPECT_EQ(binding.provider_snapshot(), provider);
}

// 场景:profile 带手动 context_window → session state 使用该值。
TEST(ApplyModelToSession, UsesProfileContextWindowOverride) {
    auto cfg = make_copilot_cfg();
    SessionModelBinding binding;
    auto profile = make_copilot_profile();
    profile.context_window = 64000;
    ApplyModelDeps deps;
    deps.cfg = &cfg;
    deps.model_binding = &binding;

    auto result = apply_model_to_session(profile, deps);

    EXPECT_EQ(result.state.context_window, 64000);
}

// Grok Coding Plan is a managed provider like Copilot, but it authenticates
// lazily when the next request is sent. Switching/restoring a session must not
// require network access or a token to be present yet.
TEST(ApplyModelToSession, SwapsToManagedGrokWithoutConnectionOverrides) {
    auto cfg = make_copilot_cfg();
    SessionModelBinding binding;
    ModelProfile profile;
    profile.name = "grok-coding";
    profile.provider = "grok";
    profile.model = "grok-4.5";
    profile.context_window = 131072;

    ApplyModelDeps deps;
    deps.cfg = &cfg;
    deps.model_binding = &binding;

    const auto result = apply_model_to_session(profile, deps);

    EXPECT_EQ(result.state.name, "grok-coding");
    EXPECT_EQ(result.state.provider, "grok");
    EXPECT_EQ(result.state.model, "grok-4.5");
    EXPECT_EQ(result.state.context_window, 131072);
    EXPECT_TRUE(result.warning.empty());
    const auto provider = binding.provider_snapshot();
    ASSERT_TRUE(provider);
    EXPECT_EQ(provider->name(), "grok");
    EXPECT_EQ(provider->model(), "grok-4.5");
}

// 场景:codex provider 已屏蔽,不能通过 /model 或 Web API 切过去。
TEST(ApplyModelToSession, RejectsDisabledCodexProvider) {
    auto cfg = make_copilot_cfg();
    SessionModelBinding binding;
    ModelProfile profile;
    profile.name = "codex";
    profile.provider = "codex";
    profile.model = "gpt-5.5";
    ApplyModelDeps deps;
    deps.cfg = &cfg;
    deps.model_binding = &binding;

    EXPECT_THROW(apply_model_to_session(profile, deps), std::runtime_error);
    EXPECT_FALSE(binding.provider_snapshot());
}
