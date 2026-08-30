#include "upgrade/package.hpp"

#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <chrono>
#include <zip.h>

namespace fs = std::filesystem;

using namespace acecode::upgrade;

namespace {

fs::path temp_root(const std::string& name) {
    auto p = fs::temp_directory_path() /
             (name + "-" + std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << body;
}

bool create_zip_entry(const fs::path& archive_path,
                      const std::string& entry_name,
                      const std::string& body,
                      zip_uint32_t unix_mode) {
    int error = 0;
    zip_t* archive = zip_open(acecode::path_to_utf8(archive_path).c_str(),
                              ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!archive) return false;
    zip_source_t* source = zip_source_buffer(
        archive, body.data(), body.size(), 0);
    if (!source) {
        zip_discard(archive);
        return false;
    }
    const zip_int64_t index = zip_file_add(
        archive, entry_name.c_str(), source, ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_source_free(source);
        zip_discard(archive);
        return false;
    }
    if (zip_file_set_external_attributes(
            archive, static_cast<zip_uint64_t>(index), 0,
            ZIP_OPSYS_UNIX, unix_mode << 16u) != 0) {
        zip_discard(archive);
        return false;
    }
    return zip_close(archive) == 0;
}

} // namespace

TEST(UpgradePackage, ValidatesZipEntryPaths) {
    EXPECT_TRUE(is_safe_zip_entry_path("acecode.exe"));
    EXPECT_TRUE(is_safe_zip_entry_path("share/acecode/file.txt"));
    EXPECT_FALSE(is_safe_zip_entry_path("/abs/acecode.exe"));
    EXPECT_FALSE(is_safe_zip_entry_path("C:/abs/acecode.exe"));
    EXPECT_FALSE(is_safe_zip_entry_path("../acecode.exe"));
    EXPECT_FALSE(is_safe_zip_entry_path("dir/../acecode.exe"));
    EXPECT_FALSE(is_safe_zip_entry_path("dir\\acecode.exe"));
}

TEST(UpgradePackage, ValidatesRootOrSingleTopLevelPackage) {
    fs::path root = temp_root("acecode-staged-root");
    write_file(root / "acecode.exe", "exe");
    auto staged = validate_staged_package(root, "windows-x64", nullptr);
    ASSERT_TRUE(staged.has_value());
    EXPECT_EQ(staged->content_root, root);

    fs::path nested = temp_root("acecode-staged-nested");
    write_file(nested / "acecode-0.1.3" / "acecode.exe", "exe");
    auto nested_staged = validate_staged_package(nested, "windows-x64", nullptr);
    ASSERT_TRUE(nested_staged.has_value());
    EXPECT_EQ(nested_staged->content_root.filename(), "acecode-0.1.3");

    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(nested, ec);
}

TEST(UpgradePackage, RestoresUnixExecutablePermissions) {
    fs::path root = temp_root("acecode-zip-mode");
    fs::path archive = root / "update.zip";
    fs::path staging = root / "staging";
    ASSERT_TRUE(create_zip_entry(archive, "ACECode.app/Contents/MacOS/ACECode",
                                 "executable", 0100755u));

    std::string error;
    ASSERT_TRUE(extract_zip_to_staging(archive, staging, &error)) << error;
    const fs::perms permissions = fs::status(
        staging / "ACECode.app" / "Contents" / "MacOS" / "ACECode").permissions();
    EXPECT_NE(permissions & fs::perms::owner_exec, fs::perms::none);
    EXPECT_NE(permissions & fs::perms::group_exec, fs::perms::none);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(UpgradePackage, RejectsSymbolicLinksAndSpecialUnixEntries) {
    fs::path root = temp_root("acecode-zip-types");
    fs::path symlink_archive = root / "symlink.zip";
    ASSERT_TRUE(create_zip_entry(symlink_archive, "ACECode.app/link",
                                 "../../outside", 0120777u));

    std::string error;
    EXPECT_FALSE(extract_zip_to_staging(
        symlink_archive, root / "symlink-staging", &error));
    EXPECT_NE(error.find("symbolic link"), std::string::npos);

    fs::path fifo_archive = root / "fifo.zip";
    ASSERT_TRUE(create_zip_entry(fifo_archive, "ACECode.app/fifo", "", 0010644u));
    error.clear();
    EXPECT_FALSE(extract_zip_to_staging(
        fifo_archive, root / "fifo-staging", &error));
    EXPECT_NE(error.find("unsupported filesystem type"), std::string::npos);

    std::error_code ec;
    fs::remove_all(root, ec);
}

// 触发场景:用户目录含非 ASCII 字符(最常见的是中文用户名),升级包被下载到
// ~/.acecode/updates/... 后进入解压。
// 期望行为:解压正常完成,内容与打包时一致。
// 回归背景:extract_zip_to_staging 曾把 zip_path.string() 直接交给 zip_open。
// MSVC 的 path::string() 返回当前本地代码页(中文系统为 GBK)的字节,而 libzip
// 在 Windows 上把该参数当 UTF-8 严格解析(MultiByteToWideChar 带
// MB_ERR_INVALID_CHARS),于是 zip_open 恒返回 NULL,升级界面报
// "invalid package: failed to open zip package"。此前的用例全部落在纯 ASCII
// 的临时目录里,所以一直没能暴露。
TEST(UpgradePackage, ExtractsFromNonAsciiPackagePath) {
    // 目录名必须经 path_from_utf8 构造:源码是 UTF-8,直接 fs::path("中文")
    // 在 MSVC 上会按本地代码页解释,建出来的就是乱码目录。
    fs::path root = temp_root("acecode-zip-nonascii") /
                    acecode::path_from_utf8("用户-测试-ünïcode");
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_FALSE(ec) << ec.message();

    fs::path archive = root / "update.zip";
    fs::path staging = root / "staging";
    ASSERT_TRUE(create_zip_entry(archive, "acecode.exe", "payload", 0100755u));

    std::string error;
    ASSERT_TRUE(extract_zip_to_staging(archive, staging, &error)) << error;

    std::ifstream ifs(staging / "acecode.exe", std::ios::binary);
    ASSERT_TRUE(ifs.good());
    std::string body((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
    EXPECT_EQ(body, "payload");

    fs::remove_all(root, ec);
}

// 触发场景:包内条目名本身含非 ASCII 字符(例如 share/acecode/seed 下的中文
// 说明文件)。
// 期望行为:落盘文件名与包内名字逐字节一致,能按原名打开。
// 回归背景:同一函数曾用 fs::path(name) 承接 zip 条目名。libzip 交出的是
// UTF-8,而 MSVC 的 fs::path(std::string) 按本地代码页解释,会解出乱码文件名
// ——当前发布包碰巧全是 ASCII 条目名,属于尚未爆发的同源缺陷。
TEST(UpgradePackage, PreservesNonAsciiEntryNames) {
    fs::path root = temp_root("acecode-zip-entry-name");
    fs::path archive = root / "update.zip";
    fs::path staging = root / "staging";
    const std::string entry = "share/acecode/说明文档.md";
    ASSERT_TRUE(create_zip_entry(archive, entry, "doc", 0100644u));

    std::string error;
    ASSERT_TRUE(extract_zip_to_staging(archive, staging, &error)) << error;

    const fs::path expected = staging / acecode::path_from_utf8(entry);
    EXPECT_TRUE(fs::is_regular_file(expected))
        << "落盘文件名与包内条目名不一致";

    std::error_code ec;
    fs::remove_all(root, ec);
}
