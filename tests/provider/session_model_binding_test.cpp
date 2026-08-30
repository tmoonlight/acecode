#include <gtest/gtest.h>

#include "provider/session_model_binding.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

acecode::ModelProfile profile_named(
    std::string name = "fast",
    std::string model = "model-a") {
    acecode::ModelProfile profile;
    profile.name = std::move(name);
    profile.provider = "openai";
    profile.base_url = "https://gateway.example/v1";
    profile.api_key = "key-a";
    profile.model = std::move(model);
    return profile;
}

acecode::SessionModelResolvedTarget target_for(
    acecode::ModelProfile profile,
    acecode::AppConfig config,
    acecode::SavedModelsRevision revision) {
    auto snapshot = std::make_shared<acecode::AppConfig>(std::move(config));
    acecode::SessionModelResolvedTarget target;
    target.revision = revision;
    target.state = acecode::session_model_state_from_profile(*snapshot, profile);
    target.profile = std::move(profile);
    target.config = std::move(snapshot);
    return target;
}

acecode::SessionModelReloadResult install_initial(
    acecode::SessionModelBinding& binding,
    const acecode::SessionModelResolvedTarget& target) {
    return binding.install_explicit(
        target,
        [target](const std::string&) { return target; });
}

} // namespace

// 触发场景:新会话/恢复会话初始化 binding 后立即提交第一条消息;期望当前
// revision 的原子 guard 直接返回,不做 profile lookup 或 Provider 重建。
TEST(SessionModelBinding, EqualRevisionUsesNoWorkFastPathAfterInitialization) {
    acecode::AppConfig cfg;
    auto profile = profile_named();
    cfg.saved_models = {profile};
    auto target = target_for(profile, cfg, 7);
    acecode::SessionModelBinding binding;
    ASSERT_TRUE(install_initial(binding, target).ok);
    const auto provider = binding.provider_snapshot();
    ASSERT_TRUE(provider);

    int resolver_calls = 0;
    const auto result = binding.ensure_current(
        false,
        [] { return acecode::SavedModelsRevision{7}; },
        [&](const std::string&) {
            ++resolver_calls;
            return target;
        });

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.outcome,
              acecode::SessionModelReloadOutcome::AlreadyCurrent);
    EXPECT_EQ(resolver_calls, 0);
    EXPECT_EQ(binding.provider_snapshot(), provider);
}

// 触发场景:同名 profile 的有效 API key/model 被编辑;期望下一次 future turn
// 重建一次,后续相同 revision 再提交不重复重建。旧会话曾永久持有旧快照。
TEST(SessionModelBinding, EffectiveEditReloadsOnceThenReturnsToFastPath) {
    acecode::AppConfig cfg;
    auto original = profile_named();
    cfg.saved_models = {original};
    acecode::SessionModelBinding binding;
    ASSERT_TRUE(install_initial(binding, target_for(original, cfg, 1)).ok);
    const auto before = binding.provider_snapshot();

    auto changed = original;
    changed.api_key = "key-b";
    changed.model = "model-b";
    cfg.saved_models = {changed};
    const auto changed_target = target_for(changed, cfg, 2);
    int resolver_calls = 0;
    auto resolver = [&](const std::string&) {
        ++resolver_calls;
        return changed_target;
    };
    const auto first = binding.ensure_current(
        false, [] { return acecode::SavedModelsRevision{2}; }, resolver);
    ASSERT_TRUE(first.ok);
    EXPECT_EQ(first.outcome, acecode::SessionModelReloadOutcome::Reloaded);
    EXPECT_NE(binding.provider_snapshot(), before);
    EXPECT_EQ(binding.provider_snapshot()->model(), "model-b");
    const int after_first_calls = resolver_calls;

    const auto second = binding.ensure_current(
        false, [] { return acecode::SavedModelsRevision{2}; }, resolver);
    EXPECT_EQ(second.outcome,
              acecode::SessionModelReloadOutcome::AlreadyCurrent);
    EXPECT_EQ(resolver_calls, after_first_calls);
}

