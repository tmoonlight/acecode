#include "upgrade/manifest.hpp"
#include "utils/sha256.hpp"

#include <gtest/gtest.h>

using namespace acecode::upgrade;

TEST(UpgradeVersion, ParsesAndComparesSemanticVersions) {
    auto a = parse_sem_version("0.1.9");
    auto b = parse_sem_version("v0.1.10");
    auto rc = parse_sem_version("1.0.0-rc.1");
    auto final_v = parse_sem_version("1.0.0");

    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(rc.has_value());
    ASSERT_TRUE(final_v.has_value());
    EXPECT_LT(compare_sem_version(*a, *b), 0);
    EXPECT_LT(compare_sem_version(*rc, *final_v), 0);
    EXPECT_FALSE(parse_sem_version("0.1").has_value());
}

TEST(UpgradeManifest, UsesCapabilityVersionedTargetsOnlyForSupportedLinuxBuilds) {
    EXPECT_EQ(manifest_target_for_platform("linux-x64"),
              "linux-x64-updater-v1");
    EXPECT_EQ(manifest_target_for_platform("linux-arm64"),
              "linux-arm64-updater-v1");
    EXPECT_EQ(manifest_target_for_platform("linux-unknown"),
              "linux-unknown");
    EXPECT_EQ(manifest_target_for_platform("windows-x64"), "windows-x64");
    EXPECT_EQ(manifest_target_for_platform("macos-arm64"), "macos-arm64");
    EXPECT_EQ(manifest_target_for_platform("linux-deepin-x64"), "linux-deepin-x64");
    EXPECT_EQ(manifest_target_for_platform("linux-deepin-arm64"), "linux-deepin-arm64");
    EXPECT_EQ(manifest_target_for_platform("linux-deepin-armv7"), "linux-deepin-armv7");
}

TEST(UpgradeManifest, DeepinCannotInstallGenericLinuxPackages) {
    UpdateManifest manifest;
    manifest.schema_version = 1;
    manifest.latest = "1.2.4";
    ReleaseInfo release;
    release.version = "1.2.4";
    for (const auto* target : {"linux-x64-updater-v1", "linux-arm64-updater-v1"}) {
        PackageInfo package;
        package.target = target;
        package.file = std::string(target) + ".zip";
        package.sha256 = acecode::sha256_hex("package");
        release.packages.push_back(package);
    }
    manifest.releases.push_back(release);
    for (const auto* target : {"linux-deepin-x64", "linux-deepin-arm64", "linux-deepin-armv7"}) {
        const auto selected = select_update_package(
            manifest, "1.2.3", manifest_target_for_platform(target));
        EXPECT_EQ(selected.status, SelectionStatus::NoCompatiblePackage);
        EXPECT_FALSE(selected.selected.has_value());
    }
    EXPECT_EQ(select_update_package(manifest, "1.2.3", "linux-x64-updater-v1").status,
              SelectionStatus::UpdateAvailable);
}

TEST(UpgradeManifest, SelectsNewestCompatiblePackage) {
    std::string sha = acecode::sha256_hex("package");
    std::string text = R"({
      "schema_version": 1,
      "latest": "0.1.4",
      "releases": [
        {"version": "0.1.4", "packages": [
          {"target": "linux-x64", "file": "acecode-0.1.4-linux-x64.zip", "sha256": ")" + sha + R"("}
        ]},
        {"version": "0.1.3", "packages": [
          {"target": "windows-x64", "file": "acecode-0.1.3-windows-x64.zip", "sha256": ")" + sha + R"("}
        ]},
        {"version": "not-a-version", "packages": [
          {"target": "windows-x64", "file": "ignored.zip", "sha256": ")" + sha + R"("}
        ]}
      ]
    })";

    std::string err;
    auto manifest = parse_update_manifest(text, &err);
    ASSERT_TRUE(manifest.has_value()) << err;
    auto selected = select_update_package(*manifest, "0.1.2", "windows-x64");

    EXPECT_EQ(selected.status, SelectionStatus::UpdateAvailable);
    ASSERT_TRUE(selected.selected.has_value());
    EXPECT_EQ(selected.selected->version, "0.1.3");
    EXPECT_EQ(selected.selected->package.file, "acecode-0.1.3-windows-x64.zip");
}

