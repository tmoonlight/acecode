#include <gtest/gtest.h>

#include "tool/ios_simulator/xcode_locator.hpp"

#include <filesystem>
#include <fstream>

using namespace acecode::ios_simulator;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

// A minimal fake Xcode.app: the layout inspect_xcode validates.
fs::path fake_xcode(const fs::path& root, const std::string& name, const std::string& version,
                    bool shared_kit = false) {
    const auto app = root / name;
    write(app / "Contents/Developer/usr/bin/simctl", "#!/bin/sh\n");
    write(app / "Contents/version.plist",
          "<plist><dict><key>CFBundleShortVersionString</key>\n  <string>" + version +
          "</string><key>ProductBuildVersion</key><string>17F113</string></dict></plist>");
    fs::create_directories(shared_kit ? app / "Contents/SharedFrameworks/SimulatorKit.framework"
                                      : app / "Contents/Developer/Library/PrivateFrameworks/SimulatorKit.framework");
    return app;
}

class XcodeLocatorTest : public testing::Test {
protected:
    void SetUp() override {
        root = fs::temp_directory_path() / ("acecode-xcode-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        fs::remove_all(root);
    }
    void TearDown() override { fs::remove_all(root); }
    fs::path root;
};

} // namespace

TEST(XcodeLocatorPure, ReadsPlistStringsOnlyForTheMatchingKey) {
    const std::string xml = "<dict><key>A</key><string>1</string><key>B</key>\n\t<string>2.5</string></dict>";
    EXPECT_EQ(plist_string_value(xml, "A"), "1");
    EXPECT_EQ(plist_string_value(xml, "B"), "2.5");
    EXPECT_EQ(plist_string_value(xml, "C"), "");
    EXPECT_EQ(plist_string_value("<key>A</key><integer>3</integer><string>x</string>", "A"), "");
}

TEST(XcodeLocatorPure, ComparesDottedVersionsNumerically) {
    EXPECT_EQ(compare_versions("26.6", "26.6"), 0);
    EXPECT_LT(compare_versions("26.6", "26.10"), 0);
    EXPECT_GT(compare_versions("27.0", "26.6"), 0);
    EXPECT_EQ(compare_versions("26", "26.0"), 0);
    EXPECT_LT(compare_versions("1155.4", "1155.10"), 0);
}

TEST(XcodeLocatorPure, MapsAppAndDeveloperPaths) {
    auto paths = xcode_paths_for("/Users/me/Downloads/Xcode.app/");
    EXPECT_EQ(paths.first, "/Users/me/Downloads/Xcode.app");
    EXPECT_EQ(paths.second, "/Users/me/Downloads/Xcode.app/Contents/Developer");
    paths = xcode_paths_for("/Applications/Xcode-beta.app/Contents/Developer");
    EXPECT_EQ(paths.first, "/Applications/Xcode-beta.app");
    EXPECT_TRUE(xcode_paths_for("/Library/Developer/CommandLineTools").first.empty());
}

TEST_F(XcodeLocatorTest, InspectsGreenInstallsAndRejectsIncompleteCopies) {
    const auto app = fake_xcode(root, "Xcode.app", "26.6");
    auto install = inspect_xcode(app.string());
    ASSERT_TRUE(install.has_value());
    EXPECT_EQ(install->version, "26.6");
    EXPECT_EQ(install->build, "17F113");
    EXPECT_TRUE(install->simulator_kit);
    EXPECT_TRUE(inspect_xcode((app / "Contents/Developer").generic_string()).has_value());

    fs::create_directories(root / "Broken.app/Contents");
    std::string why;
    EXPECT_FALSE(inspect_xcode((root / "Broken.app").string(), &why).has_value());
    EXPECT_NE(why.find("simctl"), std::string::npos);
    EXPECT_FALSE(inspect_xcode((root / "missing.app").string(), &why).has_value());
    EXPECT_FALSE(inspect_xcode("/Library/Developer/CommandLineTools", &why).has_value());
}

TEST_F(XcodeLocatorTest, ChoosesConfiguredThenSelectThenNewest) {
    auto old_install = *inspect_xcode(fake_xcode(root, "Xcode-26.app", "26.6").string());
    auto new_install = *inspect_xcode(fake_xcode(root, "Xcode-27.app", "27.0", true).string());
    old_install.sources = {"applications"};
    new_install.sources = {"spotlight"};
    auto newest = choose_xcode("", "", {old_install, new_install});
    ASSERT_TRUE(newest.selected.has_value());
    EXPECT_EQ(newest.selected->version, "27.0");
    EXPECT_EQ(newest.selected_source, "newest");

    auto selected = old_install;
    selected.sources = {"xcode-select"};
    auto by_select = choose_xcode("", "", {new_install, selected});
    EXPECT_EQ(by_select.selected->version, "26.6");
    EXPECT_EQ(by_select.selected_source, "xcode-select");

    auto configured = old_install;
    configured.sources = {"configured"};
    auto merged = choose_xcode(old_install.app_path, "", {new_install, configured, old_install});
    EXPECT_EQ(merged.selected->version, "26.6");
    EXPECT_EQ(merged.selected_source, "configured");
    ASSERT_EQ(merged.installs.size(), 2u);  // duplicates merged
    EXPECT_EQ(merged.installs.front().sources.size(), 2u);

    auto none = choose_xcode("/nope", "The path does not exist.", {});
    EXPECT_FALSE(none.selected.has_value());
    EXPECT_FALSE(none.error.empty());
    EXPECT_EQ(none.configured_error, "The path does not exist.");
}

TEST_F(XcodeLocatorTest, ResolveHonoursAConfiguredGreenInstall) {
    const auto app = fake_xcode(root, "Xcode.app", "26.6");
    DiscoveryOptions options;
    options.spotlight = false;
    auto resolved = resolve_xcode(app.string(), options);
    ASSERT_TRUE(resolved.selected.has_value());
    EXPECT_EQ(resolved.selected->app_path, app.string());
    EXPECT_EQ(resolved.selected_source, "configured");
    auto json = to_json(resolved);
    EXPECT_EQ(json["selected"]["version"], "26.6");

    auto bad = resolve_xcode((root / "missing.app").string(), options);
    EXPECT_FALSE(bad.configured_error.empty());
}

#ifdef __APPLE__
TEST_F(XcodeLocatorTest, ReadsBinaryPropertyLists) {
    const auto app = fake_xcode(root, "Xcode.app", "27.1");
    const auto plist = app / "Contents/version.plist";
    ASSERT_EQ(std::system(("/usr/bin/plutil -convert binary1 '" + plist.string() + "'").c_str()), 0);
    auto install = inspect_xcode(app.string());
    ASSERT_TRUE(install.has_value());
    EXPECT_EQ(install->version, "27.1");
    EXPECT_EQ(install->build, "17F113");
}
#endif
