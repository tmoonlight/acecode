#include "config/settings_mutations.hpp"
#include "config/saved_models_revision.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

class SettingsMutationTempDir {
public:
    SettingsMutationTempDir() {
        path_ = std::filesystem::temp_directory_path() /
            ("acecode-settings-mutations-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }

    ~SettingsMutationTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    std::string config_path() const {
        return (path_ / "config.json").string();
    }

private:
    std::filesystem::path path_;
};

acecode::SettingsMutationOptions options_for(
    const SettingsMutationTempDir& temp,
    acecode::AppConfig* live = nullptr) {
    acecode::SettingsMutationOptions options;
    options.config_path = temp.config_path();
    options.live_config = live;
    return options;
}

acecode::SavedModelDraft model_draft(std::string name) {
    acecode::SavedModelDraft draft;
    draft.name = std::move(name);
    draft.provider = "openai";
    draft.model = "test-model";
    draft.base_url = "https://example.invalid/v1";
    draft.api_key = "secret";
    return draft;
}

} // namespace

TEST(SettingsMutations, ScalarPatchPreservesLatestUnrelatedFields) {
    SettingsMutationTempDir temp;
    acecode::AppConfig initial;
    initial.max_sessions = 91;
    initial.upgrade.base_url = "https://old.example/";
    acecode::save_config(initial, temp.config_path());

    acecode::AppConfig live;
    const auto theme = acecode::set_tui_theme(
        "light",
        options_for(temp, &live));
    ASSERT_TRUE(theme.ok) << theme.error;
    EXPECT_TRUE(theme.persisted);
    EXPECT_EQ(theme.runtime_status, acecode::SettingsRuntimeStatus::AppliedLive);
    EXPECT_EQ(live.tui.theme, "light");

    const auto notifications = acecode::set_native_notifications_enabled(
        false,
        options_for(temp, &live));
    ASSERT_TRUE(notifications.ok) << notifications.error;

    const auto saved = acecode::load_config_from_path(temp.config_path());
    EXPECT_EQ(saved.max_sessions, 91);
    EXPECT_EQ(saved.upgrade.base_url, "https://old.example/");
    EXPECT_EQ(saved.tui.theme, "light");
    EXPECT_FALSE(saved.desktop.notifications.enabled);
}

TEST(SettingsMutations, InvalidScalarDoesNotReplaceConfirmedState) {
    SettingsMutationTempDir temp;
    acecode::AppConfig initial;
    initial.tui.theme = "dark";
    acecode::save_config(initial, temp.config_path());

    acecode::AppConfig live = initial;
    const auto result = acecode::set_tui_theme(
        "sepia",
        options_for(temp, &live));

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.persisted);
    EXPECT_EQ(
        result.error_kind,
        acecode::SettingsMutationErrorKind::Validation);
    EXPECT_EQ(live.tui.theme, "dark");
    EXPECT_EQ(
        acecode::load_config_from_path(temp.config_path()).tui.theme,
        "dark");
}

TEST(SettingsMutations, RuntimeFailureReportsPersistedStateWithoutSecret) {
    SettingsMutationTempDir temp;
    acecode::AppConfig initial;
    acecode::save_config(initial, temp.config_path());

    auto options = options_for(temp);
    options.apply_live = [](const acecode::AppConfig&, std::string& error) {
        error = "palette refresh failed";
        return false;
    };
    const auto result = acecode::set_tui_theme("light", options);

    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.persisted);
    EXPECT_EQ(
        result.runtime_status,
        acecode::SettingsRuntimeStatus::RuntimeApplyFailed);
    EXPECT_EQ(
        result.error_kind,
        acecode::SettingsMutationErrorKind::RuntimeApply);
    EXPECT_EQ(result.error, "palette refresh failed");
}