TEST(UpgradeManifest, ParsesReleaseNotesAndAllowsLegacyMissingNotes) {
    std::string text = R"({
      "schema_version": 1,
      "latest": "0.2.0",
      "releases": [
        {
          "version": "0.2.0",
          "published_at": "2026-07-20T08:00:00Z",
          "notes": "New update history.\nStability fixes.",
          "packages": []
        },
        {
          "version": "0.1.0",
          "packages": []
        }
      ]
    })";

    std::string err;
    auto manifest = parse_update_manifest(text, &err);

    ASSERT_TRUE(manifest.has_value()) << err;
    ASSERT_EQ(manifest->releases.size(), 2u);
    EXPECT_EQ(manifest->releases[0].version, "0.2.0");
    EXPECT_EQ(manifest->releases[0].published_at, "2026-07-20T08:00:00Z");
    EXPECT_EQ(manifest->releases[0].notes,
              "New update history.\nStability fixes.");
    EXPECT_TRUE(manifest->releases[1].published_at.empty());
    EXPECT_TRUE(manifest->releases[1].notes.empty());
}

TEST(UpgradeManifest, ReportsUpToDateWhenNoNewerCompatiblePackageExists) {
    std::string sha = acecode::sha256_hex("package");
    std::string text = R"({
      "schema_version": 1,
      "latest": "0.1.2",
      "releases": [
        {"version": "0.1.2", "packages": [
          {"target": "windows-x64", "file": "acecode-0.1.2-windows-x64.zip", "sha256": ")" + sha + R"("}
        ]}
      ]
    })";

    std::string err;
    auto manifest = parse_update_manifest(text, &err);
    ASSERT_TRUE(manifest.has_value()) << err;
    auto selected = select_update_package(*manifest, "0.1.2", "windows-x64");
    EXPECT_EQ(selected.status, SelectionStatus::UpToDate);
    EXPECT_TRUE(selected.latest_version.empty());
}

TEST(UpgradeManifest, ReportsNoCompatiblePackageWhenNewerReleasesExist) {
    std::string sha = acecode::sha256_hex("package");
    std::string text = R"({
      "schema_version": 1,
      "latest": "0.1.4",
      "releases": [
        {"version": "0.1.4", "packages": [
          {"target": "windows-x64", "file": "acecode-0.1.4-windows-x64.zip", "sha256": ")" + sha + R"("}
        ]},
        {"version": "not-semver", "packages": []}
      ]
    })";

    std::string err;
    auto manifest = parse_update_manifest(text, &err);
    ASSERT_TRUE(manifest.has_value()) << err;
    auto selected = select_update_package(
        *manifest, "0.1.2", "linux-arm64-updater-v1");

    EXPECT_EQ(selected.status, SelectionStatus::NoCompatiblePackage);
    EXPECT_FALSE(selected.selected.has_value());
    EXPECT_EQ(selected.latest_version, "0.1.4");
}

TEST(UpgradeManifest, ForceSelectsNewestCompatiblePackageIgnoringCurrentVersion) {
    std::string sha = acecode::sha256_hex("package");
    std::string text = R"({
      "schema_version": 1,
      "latest": "0.1.4",
      "releases": [
        {"version": "0.1.4", "packages": [
          {"target": "windows-x64", "file": "acecode-0.1.4-windows-x64.zip", "sha256": ")" + sha + R"("}
        ]},
        {"version": "0.1.3", "packages": [
          {"target": "windows-x64", "file": "acecode-0.1.3-windows-x64.zip", "sha256": ")" + sha + R"("}
        ]}
      ]
    })";

    std::string err;
    auto manifest = parse_update_manifest(text, &err);
    ASSERT_TRUE(manifest.has_value()) << err;
    auto selected = select_update_package(*manifest, "not-semver", "windows-x64", true);

    EXPECT_EQ(selected.status, SelectionStatus::UpdateAvailable);
    ASSERT_TRUE(selected.selected.has_value());
    EXPECT_EQ(selected.selected->version, "0.1.4");
    EXPECT_EQ(selected.selected->package.file, "acecode-0.1.4-windows-x64.zip");
}

