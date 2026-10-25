#include <gtest/gtest.h>
#include "desktop/desktop_office_service.hpp"
#include <chrono>
#include <map>
#include <vector>

namespace {
using Json = nlohmann::json;
struct FakeHost {
    std::map<std::string, std::function<std::string(const std::string&)>> bindings;
    std::vector<std::string> events;
    void bind(const std::string& name, std::function<std::string(const std::string&)> fn) {
        bindings[name] = std::move(fn);
    }
    void eval(const std::string& value) { events.push_back(value); }
    Json call(const std::string& name, const Json& args = Json::array()) {
        return Json::parse(bindings.at(name)(args.dump()));
    }
};
struct FakeWindow {
    explicit FakeWindow(FakeHost&) {}
    std::function<void()> on_closed;
    std::function<void(const std::string&)> on_pet_kind;
    Json snapshot;
    std::string pet_kind;
    static inline int starts = 0;
    static inline int closes = 0;
    static inline bool fail = false;
    static inline FakeWindow* current = nullptr; // Test-local borrow, never used after teardown.
    bool start() { starts++; current = this; return !fail; }
    void close() { closes++; if (on_closed) on_closed(); }
    void update_snapshot(const Json& value) { snapshot = value; }
    void set_pet_kind(const std::string& kind) { pet_kind = kind; }
};
using Service = acecode::desktop::DesktopOfficeService<FakeHost, FakeWindow>;
class DesktopOfficeServiceTest : public testing::Test {
protected:
    std::filesystem::path dir;
    FakeHost host;
    std::shared_ptr<Service> service; // Fixture and callbacks temporarily share the service.
    void SetUp() override {
        dir = std::filesystem::path(testing::TempDir()) / ("ace-office-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        FakeWindow::starts = FakeWindow::closes = 0;
        FakeWindow::fail = false;
        FakeWindow::current = nullptr;
    }
    void TearDown() override {
        service.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void open(bool available = true) {
        service.reset(); host.bindings.clear();
        service = std::make_shared<Service>(host, dir / "office.json", available);
        service->bind_bridge();
    }
};

TEST_F(DesktopOfficeServiceTest, DefaultsOffWithoutStartupAnnouncementsOrWrites) {
    for (int restart = 0; restart < 2; ++restart) {
        open();
        EXPECT_FALSE(service->state()["enabled"].get<bool>());
        EXPECT_EQ(service->state()["petKind"], "office");
        EXPECT_FALSE(service->state().contains("welcomePending"));
        EXPECT_FALSE(service->state().contains("petsPromoPending"));
        EXPECT_EQ(host.bindings.count("aceDesktop_claimOfficeWelcome"), 0u);
        EXPECT_EQ(host.bindings.count("aceDesktop_claimPetsPromo"), 0u);
        EXPECT_EQ(host.bindings.count("aceDesktop_getOfficePreview"), 0u);
        EXPECT_FALSE(std::filesystem::exists(dir / "office.json"));
    }
    EXPECT_EQ(FakeWindow::starts, 0);
}

TEST_F(DesktopOfficeServiceTest, LegacyAnnouncementFlagsPreserveUserPreferencesWithoutPrompts) {
    std::filesystem::create_directories(dir);
    for (const bool enabled : {false, true}) {
        for (const auto& flags : {Json::object(), Json{{"welcome_seen", false}},
                                 Json{{"welcome_seen", true}},
                                 Json{{"welcome_seen", true}, {"pets_promo_seen", false}},
                                 Json{{"welcome_seen", true}, {"pets_promo_seen", true}}}) {
            service.reset();
            Json stored = flags;
            stored["enabled"] = enabled;
            stored["pet_kind"] = "whale";
            { std::ofstream file(dir / "office.json"); file << stored.dump(); }
            for (int restart = 0; restart < 2; ++restart) {
                open();
                const Json state = host.call("aceDesktop_getOfficePreferences");
                EXPECT_EQ(state["enabled"], enabled);
                EXPECT_EQ(state["petKind"], "whale");
                EXPECT_FALSE(state.contains("welcomePending"));
                EXPECT_FALSE(state.contains("petsPromoPending"));
                EXPECT_EQ(host.bindings.count("aceDesktop_claimOfficeWelcome"), 0u);
                EXPECT_EQ(host.bindings.count("aceDesktop_claimPetsPromo"), 0u);
                if (enabled) EXPECT_EQ(FakeWindow::current->pet_kind, "whale");
                std::ifstream file(dir / "office.json");
                EXPECT_EQ(Json::parse(file), stored);
            }
            ASSERT_TRUE(service->set_pet_kind("clippy")["ok"].get<bool>());
            {
                std::ifstream file(dir / "office.json");
                const Json saved = Json::parse(file);
                EXPECT_EQ(saved, (Json{{"enabled", enabled}, {"pet_kind", "clippy"}}));
            }
            open();
            EXPECT_EQ(service->state()["enabled"], enabled);
            EXPECT_EQ(service->state()["petKind"], "clippy");
        }
    }
}

TEST_F(DesktopOfficeServiceTest, EnablingPersistsAndClosingReopensWithLatestSnapshot) {
    open();
    ASSERT_TRUE(service->set_enabled(true)["ok"].get<bool>());
    const Json snapshot = {{"version", 1}, {"agents", Json::array()}, {"offices", Json::array()},
                           {"selected", {{"sessionId", "current"}}}};
    EXPECT_EQ(host.call("aceDesktop_updateOffice", Json::array({snapshot.dump()})), true);
    EXPECT_EQ(FakeWindow::current->snapshot, snapshot);
    FakeWindow::current->close();
    EXPECT_FALSE(service->state()["enabled"].get<bool>());
    EXPECT_EQ(host.call("aceDesktop_updateOffice", Json::array({snapshot.dump()})), false);
    ASSERT_TRUE(service->set_enabled(true)["ok"].get<bool>());
    EXPECT_EQ(FakeWindow::starts, 2);
    EXPECT_EQ(FakeWindow::current->snapshot, snapshot);
    open();
    EXPECT_TRUE(service->state()["enabled"].get<bool>());
    EXPECT_EQ(FakeWindow::starts, 3);
    EXPECT_TRUE(service->set_enabled(false)["ok"].get<bool>());
    open();
    EXPECT_FALSE(service->state()["enabled"].get<bool>());
}

TEST_F(DesktopOfficeServiceTest, OldWindowCallbacksCannotDisableRecreatedOffice) {
    open(); service->set_enabled(true);
    auto old_callback = FakeWindow::current->on_closed;
    service->set_enabled(false);
    service->set_enabled(true);
    old_callback();
    EXPECT_TRUE(service->state()["enabled"].get<bool>());
    service.reset();
    EXPECT_FALSE(host.call("aceDesktop_setOfficeEnabled", Json::array({true}))["ok"].get<bool>());
    old_callback();
}

TEST_F(DesktopOfficeServiceTest, SaveFailureAndOpenFailureDoNotReportSuccess) {
    std::filesystem::create_directories(dir);
    { std::ofstream file(dir / "office.json"); file << "{}"; }
    // A directory at the atomic temporary-file path is a portable write failure.
    std::filesystem::create_directory(dir / "office.json.tmp");
    open();
    EXPECT_FALSE(service->set_enabled(true)["ok"].get<bool>());
    EXPECT_EQ(FakeWindow::starts, 0);
    std::filesystem::remove(dir / "office.json.tmp");
    FakeWindow::fail = true;
    EXPECT_FALSE(service->set_enabled(true)["ok"].get<bool>());
    EXPECT_FALSE(service->state()["enabled"].get<bool>());
    EXPECT_GT(FakeWindow::closes, 0);
}

TEST_F(DesktopOfficeServiceTest, DisabledEnvironmentAndInvalidMessagesAreRejected) {
    open(false);
    EXPECT_FALSE(service->set_enabled(true)["ok"].get<bool>());
    open(); service->set_enabled(true);
    EXPECT_FALSE(host.call("aceDesktop_setOfficeEnabled", Json::array({"true"}))["ok"].get<bool>());
    EXPECT_EQ(host.call("aceDesktop_updateOffice", Json::array({"{}"})), false);
    EXPECT_EQ(host.call("aceDesktop_updateOffice", Json::array({"not json"})), false);
}

TEST_F(DesktopOfficeServiceTest, PetSelectionPersistsIndependentlyAndUpdatesTheCurrentWindow) {
    open();
    EXPECT_EQ(service->state()["petKind"], "office");
    ASSERT_TRUE(host.call("aceDesktop_setPetKind", Json::array({"clippy"}))["ok"].get<bool>());
    EXPECT_FALSE(service->state()["enabled"].get<bool>());
    EXPECT_EQ(FakeWindow::starts, 0);
    ASSERT_TRUE(service->set_enabled(true)["ok"].get<bool>());
    EXPECT_EQ(FakeWindow::current->pet_kind, "clippy");
    for (const auto* kind : {"rover", "whale", "office"}) {
        ASSERT_TRUE(host.call("aceDesktop_setPetKind", Json::array({kind}))["ok"].get<bool>());
        EXPECT_EQ(FakeWindow::current->pet_kind, kind);
        EXPECT_EQ(FakeWindow::starts, 1);
    }
    service->set_pet_kind("whale");
    FakeWindow::current->close();
    open();
    EXPECT_EQ(service->state()["petKind"], "whale");
    EXPECT_FALSE(service->state()["enabled"].get<bool>());
    service->set_enabled(true);
    EXPECT_EQ(FakeWindow::current->pet_kind, "whale");
}

// 场景:用户在宠物自己的右键菜单「切换宠物」里选了鲸鱼娘,之后重启 ACECode。
// 期望:与设置页同一条路径——当前窗口换成鲸鱼娘、主页面收到偏好更新事件、写进 office.json,
// 重启后仍是鲸鱼娘;再选同一只宠物不会重复保存或通知。
TEST_F(DesktopOfficeServiceTest, PetMenuSwitchPersistsUpdatesWindowAndNotifiesSettings) {
    open();
    ASSERT_TRUE(service->set_enabled(true)["ok"].get<bool>());
    auto* window = FakeWindow::current;
    ASSERT_TRUE(window && window->on_pet_kind);
    host.events.clear();
    window->on_pet_kind("whale");
    EXPECT_EQ(window->pet_kind, "whale");
    EXPECT_EQ(service->state()["petKind"], "whale");
    ASSERT_FALSE(host.events.empty());
    EXPECT_NE(host.events.back().find("\"petKind\":\"whale\""), std::string::npos);
    const auto notified = host.events.size();
    window->on_pet_kind("whale");
    EXPECT_EQ(host.events.size(), notified);
    open();
    EXPECT_EQ(service->state()["petKind"], "whale");
    ASSERT_TRUE(service->set_enabled(true)["ok"].get<bool>());
    EXPECT_EQ(FakeWindow::current->pet_kind, "whale");
}

TEST_F(DesktopOfficeServiceTest, PetSelectionRejectsInvalidValuesAndRollsBackSaveFailures) {
    std::filesystem::create_directories(dir);
    { std::ofstream file(dir / "office.json"); file << R"({"enabled":true,"pet_kind":{}})"; }
    open();
    EXPECT_EQ(FakeWindow::current->pet_kind, "office");
    for (const auto& invalid : {Json("unknown"), Json(nullptr), Json(true), Json::array()})
        EXPECT_FALSE(host.call("aceDesktop_setPetKind", Json::array({invalid}))["ok"].get<bool>());
    std::filesystem::create_directory(dir / "office.json.tmp");
    EXPECT_FALSE(service->set_pet_kind("clippy")["ok"].get<bool>());
    EXPECT_EQ(service->state()["petKind"], "office");
    EXPECT_EQ(FakeWindow::current->pet_kind, "office");
    std::filesystem::remove(dir / "office.json.tmp");
    open(false);
    EXPECT_FALSE(service->set_pet_kind("clippy")["ok"].get<bool>());
}
} // namespace