// 触发场景:只改 context_window 或另一个非视觉 profile;期望 session state
// 前移 revision/上下文,但 Provider 指针不变。旧粗粒度刷新会无意义断开连接。
TEST(SessionModelBinding, ContextOnlyAndUnrelatedEditsDoNotRebuild) {
    acecode::AppConfig cfg;
    auto selected = profile_named();
    cfg.saved_models = {selected};
    acecode::SessionModelBinding binding;
    ASSERT_TRUE(install_initial(binding, target_for(selected, cfg, 10)).ok);
    const auto provider = binding.provider_snapshot();

    selected.context_window = 64000;
    auto unrelated = profile_named("other", "other-model");
    cfg.saved_models = {selected, unrelated};
    const auto next = target_for(selected, cfg, 11);
    const auto result = binding.ensure_current(
        false,
        [] { return acecode::SavedModelsRevision{11}; },
        [next](const std::string&) { return next; });

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.outcome,
              acecode::SessionModelReloadOutcome::AlreadyCurrent);
    EXPECT_EQ(binding.provider_snapshot(), provider);
    EXPECT_EQ(binding.state_snapshot().context_window, 64000);
    EXPECT_EQ(binding.applied_revision(), 11u);
}

// 触发场景:另一个 profile 新增 vision 能力,改变当前非视觉 Provider 的全局
// fallback 路由输入;期望当前 session 重建。旧比较遗漏该派生全局输入。
TEST(SessionModelBinding, GlobalVisionAvailabilityRebuildsAffectedProvider) {
    acecode::AppConfig cfg;
    auto selected = profile_named();
    cfg.saved_models = {selected};
    acecode::SessionModelBinding binding;
    ASSERT_TRUE(install_initial(binding, target_for(selected, cfg, 20)).ok);
    const auto before = binding.provider_snapshot();

    auto vision = profile_named("vision", "vision-model");
    vision.capabilities = {"vision"};
    cfg.saved_models.push_back(vision);
    const auto next = target_for(selected, cfg, 21);
    const auto result = binding.ensure_current(
        false,
        [] { return acecode::SavedModelsRevision{21}; },
        [next](const std::string&) { return next; });

    EXPECT_EQ(result.outcome, acecode::SessionModelReloadOutcome::Reloaded);
    EXPECT_NE(binding.provider_snapshot(), before);
}

// 触发场景:选中 profile 被删除/改名或本来就是 ad-hoc;期望自动 reload
// 保留旧 Provider 并记录 revision,forced reload 仍再次解析而不是永久短路。
TEST(SessionModelBinding, UnresolvableRetainsProviderAndForcedCallsRecheck) {
    struct Scenario {
        std::string selected_name;
        std::optional<acecode::ModelProfile> remaining_profile;
    };
    const std::vector<Scenario> scenarios = {
        {"deleted-profile", std::nullopt},
        {"renamed-away-profile", profile_named("renamed-profile")},
        {"(session:legacy/model)", std::nullopt},
    };

    for (const auto& scenario : scenarios) {
        SCOPED_TRACE(scenario.selected_name);
        acecode::AppConfig initial_cfg;
        auto profile = profile_named(scenario.selected_name);
        if (scenario.selected_name.rfind("(session:", 0) != 0) {
            initial_cfg.saved_models = {profile};
        }
        acecode::SessionModelBinding binding;
        ASSERT_TRUE(install_initial(
            binding, target_for(profile, initial_cfg, 30)).ok);
        const auto provider = binding.provider_snapshot();

        int resolver_calls = 0;
        auto unresolved = [&](const std::string& selected_name) {
            EXPECT_EQ(selected_name, scenario.selected_name);
            ++resolver_calls;
            acecode::SessionModelResolvedTarget target;
            target.revision = 31;
            auto current = std::make_shared<acecode::AppConfig>();
            if (scenario.remaining_profile.has_value()) {
                current->saved_models = {*scenario.remaining_profile};
            }
            target.config = std::move(current);
            return target;
        };
        const auto automatic = binding.ensure_current(
            false,
            [] { return acecode::SavedModelsRevision{31}; },
            unresolved);
        EXPECT_EQ(automatic.outcome,
                  acecode::SessionModelReloadOutcome::Unresolvable);
        EXPECT_EQ(binding.provider_snapshot(), provider);
        EXPECT_EQ(binding.applied_revision(), 31u);
        const int after_automatic = resolver_calls;

        EXPECT_EQ(binding.ensure_current(
                      false,
                      [] { return acecode::SavedModelsRevision{31}; },
                      unresolved).outcome,
                  acecode::SessionModelReloadOutcome::AlreadyCurrent);
        EXPECT_EQ(resolver_calls, after_automatic);
        EXPECT_EQ(binding.ensure_current(
                      true,
                      [] { return acecode::SavedModelsRevision{31}; },
                      unresolved).outcome,
                  acecode::SessionModelReloadOutcome::Unresolvable);
        EXPECT_GT(resolver_calls, after_automatic);
        EXPECT_EQ(binding.provider_snapshot(), provider);
    }
}