TEST(SettingsMutations, UpgradeUrlIsNormalizedAndInstructionsAreBounded) {
    SettingsMutationTempDir temp;
    acecode::AppConfig initial;
    acecode::save_config(initial, temp.config_path());

    auto url_result = acecode::set_upgrade_base_url(
        "  https://updates.example/channel  ",
        options_for(temp));
    ASSERT_TRUE(url_result.ok) << url_result.error;
    EXPECT_EQ(
        acecode::load_config_from_path(temp.config_path()).upgrade.base_url,
        "https://updates.example/channel/");

    const std::string oversized(
        acecode::kCustomInstructionsMaxBytes + 1,
        'x');
    auto instructions_result = acecode::set_custom_instructions(
        oversized,
        options_for(temp));
    EXPECT_FALSE(instructions_result.ok);
    EXPECT_EQ(
        instructions_result.error_kind,
        acecode::SettingsMutationErrorKind::Validation);
    EXPECT_EQ(
        instructions_result.error.find(oversized),
        std::string::npos);
}

TEST(SettingsMutations, RemoteWebModePersistsProxyIntentAndKeepsDaemonLocal) {
    SettingsMutationTempDir temp;
    acecode::AppConfig initial;
    initial.web.bind = "192.168.1.99";
    initial.web.remote_enabled = true;
    acecode::save_config(initial, temp.config_path());

    acecode::AppConfig live = initial;
    live.web.port = 45678; // Simulates a Desktop/CLI runtime port override.
    const auto disabled = acecode::set_remote_web_enabled(
        false,
        options_for(temp, &live));
    ASSERT_TRUE(disabled.ok) << disabled.error;
    EXPECT_TRUE(disabled.persisted);
    EXPECT_EQ(live.web.bind, "127.0.0.1");
    EXPECT_FALSE(live.web.remote_enabled);
    EXPECT_EQ(live.web.port, 45678);
    EXPECT_EQ(
        acecode::load_config_from_path(temp.config_path()).web.bind,
        "127.0.0.1");
    EXPECT_EQ(
        acecode::load_config_from_path(temp.config_path()).web.port,
        initial.web.port);

    const auto enabled = acecode::set_remote_web_enabled(
        true,
        options_for(temp, &live));
    ASSERT_TRUE(enabled.ok) << enabled.error;
    EXPECT_TRUE(enabled.persisted);
    EXPECT_EQ(live.web.bind, "127.0.0.1");
    EXPECT_TRUE(live.web.remote_enabled);
    EXPECT_EQ(live.web.port, 45678);
    const auto saved = acecode::load_config_from_path(temp.config_path());
    EXPECT_EQ(saved.web.bind, "127.0.0.1");
    EXPECT_TRUE(saved.web.remote_enabled);
}

TEST(SettingsMutations, SavedModelsReuseEditorAndHonorBusyDeleteGuard) {
    SettingsMutationTempDir temp;
    acecode::AppConfig initial;
    acecode::save_config(initial, temp.config_path());

    const auto add = acecode::add_saved_model_setting(
        model_draft("primary"),
        options_for(temp));
    ASSERT_TRUE(add.ok) << add.error;

    const auto duplicate = acecode::add_saved_model_setting(
        model_draft("primary"),
        options_for(temp));
    EXPECT_FALSE(duplicate.ok);
    EXPECT_EQ(
        duplicate.error_kind,
        acecode::SettingsMutationErrorKind::Validation);
    EXPECT_EQ(duplicate.error.find("secret"), std::string::npos);

    const auto make_default = acecode::set_default_model_setting(
        "primary",
        options_for(temp));
    ASSERT_TRUE(make_default.ok) << make_default.error;

    const auto busy_remove = acecode::remove_saved_model_setting(
        "primary",
        [](const std::string&) { return true; },
        options_for(temp));
    EXPECT_FALSE(busy_remove.ok);
    EXPECT_EQ(
        acecode::load_config_from_path(temp.config_path()).saved_models.size(),
        1u);

    auto renamed = model_draft("renamed");
    renamed.model = "new-model";
    const auto update = acecode::update_saved_model_setting(
        "primary",
        renamed,
        options_for(temp));
    ASSERT_TRUE(update.ok) << update.error;

    const auto saved = acecode::load_config_from_path(temp.config_path());
    ASSERT_EQ(saved.saved_models.size(), 1u);
    EXPECT_EQ(saved.saved_models.front().name, "renamed");
    EXPECT_EQ(saved.default_model_name, "renamed");
}

