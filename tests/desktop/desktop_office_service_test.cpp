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
    void open(const std::string& version = "0.9.37", bool available = true) {
        service.reset(); host.bindings.clear();
        service = std::make_shared<Service>(host, dir / "office.json", version, available,
                                           [] { return "<html>offline preview</html>"; });
        service->bind_bridge();
    }
};

TEST_F(DesktopOfficeServiceTest, DefaultsOffAndVersionGateIncludesLaterVersions) {
    for (const auto* version : {"0.9.35", "0.9.36", "0.9.37-pre.1", "invalid"}) {
        open(version);
        EXPECT_FALSE(service->state()["welcomePending"].get<bool>());
        EXPECT_FALSE(service->state()["enabled"].get<bool>());
    }
    for (const auto* version : {"0.9.37", "0.9.38", "1.0.0"}) {
        open(version);
        EXPECT_TRUE(service->state()["welcomePending"].get<bool>());
    }
    EXPECT_EQ(FakeWindow::starts, 0);
}

TEST_F(DesktopOfficeServiceTest, WelcomeIsClaimedOnceAcrossRestartsWithoutEnabling) {
    open();
    EXPECT_TRUE(host.call("aceDesktop_claimOfficeWelcome")["show"].get<bool>());
    EXPECT_FALSE(host.call("aceDesktop_claimOfficeWelcome")["show"].get<bool>());
    open("0.10.0");
    EXPECT_FALSE(host.call("aceDesktop_claimOfficeWelcome")["show"].get<bool>());
    EXPECT_FALSE(service->state()["enabled"].get<bool>());
}

// 场景:升级上来的老用户(office.json 已记过 welcome_seen)第一次启动带「更多宠物」宣传的版本。
// 期望:宣传待展示,认领一次就不再出现(重启后也不出现),认领不改变已开启的宠物与当前宠物。
TEST_F(DesktopOfficeServiceTest, PetsPromoIsClaimedOnceByUsersWhoAnsweredTheInvitation) {
    std::filesystem::create_directories(dir);
    { std::ofstream file(dir / "office.json"); file << R"({"enabled":true,"welcome_seen":true,"pet_kind":"office"})"; }
    open("1.0.8");
    EXPECT_FALSE(service->state()["welcomePending"].get<bool>());
    EXPECT_TRUE(service->state()["petsPromoPending"].get<bool>());
    const Json first = host.call("aceDesktop_claimPetsPromo");
    EXPECT_TRUE(first["show"].get<bool>());
    EXPECT_FALSE(first["petsPromoPending"].get<bool>());
    EXPECT_FALSE(host.call("aceDesktop_claimPetsPromo")["show"].get<bool>());
    open("1.0.8");
    EXPECT_FALSE(host.call("aceDesktop_claimPetsPromo")["show"].get<bool>());
    EXPECT_TRUE(service->state()["enabled"].get<bool>());
    EXPECT_EQ(service->state()["petKind"], "office");
}

// 场景:全新用户。期望:只弹虚拟办公室的开启邀请;认领邀请时一并消耗「更多宠物」宣传,
// 下次启动不会再弹第二个窗。没看过邀请就在设置里开 / 关宠物,同样视为已经见过所有宠物;
// 老用户在设置里开关宠物则不影响宣传。
TEST_F(DesktopOfficeServiceTest, FirstTimeUsersOnlyGetTheInvitation) {
    open("1.0.8");
    EXPECT_TRUE(service->state()["welcomePending"].get<bool>());
    EXPECT_FALSE(service->state()["petsPromoPending"].get<bool>());
    EXPECT_TRUE(host.call("aceDesktop_claimOfficeWelcome")["show"].get<bool>());
    EXPECT_FALSE(service->state()["petsPromoPending"].get<bool>());
    open("1.0.8");
    EXPECT_FALSE(host.call("aceDesktop_claimPetsPromo")["show"].get<bool>());

    std::filesystem::remove_all(dir);
    open("1.0.8");
    ASSERT_TRUE(service->set_enabled(true)["ok"].get<bool>());
    EXPECT_FALSE(service->state()["petsPromoPending"].get<bool>());

    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    { std::ofstream file(dir / "office.json"); file << R"({"enabled":true,"welcome_seen":true})"; }
    open("1.0.8");
    ASSERT_TRUE(service->set_enabled(false)["ok"].get<bool>());
    EXPECT_TRUE(service->state()["petsPromoPending"].get<bool>());
    open("0.9.37", false);
    EXPECT_FALSE(service->state()["petsPromoPending"].get<bool>());
}

TEST_F(DesktopOfficeServiceTest, EnablingPersistsAndClosingReopensWithLatestSnapshot) {
    open();
    ASSERT_TRUE(service->set_enabled(true)["ok"].get<bool>());
    EXPECT_FALSE(service->state()["welcomePending"].get<bool>());
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
    EXPECT_FALSE(host.call("aceDesktop_claimOfficeWelcome")["ok"].get<bool>());
    EXPECT_TRUE(service->state()["welcomePending"].get<bool>());
    std::filesystem::remove(dir / "office.json.tmp");
    FakeWindow::fail = true;
    EXPECT_FALSE(service->set_enabled(true)["ok"].get<bool>());
    EXPECT_FALSE(service->state()["enabled"].get<bool>());
    EXPECT_GT(FakeWindow::closes, 0);
}

TEST_F(DesktopOfficeServiceTest, DisabledEnvironmentAndInvalidMessagesAreRejected) {
    open("0.9.37", false);
    EXPECT_FALSE(service->state()["welcomePending"].get<bool>());
    EXPECT_FALSE(service->set_enabled(true)["ok"].get<bool>());
    EXPECT_EQ(host.call("aceDesktop_getOfficePreview"), nullptr);
    open(); service->set_enabled(true);
    EXPECT_FALSE(host.call("aceDesktop_setOfficeEnabled", Json::array({"true"}))["ok"].get<bool>());
    EXPECT_EQ(host.call("aceDesktop_updateOffice", Json::array({"{}"})), false);
    EXPECT_EQ(host.call("aceDesktop_updateOffice", Json::array({"not json"})), false);
    EXPECT_EQ(host.call("aceDesktop_getOfficePreview"), "<html>offline preview</html>");
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
    open("1.0.0", false);
    EXPECT_FALSE(service->set_pet_kind("clippy")["ok"].get<bool>());
}
} // namespace
