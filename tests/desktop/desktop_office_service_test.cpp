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
    Json snapshot;
    static inline int starts = 0;
    static inline int closes = 0;
    static inline bool fail = false;
    static inline FakeWindow* current = nullptr; // Test-local borrow, never used after teardown.
    bool start() { starts++; current = this; return !fail; }
    void close() { closes++; if (on_closed) on_closed(); }
    void update_snapshot(const Json& value) { snapshot = value; }
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
} // namespace
