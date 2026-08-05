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

TEST(UserInstallPolicy, NormalizesDotSegmentsWithinExpectedLayout) {
    EXPECT_TRUE(acecode::desktop::macos_user_install_destination_is_safe(
        fs::path("/Users/alice/./"),
        fs::path("/Users/alice/tmp/../Applications"),
        fs::path("/Users/alice/Applications/./ACECode.app")));
}

TEST(UserInstallPolicy, RejectsRelativeAndEmptyHomeDirectories) {
    EXPECT_TRUE(acecode::desktop::macos_user_install_paths({}).home.empty());
    EXPECT_TRUE(acecode::desktop::macos_user_install_paths("Users/alice").home.empty());
    EXPECT_FALSE(acecode::desktop::macos_user_install_destination_is_safe(
        "Users/alice", "Users/alice/Applications",
        "Users/alice/Applications/ACECode.app"));
}

TEST(UserInstallPolicy, RejectsSystemWideApplicationsDirectory) {
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

} // namespace