// 触发场景:自动 reload 的新 profile 在构造前已变为无效 Provider;期望返回
// 独立失败,旧 Provider/state/revision 全部不动,下次仍可重试。
TEST(SessionModelBinding, ConstructionFailurePreservesSnapshotAndStaleRevision) {
    acecode::AppConfig cfg;
    auto profile = profile_named();
    cfg.saved_models = {profile};
    acecode::SessionModelBinding binding;
    ASSERT_TRUE(install_initial(binding, target_for(profile, cfg, 40)).ok);
    const auto provider = binding.provider_snapshot();
    const auto state = binding.state_snapshot();

    auto invalid = profile;
    invalid.provider = "codex";
    const auto invalid_target = target_for(invalid, cfg, 41);
    const auto failed = binding.ensure_current(
        false,
        [] { return acecode::SavedModelsRevision{41}; },
        [invalid_target](const std::string&) { return invalid_target; });

    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(binding.provider_snapshot(), provider);
    EXPECT_EQ(binding.state_snapshot().name, state.name);
    EXPECT_EQ(binding.state_snapshot().model, state.model);
    EXPECT_EQ(binding.applied_revision(), 40u);
}

// 触发场景:Provider 已发布后 metadata callback 失败;期望不回滚连接,
// 返回固定安全 warning,且不包含 callback 内部可能带凭据的异常内容。
TEST(SessionModelBinding, MetadataFailureWarnsWithoutRollingBackProvider) {
    acecode::AppConfig cfg;
    auto original = profile_named();
    cfg.saved_models = {original};
    acecode::SessionModelBinding binding;
    ASSERT_TRUE(install_initial(binding, target_for(original, cfg, 50)).ok);
    const auto before = binding.provider_snapshot();

    auto changed = original;
    changed.model = "model-b";
    cfg.saved_models = {changed};
    const auto next = target_for(changed, cfg, 51);
    const auto result = binding.ensure_current(
        false,
        [] { return acecode::SavedModelsRevision{51}; },
        [next](const std::string&) { return next; },
        [](const acecode::SessionModelState&,
           const acecode::SessionModelTransition&) -> bool {
            throw std::runtime_error("Bearer secret-must-not-escape");
        });

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.outcome, acecode::SessionModelReloadOutcome::Reloaded);
    EXPECT_EQ(result.warning, "session metadata could not be persisted");
    EXPECT_EQ(result.warning.find("secret-must-not-escape"), std::string::npos);
    EXPECT_NE(binding.provider_snapshot(), before);
}