TEST(UpgradeManifest, ForceReportsNoCompatiblePackageForMissingTarget) {
    std::string sha = acecode::sha256_hex("package");
    std::string text = R"({
      "schema_version": 1,
      "latest": "0.1.4",
      "releases": [
        {"version": "0.1.4", "packages": [
          {"target": "windows-x64", "file": "acecode-0.1.4-windows-x64.zip", "sha256": ")" + sha + R"("}
        ]}
      ]
    })";

    std::string err;
    auto manifest = parse_update_manifest(text, &err);
    ASSERT_TRUE(manifest.has_value()) << err;
    auto selected = select_update_package(
        *manifest, "not-semver", "linux-x64-updater-v1", true);

    EXPECT_EQ(selected.status, SelectionStatus::NoCompatiblePackage);
    EXPECT_FALSE(selected.selected.has_value());
    EXPECT_EQ(selected.latest_version, "0.1.4");
}

TEST(UpgradeManifest, RejectsInvalidManifestAndUnsafePackage) {
    std::string err;
    EXPECT_FALSE(parse_update_manifest(R"({"schema_version": 2, "latest": "1.0.0", "releases": []})", &err));
    EXPECT_NE(err.find("unsupported"), std::string::npos);

    std::string sha = acecode::sha256_hex("package");
    std::string text = R"({
      "schema_version": 1,
      "latest": "0.1.3",
      "releases": [
        {"version": "0.1.3", "packages": [
          {"target": "windows-x64", "file": "../bad.zip", "sha256": ")" + sha + R"("}
        ]}
      ]
    })";
    auto manifest = parse_update_manifest(text, &err);
    ASSERT_TRUE(manifest.has_value()) << err;
    auto selected = select_update_package(*manifest, "0.1.2", "windows-x64");
    EXPECT_EQ(selected.status, SelectionStatus::InvalidManifest);
    EXPECT_NE(selected.error.find("unsafe"), std::string::npos);
}

namespace {

ReleaseInfo rollback_release(const std::string& version,
                             const std::string& published_at,
                             const std::string& target = "windows-x64",
                             const std::string& sha256 = acecode::sha256_hex("package")) {
    ReleaseInfo release;
    release.version = version;
    release.published_at = published_at;
    PackageInfo package;
    package.target = target;
    package.file = "acecode-" + version + "-" + target + ".zip";
    package.sha256 = sha256;
    package.size = 42;
    release.packages.push_back(package);
    return release;
}

} // namespace

// 场景:升级弹窗给每个历史版本判断能不能「回滚到此版本」。
// 期望:只有 2026-11-01(含当天,按 UTC 日期)及之后发布、版本号低于当前运行版本、
//      并且为本平台发布了合法包(文件名安全、sha256 为 64 位小写十六进制)的版本可回滚。
//      缺发布日期、晚于等于当前版本、只有别的平台的包、sha256 不合法都不行。
TEST(UpgradeManifest, RollbackEligibilityRequiresDateOlderVersionAndPackage) {
    EXPECT_TRUE(rollback_package_for_release(
        rollback_release("1.0.18", "2026-11-01T13:15:43Z"), "1.0.34", "windows-x64"));
    // 10-31 当天(UTC)发布的版本在截止日之前。
    EXPECT_FALSE(rollback_package_for_release(
        rollback_release("1.0.17", "2026-10-31T23:59:59Z"), "1.0.34", "windows-x64"));
    EXPECT_FALSE(rollback_package_for_release(
        rollback_release("1.0.30", ""), "1.0.34", "windows-x64"));
    EXPECT_FALSE(rollback_package_for_release(
        rollback_release("1.0.34", "2026-11-22T16:33:51Z"), "1.0.34", "windows-x64"));
    EXPECT_FALSE(rollback_package_for_release(
        rollback_release("1.0.35", "2026-11-30T00:00:00Z"), "1.0.34", "windows-x64"));
    // 预发布版按语义版本比较低于正式版,只要本平台有包就可以回滚。
    EXPECT_TRUE(rollback_package_for_release(
        rollback_release("1.0.34-pre.1", "2026-11-22T05:33:41Z"), "1.0.34", "windows-x64"));
    EXPECT_FALSE(rollback_package_for_release(
        rollback_release("1.0.34-pre.1", "2026-11-22T05:33:41Z"), "1.0.34", "macos-arm64"));
    EXPECT_FALSE(rollback_package_for_release(
        rollback_release("1.0.29", "2026-11-15T09:59:32Z", "windows-x64", "ABC"),
        "1.0.34", "windows-x64"));
}