TEST(SettingsMutations, AdvancedModelAndCredentialReusePersistAtomically) {
    SettingsMutationTempDir temp;
    acecode::AppConfig initial;
    acecode::save_config(initial, temp.config_path());

    auto source = model_draft("source");
    source.models_dev_provider_id = "openrouter";
    source.base_url = "https://openrouter.ai/api/v1/";
    auto added = acecode::add_saved_model_setting(source, options_for(temp));
    ASSERT_TRUE(added.ok) << added.error;

    auto reused = model_draft("reused");
    reused.api_key.clear();
    reused.api_key_supplied = false;
    reused.base_url = "HTTPS://OPENROUTER.AI/api/v1";
    reused.models_dev_provider_id = "openrouter";
    reused.credential_source_name = "source";
    reused.endpoint_mode = "base_url";
    reused.max_output_tokens = 64000;
    reused.capabilities = {"reasoning"};
    reused.capabilities_source = "manual";
    acecode::ModelReasoningOptions reasoning;
    reasoning.supported = true;
    reasoning.default_enabled = true;
    reasoning.supported_efforts = {"low", "high"};
    reasoning.default_effort = "low";
    reasoning.effort = "high";
    reasoning.supports_max_tokens = false;
    reused.reasoning = reasoning;
    auto copied = acecode::add_saved_model_setting(reused, options_for(temp));
    ASSERT_TRUE(copied.ok) << copied.error;

    const auto saved = acecode::load_config_from_path(temp.config_path());
    ASSERT_EQ(saved.saved_models.size(), 2u);
    EXPECT_EQ(saved.saved_models[1].api_key, "secret");
    EXPECT_EQ(saved.saved_models[1].max_output_tokens, 64000);
    ASSERT_TRUE(saved.saved_models[1].reasoning.has_value());
    EXPECT_EQ(saved.saved_models[1].reasoning->effort, "high");

    auto incompatible = reused;
    incompatible.name = "rejected";
    incompatible.models_dev_provider_id = "different";
    const auto rejected = acecode::add_saved_model_setting(
        incompatible, options_for(temp));
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error_code, "INVALID_CREDENTIAL_SOURCE");
    EXPECT_EQ(rejected.error.find("secret"), std::string::npos);
    EXPECT_EQ(acecode::load_config_from_path(temp.config_path()).saved_models.size(),
              2u);
}

// 触发场景:同一 running process 依次 add、有效 update、remove saved model;
// 期望持久化并发布 live list 后每次 revision 恰好 +1。旧实现没有完整失效信号。
TEST(SettingsMutations, ChangedSavedModelMutationsAdvanceRevisionExactlyOnce) {
    SettingsMutationTempDir temp;
    acecode::AppConfig initial;
    acecode::save_config(initial, temp.config_path());
    acecode::AppConfig live = initial;
    const auto options = options_for(temp, &live);

    auto before = acecode::current_saved_models_revision();
    const auto added = acecode::add_saved_model_setting(
        model_draft("primary"), options);
    ASSERT_TRUE(added.ok) << added.error;
    EXPECT_EQ(acecode::current_saved_models_revision(), before + 1);

    before = acecode::current_saved_models_revision();
    auto changed = model_draft("primary");
    changed.model = "changed-model";
    const auto updated = acecode::update_saved_model_setting(
        "primary", changed, options);
    ASSERT_TRUE(updated.ok) << updated.error;
    EXPECT_EQ(acecode::current_saved_models_revision(), before + 1);

    before = acecode::current_saved_models_revision();
    const auto removed = acecode::remove_saved_model_setting(
        "primary", {}, options);
    ASSERT_TRUE(removed.ok) << removed.error;
    EXPECT_EQ(acecode::current_saved_models_revision(), before + 1);
}