// 触发场景:revision 5 的慢构建在发布前看到 revision 6;期望丢弃旧结果并
// 重试最新 profile,applied revision 只前进不回退。旧 slot swap 会发布陈旧连接。
TEST(SessionModelBinding, RevalidationDiscardsStaleBuildBeforePublication) {
    acecode::AppConfig cfg;
    auto initial = profile_named("fast", "model-a");
    cfg.saved_models = {initial};
    acecode::SessionModelBinding binding;
    ASSERT_TRUE(install_initial(binding, target_for(initial, cfg, 4)).ok);

    auto revision5 = initial;
    revision5.model = "model-v5";
    auto cfg5 = cfg;
    cfg5.saved_models = {revision5};
    auto current = target_for(revision5, cfg5, 5);
    std::mutex mu;
    std::condition_variable cv;
    int calls = 0;
    bool revalidating = false;
    bool release = false;
    auto resolver = [&](const std::string&) {
        std::unique_lock<std::mutex> lock(mu);
        ++calls;
        if (calls == 2) {
            revalidating = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
        }
        return current;
    };

    acecode::SessionModelReloadResult result;
    std::thread worker([&] {
        result = binding.ensure_current(
            false,
            [&] {
                std::lock_guard<std::mutex> lock(mu);
                return current.revision;
            },
            resolver);
    });
    bool reached_revalidation = false;
    {
        std::unique_lock<std::mutex> lock(mu);
        reached_revalidation = cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return revalidating;
        });
        if (reached_revalidation) {
            auto revision6 = initial;
            revision6.model = "model-v6";
            auto cfg6 = cfg;
            cfg6.saved_models = {revision6};
            current = target_for(revision6, cfg6, 6);
        }
        release = true;
    }
    cv.notify_all();
    worker.join();

    ASSERT_TRUE(reached_revalidation);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.outcome, acecode::SessionModelReloadOutcome::Reloaded);
    EXPECT_EQ(binding.provider_snapshot()->model(), "model-v6");
    EXPECT_EQ(binding.applied_revision(), 6u);
}

// 触发场景:两个 future submits 同时观察到同一新 revision;期望等待者在
// serialization 后重查,只有一个返回 reloaded,两者最终共享同一 Provider。
TEST(SessionModelBinding, ConcurrentLazyReloadIsSingleFlight) {
    acecode::AppConfig cfg;
    auto initial = profile_named("fast", "model-a");
    cfg.saved_models = {initial};
    acecode::SessionModelBinding binding;
    ASSERT_TRUE(install_initial(binding, target_for(initial, cfg, 60)).ok);

    auto changed = initial;
    changed.model = "model-b";
    cfg.saved_models = {changed};
    const auto next = target_for(changed, cfg, 61);
    std::atomic<int> resolver_calls{0};
    std::mutex mu;
    std::condition_variable cv;
    bool first_inside = false;
    bool release = false;
    auto resolver = [&](const std::string&) {
        const int call = ++resolver_calls;
        if (call == 1) {
            std::unique_lock<std::mutex> lock(mu);
            first_inside = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
        }
        return next;
    };

    acecode::SessionModelReloadResult first;
    acecode::SessionModelReloadResult second;
    std::thread a([&] {
        first = binding.ensure_current(
            false, [] { return acecode::SavedModelsRevision{61}; }, resolver);
    });
    bool first_reached_resolver = false;
    {
        std::unique_lock<std::mutex> lock(mu);
        first_reached_resolver = cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return first_inside;
        });
    }
    std::thread b([&] {
        second = binding.ensure_current(
            false, [] { return acecode::SavedModelsRevision{61}; }, resolver);
    });
    {
        std::lock_guard<std::mutex> lock(mu);
        release = true;
    }
    cv.notify_all();
    a.join();
    b.join();

    ASSERT_TRUE(first_reached_resolver);
    const int reloads =
        (first.outcome == acecode::SessionModelReloadOutcome::Reloaded ? 1 : 0) +
        (second.outcome == acecode::SessionModelReloadOutcome::Reloaded ? 1 : 0);
    EXPECT_EQ(reloads, 1);
    EXPECT_EQ(resolver_calls.load(), 2);
    EXPECT_EQ(binding.provider_snapshot()->model(), "model-b");
}