// 场景:点了「回滚到此版本」之后,执行升级任务时按版本号精确挑包。
// 期望:带 v 前缀的版本号也能对上清单原文;选中的就是那个版本的本平台包;
//      清单里没有、或者不满足回滚资格的版本返回 NoCompatiblePackage 并带原因。
TEST(UpgradeManifest, SelectsExactRollbackPackage) {
    UpdateManifest manifest;
    manifest.schema_version = 1;
    manifest.latest = "1.0.34";
    manifest.releases.push_back(rollback_release("1.0.34", "2026-11-22T16:33:51Z"));
    manifest.releases.push_back(rollback_release("1.0.33", "2026-11-19T04:50:55Z"));
    manifest.releases.push_back(rollback_release("1.0.17", "2026-10-25T18:35:16Z"));

    const auto selected = select_rollback_package(manifest, "1.0.34", "v1.0.33", "windows-x64");
    EXPECT_EQ(selected.status, SelectionStatus::UpdateAvailable);
    ASSERT_TRUE(selected.selected.has_value());
    EXPECT_EQ(selected.selected->version, "1.0.33");
    EXPECT_EQ(selected.selected->package.file, "acecode-1.0.33-windows-x64.zip");

    const auto too_old = select_rollback_package(manifest, "1.0.34", "1.0.17", "windows-x64");
    EXPECT_EQ(too_old.status, SelectionStatus::NoCompatiblePackage);
    EXPECT_FALSE(too_old.selected.has_value());
    EXPECT_NE(too_old.error.find("2026-11-01"), std::string::npos) << too_old.error;

    const auto missing = select_rollback_package(manifest, "1.0.34", "1.0.20", "windows-x64");
    EXPECT_EQ(missing.status, SelectionStatus::NoCompatiblePackage);
    const auto current = select_rollback_package(manifest, "1.0.34", "1.0.34", "windows-x64");
    EXPECT_EQ(current.status, SelectionStatus::NoCompatiblePackage);
    const auto garbage = select_rollback_package(manifest, "1.0.34", "latest", "windows-x64");
    EXPECT_EQ(garbage.status, SelectionStatus::NoCompatiblePackage);
}

TEST(UpgradeManifest, ResolvesPackageUnderBaseUrl) {
    EXPECT_TRUE(is_safe_package_file("releases/acecode-0.1.3-windows-x64.zip"));
    EXPECT_FALSE(is_safe_package_file("https://evil.test/a.zip"));
    EXPECT_FALSE(is_safe_package_file("..%2Fencoded-traversal.zip"));
    EXPECT_FALSE(is_safe_package_file("../bad.zip"));
    EXPECT_EQ(manifest_url("https://updates.example.test/ace"),
              "https://updates.example.test/ace/aceupdate.json");
    EXPECT_EQ(resolve_package_url("https://updates.example.test/ace", "pkg.zip"),
              "https://updates.example.test/ace/pkg.zip");
}
