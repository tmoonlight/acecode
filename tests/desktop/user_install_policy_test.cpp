#include "desktop/user_install_policy.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

namespace fs = std::filesystem;

TEST(UserInstallPolicy, UsesCurrentUsersApplicationsDirectory) {
    const auto paths = acecode::desktop::macos_user_install_paths(
        fs::path("/Users/alice"));

    EXPECT_EQ(paths.home, fs::path("/Users/alice"));
    EXPECT_EQ(paths.applications, fs::path("/Users/alice/Applications"));
    EXPECT_EQ(paths.destination,
              fs::path("/Users/alice/Applications/ACECode.app"));
    EXPECT_TRUE(acecode::desktop::macos_user_install_destination_is_safe(
        paths.home, paths.applications, paths.destination));
}

TEST(UserInstallPolicy, UsesStandardSystemApplicationsDirectory) {
    const auto paths = acecode::desktop::macos_system_install_paths();

    EXPECT_EQ(paths.applications, fs::path("/Applications"));
    EXPECT_EQ(paths.destination, fs::path("/Applications/ACECode.app"));
    EXPECT_EQ(acecode::desktop::macos_self_update_install_location(
                  "/Users/alice", paths.applications, paths.destination),
              acecode::desktop::MacosInstallLocation::system_applications);
}

TEST(UserInstallPolicy, NormalizesDotSegmentsWithinExpectedLayout) {
    EXPECT_TRUE(acecode::desktop::macos_user_install_destination_is_safe(
        fs::path("/Users/alice/./"),
        fs::path("/Users/alice/tmp/../Applications"),
        fs::path("/Users/alice/Applications/./ACECode.app")));
}

TEST(UserInstallPolicy, ClassifiesLegacyPerUserDestination) {
    EXPECT_EQ(acecode::desktop::macos_self_update_install_location(
                  "/Users/alice", "/Users/alice/Applications",
                  "/Users/alice/Applications/ACECode.app"),
              acecode::desktop::MacosInstallLocation::user_applications);
}

TEST(UserInstallPolicy, RejectsRelativeAndEmptyHomeDirectories) {
    EXPECT_TRUE(acecode::desktop::macos_user_install_paths({}).home.empty());
    EXPECT_TRUE(acecode::desktop::macos_user_install_paths("Users/alice").home.empty());
    EXPECT_FALSE(acecode::desktop::macos_user_install_destination_is_safe(
        "Users/alice", "Users/alice/Applications",
        "Users/alice/Applications/ACECode.app"));
}

TEST(UserInstallPolicy, KeepsSystemDestinationOutOfPerUserPolicy) {
    EXPECT_FALSE(acecode::desktop::macos_user_install_destination_is_safe(
        "/Users/alice", "/Applications", "/Applications/ACECode.app"));
}

TEST(UserInstallPolicy, RejectsRedirectedApplicationsDirectory) {
    EXPECT_FALSE(acecode::desktop::macos_user_install_destination_is_safe(
        "/Users/alice", "/Users/alice/AlternateApps",
        "/Users/alice/AlternateApps/ACECode.app"));
    EXPECT_FALSE(acecode::desktop::macos_user_install_destination_is_safe(
        "/Users/alice", "/Volumes/External/Applications",
        "/Volumes/External/Applications/ACECode.app"));
}

TEST(UserInstallPolicy, RejectsUnexpectedDestinationNamesAndLocations) {
    EXPECT_FALSE(acecode::desktop::macos_user_install_destination_is_safe(
        "/Users/alice", "/Users/alice/Applications",
        "/Users/alice/Applications/Other.app"));
    EXPECT_FALSE(acecode::desktop::macos_user_install_destination_is_safe(
        "/Users/alice", "/Users/alice/Applications",
        "/Users/alice/ACECode.app"));
}

TEST(UserInstallPolicy, SelfUpdateRejectsOtherNamesAndApplicationsDirectories) {
    EXPECT_EQ(acecode::desktop::macos_self_update_install_location(
                  "/Users/alice", "/Applications", "/Applications/Other.app"),
              acecode::desktop::MacosInstallLocation::unsupported);
    EXPECT_EQ(acecode::desktop::macos_self_update_install_location(
                  "/Users/alice", "/Volumes/External/Applications",
                  "/Volumes/External/Applications/ACECode.app"),
              acecode::desktop::MacosInstallLocation::unsupported);
    EXPECT_EQ(acecode::desktop::macos_self_update_install_location(
                  "/Users/alice", "/Users/bob/Applications",
                  "/Users/bob/Applications/ACECode.app"),
              acecode::desktop::MacosInstallLocation::unsupported);
}

} // namespace