// 触发场景:结构相同的 update、validation failure、persistence failure 和
// live apply failure;期望 revision 均不动。旧的“请求次数”计数会误触发重建。
TEST(SettingsMutations, NoOpAndEveryFailureClassLeaveRevisionUnchanged) {
    SettingsMutationTempDir temp;
    acecode::AppConfig initial;
    initial.saved_models.push_back({
        "primary", "openai", "https://example.invalid/v1", "secret",
        "test-model",
    });
    acecode::save_config(initial, temp.config_path());
    acecode::AppConfig live = initial;

    auto before = acecode::current_saved_models_revision();
    const auto unchanged = acecode::update_saved_model_setting(
        "primary", model_draft("primary"), options_for(temp, &live));
    ASSERT_TRUE(unchanged.ok) << unchanged.error;
    EXPECT_FALSE(unchanged.changed);
    EXPECT_EQ(acecode::current_saved_models_revision(), before);

    const auto invalid = acecode::add_saved_model_setting(
        model_draft("primary"), options_for(temp, &live));
    EXPECT_FALSE(invalid.ok);
    EXPECT_EQ(acecode::current_saved_models_revision(), before);

    acecode::SettingsMutationOptions bad_path;
    bad_path.config_path = std::filesystem::path(temp.config_path())
        .parent_path().string();
    bad_path.live_config = &live;
    const auto persistence = acecode::add_saved_model_setting(
        model_draft("other"), bad_path);
    EXPECT_FALSE(persistence.ok);
    EXPECT_EQ(persistence.error_kind,
              acecode::SettingsMutationErrorKind::Persistence);
    EXPECT_EQ(acecode::current_saved_models_revision(), before);

    auto runtime_options = options_for(temp, &live);
    runtime_options.apply_live = [](
        const acecode::AppConfig&, std::string& error) {
        error = "synthetic runtime apply failure";
        return false;
    };
    const auto runtime = acecode::add_saved_model_setting(
        model_draft("runtime-failure"), runtime_options);
    EXPECT_TRUE(runtime.ok);
    EXPECT_EQ(runtime.runtime_status,
              acecode::SettingsRuntimeStatus::RuntimeApplyFailed);
    EXPECT_EQ(acecode::current_saved_models_revision(), before);
    EXPECT_EQ(live.saved_models.size(), 1u);

    // 同一请求重试时磁盘已包含该条目,仍属于 no-op;不能绕过 publisher
    // 把失败过的列表偷偷复制到 live config,也不能补发一个 revision。
    const auto retry = acecode::update_saved_model_setting(
        "runtime-failure",
        model_draft("runtime-failure"),
        options_for(temp, &live));
    ASSERT_TRUE(retry.ok) << retry.error;
    EXPECT_FALSE(retry.changed);
    EXPECT_EQ(acecode::current_saved_models_revision(), before);
    EXPECT_EQ(live.saved_models.size(), 1u);
}

// 触发场景:独立 `acecode configure` 只有磁盘配置、没有 running live config;
// 期望成功写盘但不制造本进程 revision。运行中进程须先显式加载该编辑。
TEST(SettingsMutations, DiskOnlyConfigureMutationDoesNotAdvanceProcessRevision) {
    SettingsMutationTempDir temp;
    acecode::AppConfig initial;
    acecode::save_config(initial, temp.config_path());
    const auto before = acecode::current_saved_models_revision();

    const auto result = acecode::add_saved_model_setting(
        model_draft("external"), options_for(temp));

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.persisted);
    EXPECT_EQ(acecode::current_saved_models_revision(), before);
}