// 触发场景:lazy reload 构造中同时收到 forced reload;期望 forced 等待后重查
// fingerprint,两次操作仅发布一次新 Provider。旧实现会重复构造并重复认证。
TEST(SessionModelBinding, LazyAndForcedOverlapPublishOnlyOnce) {
    acecode::AppConfig cfg;
    auto initial = profile_named("fast", "model-a");
    cfg.saved_models = {initial};
    acecode::SessionModelBinding binding;
    ASSERT_TRUE(install_initial(binding, target_for(initial, cfg, 65)).ok);

    auto changed = initial;
    changed.model = "model-b";
    cfg.saved_models = {changed};
    const auto next = target_for(changed, cfg, 66);
    std::mutex mu;
    std::condition_variable cv;
    bool first_inside = false;
    bool release = false;
    int resolver_calls = 0;
    auto resolver = [&](const std::string&) {
        std::unique_lock<std::mutex> lock(mu);
        ++resolver_calls;
        if (!first_inside) {
            first_inside = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
        }
        return next;
    };
    std::atomic<int> publications{0};
    auto transition = [&](const acecode::SessionModelState&,
                          const acecode::SessionModelTransition& change) {
        if (change.provider_published) publications.fetch_add(1);
        return true;
    };

    acecode::SessionModelReloadResult lazy;
    acecode::SessionModelReloadResult forced;
    std::thread first([&] {
        lazy = binding.ensure_current(
            false, [] { return acecode::SavedModelsRevision{66}; },
            resolver, transition);
    });
    {
        std::unique_lock<std::mutex> lock(mu);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return first_inside;
        }));
    }
    std::thread second([&] {
        forced = binding.ensure_current(
            true, [] { return acecode::SavedModelsRevision{66}; },
            resolver, transition);
    });
    {
        std::lock_guard<std::mutex> lock(mu);
        release = true;
    }
    cv.notify_all();
    first.join();
    second.join();

    ASSERT_TRUE(lazy.ok) << lazy.error;
    ASSERT_TRUE(forced.ok) << forced.error;
    EXPECT_EQ(publications.load(), 1);
    EXPECT_EQ(lazy.outcome, acecode::SessionModelReloadOutcome::Reloaded);
    EXPECT_EQ(forced.outcome,
              acecode::SessionModelReloadOutcome::AlreadyCurrent);
    EXPECT_EQ(binding.provider_snapshot()->model(), "model-b");
}

// 触发场景:旧 name 的 reload 已进入重校验时用户显式切换到新 name;期望
// 两者按 binding 串行,后发生的 switch 最终获胜,旧 reload 不能回写覆盖。
TEST(SessionModelBinding, ExplicitSwitchWinsAfterOlderReload) {
    acecode::AppConfig cfg;
    auto old_profile = profile_named("old", "model-a");
    auto new_profile = profile_named("new", "model-new");
    cfg.saved_models = {old_profile, new_profile};
    acecode::SessionModelBinding binding;
    ASSERT_TRUE(install_initial(binding, target_for(old_profile, cfg, 70)).ok);

    auto changed_old = old_profile;
    changed_old.model = "model-old-v2";
    auto changed_cfg = cfg;
    changed_cfg.saved_models = {changed_old, new_profile};
    const auto old_target = target_for(changed_old, changed_cfg, 71);
    const auto new_target = target_for(new_profile, changed_cfg, 71);
    std::mutex mu;
    std::condition_variable cv;
    bool reload_inside = false;
    bool release = false;
    auto old_resolver = [&](const std::string&) {
        std::unique_lock<std::mutex> lock(mu);
        if (!reload_inside) {
            reload_inside = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
        }
        return old_target;
    };

    acecode::SessionModelReloadResult reload;
    acecode::SessionModelReloadResult switched;
    std::thread old_worker([&] {
        reload = binding.ensure_current(
            false, [] { return acecode::SavedModelsRevision{71}; },
            old_resolver);
    });
    {
        std::unique_lock<std::mutex> lock(mu);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return reload_inside;
        }));
    }
    std::thread switch_worker([&] {
        switched = binding.install_explicit(
            new_target,
            [new_target](const std::string&) { return new_target; });
    });
    {
        std::lock_guard<std::mutex> lock(mu);
        release = true;
    }
    cv.notify_all();
    old_worker.join();
    switch_worker.join();

    ASSERT_TRUE(reload.ok) << reload.error;
    ASSERT_TRUE(switched.ok) << switched.error;
    EXPECT_EQ(binding.state_snapshot().name, "new");
    EXPECT_EQ(binding.provider_snapshot()->model(), "model-new");
    EXPECT_EQ(binding.applied_revision(), 71u);
}

