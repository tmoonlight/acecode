#include <gtest/gtest.h>
#include "themes/theme_drafts.hpp"
#include "image/image_processor.hpp"
#include "tui/tui_ask_channel.hpp"
#include "tui/tui_state.hpp"
#include "utils/sha256.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <zip.h>

namespace {
namespace fs = std::filesystem;
using nlohmann::json;
using namespace acecode::themes;

class ThemeDraftsTest : public ::testing::Test {
protected:
    fs::path root;
    json palette;
    std::string png;
    void SetUp() override {
        root = fs::temp_directory_path() / ("ace-draft-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        json colors = json::object();
        for (const auto* key : {"bg", "surface", "surface-alt", "surface-hi", "shell-hi", "shell-bg",
            "border", "border-soft", "fg", "fg-2", "fg-mute", "accent", "accent-bg", "accent-soft",
            "ok", "ok-bg", "ok-border", "warn", "warn-bg", "danger", "danger-bg", "code-bg", "code-fg",
            "code-line", "selection", "on-selection", "send-bg", "send-fg"}) colors[key] = "#ABCDEF";
        palette = {{"action", "palette"}, {"name", "EVA 夜色"}, {"mode", "dark"}, {"colors", colors}};
        std::string ppm = "P6\n4 2\n255\n" + std::string(24, 'x');
        acecode::image::ImageNormalizeOptions options;
        options.force_png = true;
        auto converted = acecode::image::normalize_image_bytes(ppm, "", options);
        ASSERT_TRUE(converted.ok) << converted.error;
        png = converted.bytes;
        write(root / "background.png", png);
        write(root / "preview.png", png);
    }
    void TearDown() override { std::error_code ec; fs::remove_all(root, ec); }
    void write(const fs::path& path, const std::string& bytes) {
        std::ofstream stream(path, std::ios::binary); stream.write(bytes.data(), bytes.size());
    }
    json saved_draft(const std::string& id) {
        std::ifstream stream(root / "themes/drafts" / id / "draft.json", std::ios::binary);
        return json::parse(stream);
    }
    void replace_draft(const std::string& id, const json& draft) {
        write(root / "themes/drafts" / id / "draft.json", draft.dump(2));
    }
    static json approve(const json& payload) {
        return {{"answers", json::array({{{"question_id", payload[0]["id"]},
            {"selected", json::array({payload[0]["options"][0]["value"]})}, {"custom_text", ""}}})}};
    }
    json run(const json& args, const ThemeDraftStore::Confirm& confirmation = approve,
             const std::string& session = "session-a") {
        return ThemeDraftStore(root / "themes").execute(args, session, acecode::path_to_utf8(root), confirmation);
    }
    json prototype(const std::string& id) {
        return {{"action", "prototype"}, {"draft_id", id},
            {"background_path", "background.png"}, {"preview_path", "preview.png"}};
    }
    std::string ready() {
        const auto id = run(palette).at("draft_id").get<std::string>();
        EXPECT_TRUE(run(prototype(id))["confirmed"]);
        return id;
    }
};

TEST_F(ThemeDraftsTest, OptionalBackgroundsAndOpacitiesPreserveExistingAppearanceAndApprovals) {
    palette["appearance"] = {{"logo_color", "#9B6DFF"}, {"home_title_color", "#FFFFFF"},
        {"extend_to_titlebar", true}, {"home_composer_opacity", 0.7},
        {"home_background_color", "#102030"}, {"home_background_opacity", 0.4},
        {"session_background_opacity", 0.6}, {"user_message_background_opacity", 0.8}};
    const auto id = run(palette).at("draft_id").get<std::string>();
    auto request = prototype(id);
    request["session_background_path"] = "background.png";
    request["user_message_background_path"] = "preview.png";
    EXPECT_TRUE(run(request)["confirmed"]);
    const auto installed = run({{"action", "install"}, {"draft_id", id}});
    ThemeStore store(root / "themes", "");
    const auto definition = store.definition(installed.at("id"));
    EXPECT_EQ(definition.at("appearance"), palette.at("appearance"));
    EXPECT_EQ(store.image(installed.at("id"), "session-background"), png);
    EXPECT_EQ(store.image(installed.at("id"), "user-message-background"), png);
    EXPECT_EQ(theme_image_files(definition).size(), 4u);
    EXPECT_EQ(run({{"action", "install"}, {"draft_id", id}}).at("id"), installed.at("id"));
}

TEST_F(ThemeDraftsTest, ExtraBackgroundChangesAndOmissionRequireCurrentPrototypeApproval) {
    const auto id = run(palette).at("draft_id").get<std::string>();
    auto request = prototype(id);
    request["user_message_background_path"] = "background.png";
    EXPECT_TRUE(run(request)["confirmed"]);
    write(root / "themes/drafts" / id / "user-message-background.png", png + "changed");
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", id}}), ThemeError);
    EXPECT_EQ(run({{"action", "status"}, {"draft_id", id}})["stage"], "prototype_pending");
    EXPECT_TRUE(run(request)["confirmed"]);
    EXPECT_FALSE(run(prototype(id), [](const json&) { return json{{"cancelled", true}}; })["confirmed"]);
    EXPECT_FALSE(saved_draft(id).contains("user_message_background_sha256"));
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", id}}), ThemeError);
    EXPECT_TRUE(run(prototype(id))["confirmed"]);
    const auto installed = run({{"action", "install"}, {"draft_id", id}});
    EXPECT_FALSE(ThemeStore(root / "themes", "").definition(installed.at("id")).contains("user_message_background"));
}

TEST_F(ThemeDraftsTest, OpacityRejectsInvalidValuesAndChangesInvalidateBothConfirmations) {
    for (const auto& value : {json(-0.1), json(1.01), json("0.7"), json(true), json(nullptr)}) {
        auto invalid = palette;
        invalid["appearance"] = {{"home_composer_opacity", value}};
        EXPECT_THROW(run(invalid), ThemeError) << value;
    }
    for (const auto& value : {json(0), json(1), json(0.3)})
        EXPECT_TRUE(valid_theme_appearance({{"home_background_opacity", value}}));
    palette["appearance"] = {{"logo_color", "#123456"}, {"extend_to_titlebar", true}, {"home_composer_opacity", 0.7}};
    const auto id = ready();
    auto revision = palette;
    revision["draft_id"] = id;
    revision["appearance"]["home_composer_opacity"] = 0.5;
    EXPECT_FALSE(run(revision, [](const json&) { return json{{"cancelled", true}}; })["confirmed"]);
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", id}}), ThemeError);
    EXPECT_EQ(saved_draft(id)["appearance"]["logo_color"], "#123456");
    EXPECT_TRUE(saved_draft(id)["appearance"]["extend_to_titlebar"]);
}

TEST_F(ThemeDraftsTest, ExplicitConfirmationsInstallOfflineThemeAndShareableThreeFilePackage) {
    auto first = run(palette);
    EXPECT_TRUE(first["confirmed"]);
    EXPECT_EQ(first["stage"], "prototype_pending");
    const auto id = first.at("draft_id").get<std::string>();
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", id}}), ThemeError);
    EXPECT_TRUE(run(prototype(id))["confirmed"]);
    auto installed = run({{"action", "install"}, {"draft_id", id}});
    EXPECT_EQ(installed["stage"], "installed");
    EXPECT_TRUE(installed["apply"]);
    ThemeTransport offline;
    offline.fetch = [](const std::string&) { return acecode::upgrade::HttpTextResult{503}; };
    ThemeStore store(root / "themes", "https://offline.invalid", offline);
    const auto d = store.definition(installed["id"]);
    EXPECT_EQ(d["mode"], "dark");
    EXPECT_EQ(d["colors"].size(), 28u);
    EXPECT_EQ(store.image(installed["id"], "background"), png);
    auto catalog = store.catalog();
    EXPECT_TRUE(catalog["offline"]);
    ASSERT_EQ(catalog["themes"].size(), 2u);
    EXPECT_FALSE(catalog["themes"][0]["available"]);
    EXPECT_EQ(catalog["themes"][1]["source"], "local");
    EXPECT_TRUE(catalog["themes"][1]["installed"]);
    EXPECT_EQ(catalog["themes"][1]["name"], "EVA 夜色");
    int error = 0;
    auto* archive = zip_open(installed.at("package_path").get<std::string>().c_str(), ZIP_RDONLY, &error);
    ASSERT_NE(archive, nullptr);
    EXPECT_EQ(zip_get_num_entries(archive, 0), 3);
    for (const auto* name : {"theme.json", "background.png", "thumbnail.png"}) EXPECT_GE(zip_name_locate(archive, name, 0), 0);
    zip_discard(archive);
    EXPECT_EQ(run({{"action", "install"}, {"draft_id", id}})["id"], installed["id"]);
}

TEST_F(ThemeDraftsTest, LegacyApprovalsRemainValidWithoutAppearance) {
    const auto id = ready();
    auto draft = saved_draft(id);
    ASSERT_FALSE(draft.contains("appearance"));
    const auto old_palette_hash = acecode::sha256_hex(json{{"name", draft.at("name")},
        {"mode", draft.at("mode")}, {"colors", draft.at("colors")}}.dump());
    draft["palette_approval"] = old_palette_hash;
    draft["prototype_approval"] = acecode::sha256_hex(json{{"palette", old_palette_hash},
        {"background", draft.at("background_sha256")}, {"preview", draft.at("preview_sha256")}}.dump());
    replace_draft(id, draft);
    EXPECT_EQ(run({{"action", "status"}, {"draft_id", id}})["stage"], "ready");
    const auto installed = run({{"action", "install"}, {"draft_id", id}});
    EXPECT_EQ(installed["stage"], "installed");
    EXPECT_FALSE(installed.contains("appearance"));
    EXPECT_FALSE(ThemeStore(root / "themes", "").definition(installed.at("id")).contains("appearance"));
    EXPECT_EQ(run({{"action", "install"}, {"draft_id", id}})["id"], installed.at("id"));
}

TEST_F(ThemeDraftsTest, AppearanceSurvivesWorkflowAndInstalledRetryRejectsChangedOverrides) {
    const json appearance = {{"logo_color", "#9B6DFF"}, {"home_title_color", "#F5F0FF"},
        {"extend_to_titlebar", true}};
    palette["appearance"] = appearance;
    const auto id = ready();
    EXPECT_EQ(run({{"action", "status"}, {"draft_id", id}})["appearance"], appearance);
    EXPECT_EQ(run({{"action", "status"}})["drafts"][0]["appearance"], appearance);
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", id}, {"appearance", appearance}}), ThemeError);
    const auto installed = run({{"action", "install"}, {"draft_id", id}});
    EXPECT_EQ(installed["appearance"], appearance);
    ThemeStore store(root / "themes", "");
    auto definition = store.definition(installed.at("id"));
    EXPECT_EQ(definition.at("appearance"), appearance);
    EXPECT_EQ(run({{"action", "install"}, {"draft_id", id}})["id"], installed.at("id"));
    definition["appearance"]["logo_color"] = "#123456";
    write(acecode::path_from_utf8(installed.at("installed_path")) / "theme.json", definition.dump(2));
    try {
        run({{"action", "install"}, {"draft_id", id}});
        FAIL() << "A changed installed appearance must not reuse the package";
    } catch (const ThemeError& error) {
        EXPECT_EQ(error.code, "THEME_VERSION_CONFLICT");
    }
}

TEST_F(ThemeDraftsTest, AppearanceChangesAndOmissionRequireNewConfirmations) {
    palette["appearance"] = {{"logo_color", "#9B6DFF"}, {"extend_to_titlebar", true}};
    const auto id = ready();
    auto revision = palette;
    revision["draft_id"] = id;
    revision["appearance"]["extend_to_titlebar"] = false;
    const auto cancelled = run(revision, [](const json&) { return json{{"cancelled", true}}; });
    EXPECT_FALSE(cancelled["confirmed"]);
    EXPECT_EQ(cancelled["stage"], "palette_pending");
    EXPECT_THROW(run(prototype(id)), ThemeError);
    EXPECT_TRUE(run(revision)["confirmed"]);
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", id}}), ThemeError);
    EXPECT_TRUE(run(prototype(id))["confirmed"]);
    revision.erase("appearance");
    const auto removed = run(revision);
    EXPECT_TRUE(removed["confirmed"]);
    EXPECT_FALSE(removed.contains("appearance"));
    EXPECT_FALSE(saved_draft(id).contains("appearance"));
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", id}}), ThemeError);
    EXPECT_TRUE(run(prototype(id))["confirmed"]);
    const auto installed = run({{"action", "install"}, {"draft_id", id}});
    EXPECT_FALSE(ThemeStore(root / "themes", "").definition(installed.at("id")).contains("appearance"));
}

TEST_F(ThemeDraftsTest, StoredAppearanceAdditionChangeAndRemovalInvalidateApproval) {
    const json appearance = {{"logo_color", "#9B6DFF"}, {"home_title_color", "#FFFFFF"},
        {"extend_to_titlebar", true}};
    const auto legacy_id = ready();
    auto legacy = saved_draft(legacy_id);
    legacy["appearance"] = appearance;
    replace_draft(legacy_id, legacy);
    EXPECT_EQ(run({{"action", "status"}, {"draft_id", legacy_id}})["stage"], "palette_pending");
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", legacy_id}}), ThemeError);

    palette["appearance"] = appearance;
    for (const auto* key : {"logo_color", "home_title_color", "extend_to_titlebar", "appearance"}) {
        const auto id = ready();
        auto draft = saved_draft(id);
        if (std::string(key) == "appearance") draft.erase("appearance");
        else draft["appearance"][key] = std::string(key) == "extend_to_titlebar" ? json(false) : json("#123456");
        replace_draft(id, draft);
        EXPECT_EQ(run({{"action", "status"}, {"draft_id", id}})["stage"], "palette_pending") << key;
        EXPECT_THROW(run({{"action", "install"}, {"draft_id", id}}), ThemeError) << key;
    }
}

TEST_F(ThemeDraftsTest, EmptyAppearanceRemainsExplicitThroughInstallation) {
    palette["appearance"] = json::object();
    const auto id = ready();
    const auto installed = run({{"action", "install"}, {"draft_id", id}});
    ASSERT_TRUE(installed.contains("appearance"));
    EXPECT_EQ(installed.at("appearance"), json::object());
    EXPECT_EQ(ThemeStore(root / "themes", "").definition(installed.at("id")).at("appearance"), json::object());
}

TEST_F(ThemeDraftsTest, CancelTimeoutCustomTextAndMissingChannelNeverApprove) {
    const std::vector<ThemeDraftStore::Confirm> responses = {
        {}, [](const json&) { return json{{"cancelled", true}}; },
        [](const json& p) { auto r = approve(p); r["timed_out"] = true; return r; },
        [](const json& p) { auto r = approve(p); r["auto_answered"] = true; return r; },
        [](const json& p) { auto r = approve(p); r["answers"][0]["question_id"] = "stale-question"; return r; },
        [](const json& p) { auto r = approve(p); r["answers"][0]["selected"].push_back("修改"); return r; },
        [](const json& p) { auto r = approve(p); r["answers"][0]["custom_text"] = "Change the colors"; return r; },
        [](const json& p) { auto r = approve(p); r["answers"][0]["selected"] = {"修改"}; return r; }
    };
    for (const auto& callback : responses) {
        const auto state = run(palette, callback);
        EXPECT_FALSE(state["confirmed"]);
        EXPECT_EQ(state["stage"], "palette_pending");
        EXPECT_THROW(run(prototype(state.at("draft_id"))), ThemeError);
    }
}

TEST_F(ThemeDraftsTest, ActualTuiChannelPreservesIdsAndApprovesPaletteAndPrototype) {
    acecode::TuiState state;
    state.ask_config.selection_feedback_ms = 0;
    auto screen = ftxui::ScreenInteractive::FitComponent();
    std::atomic<bool> abort{false};
    const auto via_tui = [&](const json& payload) {
        EXPECT_NE(payload[0]["id"], payload[0]["text"]);
        auto future = std::async(std::launch::async, [&] {
            return acecode::tui::ask_via_tui_overlay(state, screen, payload, &abort, 0, "");
        });
        bool opened = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(state.mu);
                if (state.ask_pending) {
                    opened = true;
                    // Exercise the same controller path as a keyboard answer.
                    state.ask_session->dispatch({
                        acecode::tui::AskQuestionEventKind::ChooseNumber, 1});
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!opened) abort.store(true);
        state.ask_cv.notify_all();
        EXPECT_TRUE(opened);
        auto response = future.get();
        if (opened) EXPECT_EQ(response["answers"][0]["question_id"], payload[0]["id"]);
        return response;
    };
    const auto color_state = run(palette, via_tui);
    EXPECT_TRUE(color_state["confirmed"]);
    const auto id = color_state.at("draft_id").get<std::string>();
    EXPECT_TRUE(run(prototype(id), via_tui)["confirmed"]);
    EXPECT_EQ(run({{"action", "install"}, {"draft_id", id}})["stage"], "installed");
}

TEST_F(ThemeDraftsTest, DraftsRemainSessionOwnedAcrossInstancesAndRejectApprovalArguments) {
    const auto id = ready();
    EXPECT_EQ(run({{"action", "status"}, {"draft_id", id}})["stage"], "ready");
    EXPECT_EQ(run({{"action", "status"}})["drafts"].size(), 1u);
    EXPECT_TRUE(run({{"action", "status"}}, approve, "session-b")["drafts"].empty());
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", id}}, approve, "session-b"), ThemeError);
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", id}, {"confirmed", true}}), ThemeError);
    EXPECT_THROW(run({{"action", "status"}, {"draft_id", "../escape"}}), ThemeError);
}

TEST_F(ThemeDraftsTest, PaletteRevisionAndChangedStoredImagesInvalidatePrototypeApproval) {
    auto id = ready();
    auto updated = palette;
    updated["draft_id"] = id;
    updated["colors"]["accent"] = "#123456";
    EXPECT_TRUE(run(updated)["confirmed"]);
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", id}}), ThemeError);
    EXPECT_TRUE(run(prototype(id))["confirmed"]);
    write(root / "themes/drafts" / id / "background.png", png + "changed");
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", id}}), ThemeError);
    EXPECT_EQ(run({{"action", "status"}, {"draft_id", id}})["stage"], "prototype_pending");
}

TEST_F(ThemeDraftsTest, CopiedResourcesSurviveOriginalRemovalAndBadImageCannotInstall) {
    const auto id = ready();
    fs::remove(root / "background.png");
    fs::remove(root / "preview.png");
    EXPECT_EQ(run({{"action", "install"}, {"draft_id", id}})["stage"], "installed");
    const auto second = run(palette).at("draft_id").get<std::string>();
    write(root / "background.png", std::string("\x89PNG\r\n\x1a\n", 8) + std::string(24, 'x'));
    write(root / "preview.png", png);
    EXPECT_THROW(run(prototype(second)), ThemeError);
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", second}}), ThemeError);
}

TEST_F(ThemeDraftsTest, ConcurrentPaletteUpdateWaitsForApprovalAndThenInvalidatesIt) {
    const auto id = run(palette).at("draft_id").get<std::string>();
    std::promise<void> waiting, release;
    auto released = release.get_future().share();
    auto approval = std::async(std::launch::async, [&] {
        return run(prototype(id), [&](const json& p) {
            waiting.set_value(); released.wait(); return approve(p);
        });
    });
    waiting.get_future().wait();
    auto updated = palette; updated["draft_id"] = id; updated["colors"]["accent"] = "#123456";
    auto revision = std::async(std::launch::async, [&] { return run(updated); });
    const auto pending = revision.wait_for(std::chrono::milliseconds(50));
    release.set_value();
    EXPECT_EQ(pending, std::future_status::timeout);
    EXPECT_TRUE(approval.get()["confirmed"]);
    EXPECT_TRUE(revision.get()["confirmed"]);
    EXPECT_THROW(run({{"action", "install"}, {"draft_id", id}}), ThemeError);
}

TEST(ThemeLocalId, RejectsPathsAndMatchesFrontendSlugRules) {
    for (const auto* id : {"ai-eva", "ai-eva-01", "ai-0"}) EXPECT_TRUE(is_local_theme(id));
    for (const auto* id : {"ai-", "ai--a", "ai-a-", "ai-a--b", "ai-../a", "ai-a/b", "ai-A", "eva-01", "blue"})
        EXPECT_FALSE(is_local_theme(id));
    EXPECT_TRUE(is_local_theme("ai-" + std::string(61, 'a')));
    EXPECT_FALSE(is_local_theme("ai-" + std::string(62, 'a')));
}
} // namespace
