// 覆盖 src/base/platform/install_record:安装程序在安装目录里留下的 .acecode-install.json。
//   1. 序列化后再解析字段不丢;schema 不认识、缺版本、不是 JSON 时拒绝。
//   2. 写入安装记录后能读回,临时文件不残留。
//   3. 目录比较忽略大小写、斜杠方向、末尾分隔符与引号。
//   4. 自升级刷新版本:有记录的目录把版本改成新版本;便携目录(没有记录)保持空操作。

#include "platform/install_record.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

namespace record = acecode::install_record;
namespace fs = std::filesystem;

namespace {

fs::path fresh_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() / ("acecode-install-record-" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

}  // namespace

// 场景:安装 1.0.20 时写下的记录(加了 PATH、没建桌面快捷方式)。
// 期望:解析回来的每个字段都与写入时相同。
TEST(InstallRecord, RoundTripsAllFields) {
    record::InstallRecord in;
    in.version = "1.0.20";
    in.target = "windows-x64";
    in.add_to_path = true;
    in.desktop_shortcut = false;
    in.installed_at = "2026-11-05T01:02:03Z";
    std::string error;
    const auto out = record::parse(record::serialize(in), &error);
    ASSERT_TRUE(out.has_value()) << error;
    EXPECT_EQ(out->version, in.version);
    EXPECT_EQ(out->target, in.target);
    EXPECT_TRUE(out->add_to_path);
    EXPECT_FALSE(out->desktop_shortcut);
    EXPECT_EQ(out->installed_at, in.installed_at);
}

// 场景:未来版本写的 schema 2、缺 version 的记录、损坏的文件。
// 期望:全部拒绝,卸载程序据此判定「不是本安装程序装的」,不去删目录。
TEST(InstallRecord, RejectsUnknownOrIncompleteRecords) {
    EXPECT_FALSE(record::parse(R"({"schema": 2, "version": "1.0.20"})").has_value());
    EXPECT_FALSE(record::parse(R"({"schema": 1})").has_value());
    EXPECT_FALSE(record::parse("{broken").has_value());
    EXPECT_FALSE(record::parse("[]").has_value());
}

// 场景:往临时安装目录写记录再读回;另一个空目录直接读。
// 期望:前者读回版本一致且 .tmp 文件不残留;后者返回空(便携目录没有记录)。
TEST(InstallRecord, WritesAndReadsMarker) {
    const fs::path dir = fresh_dir();
    record::InstallRecord in;
    in.version = "1.0.19";
    std::string error;
    ASSERT_TRUE(record::write_marker(dir, in, &error)) << error;
    const auto out = record::read_marker(dir);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->version, "1.0.19");
    EXPECT_FALSE(fs::exists(dir / (std::string(record::kMarkerFileName) + ".tmp")));
    const fs::path empty = dir / "portable";
    fs::create_directories(empty);
    EXPECT_FALSE(record::read_marker(empty).has_value());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// 场景:注册表 InstallLocation 常见的几种写法(Inno 会带末尾反斜杠,手改的可能带引号或正斜杠)。
// 期望:都认作同一个目录;不同目录或空串不相等。
TEST(InstallRecord, ComparesInstallDirectoriesLoosely) {
    EXPECT_TRUE(record::same_install_dir("C:\\Users\\A\\AppData\\Local\\Programs\\ACECode\\",
                                         "c:/users/a/appdata/local/programs/acecode"));
    EXPECT_TRUE(record::same_install_dir("\"D:\\Apps\\ACECode\"", "D:\\Apps\\ACECode"));
    EXPECT_FALSE(record::same_install_dir("D:\\Apps\\ACECode", "D:\\Apps\\ACECode2"));
    EXPECT_FALSE(record::same_install_dir("", ""));
}

// 场景:自升级把装在有记录目录里的 1.0.19 换成 1.0.20;另一个便携目录同样升级。
// 期望:前者记录里的版本变成 1.0.20 并返回 true;后者返回 false 且不生成记录文件
// (修复前「设置 > 应用」与安装程序都会一直以为装的是旧版本)。
TEST(InstallRecord, RefreshUpdatesOnlyRecordedInstalls) {
    const fs::path dir = fresh_dir();
    record::InstallRecord in;
    in.version = "1.0.19";
    ASSERT_TRUE(record::write_marker(dir, in));
    EXPECT_TRUE(record::refresh_installed_version(dir, "1.0.20"));
    EXPECT_EQ(record::read_marker(dir)->version, "1.0.20");

    const fs::path portable = dir / "portable";
    fs::create_directories(portable);
    EXPECT_FALSE(record::refresh_installed_version(portable, "1.0.20"));
    EXPECT_FALSE(fs::exists(portable / record::kMarkerFileName));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