// 触发场景:运行中的 turn 已捕获旧 shared_ptr,同时 future reload 发布新值;
// 期望旧 turn 的快照继续存活,后续快照才看到新 Provider。
TEST(SessionModelBinding, InFlightProviderSnapshotSurvivesReplacement) {
    acecode::AppConfig cfg;
    auto initial = profile_named("fast", "model-a");
    cfg.saved_models = {initial};
    acecode::SessionModelBinding binding;
    ASSERT_TRUE(install_initial(binding, target_for(initial, cfg, 70)).ok);
    const auto in_flight = binding.provider_snapshot();

    auto changed = initial;
    changed.model = "model-b";
    cfg.saved_models = {changed};
    const auto next = target_for(changed, cfg, 71);
    ASSERT_TRUE(binding.ensure_current(
        false,
        [] { return acecode::SavedModelsRevision{71}; },
        [next](const std::string&) { return next; }).ok);

    ASSERT_TRUE(in_flight);
    EXPECT_EQ(in_flight->model(), "model-a");
    ASSERT_TRUE(binding.provider_snapshot());
    EXPECT_EQ(binding.provider_snapshot()->model(), "model-b");
    EXPECT_NE(binding.provider_snapshot(), in_flight);
}

// 触发场景:fingerprint 相同的旧 context 快照被错误标成更高 revision;期望
// 发布 state 前再次解析,采用真正的新 context/revision。旧实现会静默保留旧值。
TEST(SessionModelBinding, MatchingFingerprintRevalidatesStateBeforePublication) {
    acecode::AppConfig cfg;
    auto profile = profile_named();
    profile.context_window = 32000;
    cfg.saved_models = {profile};
    acecode::SessionModelBinding binding;
    ASSERT_TRUE(install_initial(binding, target_for(profile, cfg, 80)).ok);
    const auto provider = binding.provider_snapshot();

    auto stale_profile = profile;
    stale_profile.context_window = 64000;
    auto stale_cfg = cfg;
    stale_cfg.saved_models = {stale_profile};
    const auto stale = target_for(stale_profile, stale_cfg, 79);

    auto current_profile = profile;
    current_profile.context_window = 96000;
    auto current_cfg = cfg;
    current_cfg.saved_models = {current_profile};
    const auto current = target_for(current_profile, current_cfg, 81);
    int resolver_calls = 0;
    const auto result = binding.ensure_current(
        true,
        [] { return acecode::SavedModelsRevision{81}; },
        [&](const std::string&) {
            return ++resolver_calls == 1 ? stale : current;
        });

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.outcome,
              acecode::SessionModelReloadOutcome::AlreadyCurrent);
    EXPECT_GE(resolver_calls, 2);
    EXPECT_EQ(binding.provider_snapshot(), provider);
    EXPECT_EQ(binding.state_snapshot().context_window, 96000);
    EXPECT_EQ(binding.applied_revision(), 81u);
}
