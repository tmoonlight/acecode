// 覆盖 src/apps/installer/payload_archive:安装程序资源区里的 zip 与描述 JSON。
//   1. 正常 zip(含子目录)解压到目标目录,文件内容一致,进度单调并以 100% 结束。
//   2. 路径穿越 / 盘符 / 冒号 / 设备名 / 末尾点号的条目整包拒绝,目录外不留下任何文件。
//   3. 不是 zip 的字节流报错。
//   4. 描述 JSON:合法时解析出版本、目标平台、大小与哈希;缺字段或字段非法时拒绝。
//   5. sha256_of 与标准测试向量一致。

#include "installer/payload_archive.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>
#include <zip.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

using namespace acecode::installer;
namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const std::string& name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() /
                         ("acecode-payload-" + name + "-" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

// 用 libzip 在磁盘上建一个 zip,再整块读回字节(安装程序从资源区拿到的也是一整块内存)。
std::string make_zip(const fs::path& dir, const std::vector<std::pair<std::string, std::string>>& entries) {
    const fs::path path = dir / "payload.zip";
    int error = 0;
    zip_t* archive = zip_open(acecode::path_to_utf8(path).c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!archive) return {};
    for (const auto& [name, body] : entries) {
        if (!name.empty() && name.back() == '/') {
            zip_dir_add(archive, name.c_str(), ZIP_FL_ENC_UTF_8);
            continue;
        }
        zip_source_t* source = zip_source_buffer(archive, body.data(), body.size(), 0);
        zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
    }
    if (zip_close(archive) != 0) return {};
    std::ifstream in(path, std::ios::binary);
    std::stringstream bytes;
    bytes << in.rdbuf();
    return bytes.str();
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream bytes;
    bytes << in.rdbuf();
    return bytes.str();
}

class RecordingObserver final : public ExtractObserver {
public:
    void on_extract_progress(std::uint64_t done, std::uint64_t total) override {
        calls.emplace_back(done, total);
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
};

}  // namespace

// 场景:与 CI 包同形的 zip:根目录 acecode.exe、空目录、share/acecode/seed/a.txt 嵌套文件。
// 期望:解压成功,文件个数 2、总字节数正确,内容逐字节一致;进度回调单调不减且最后一次为 total/total。
TEST(PayloadArchive, ExtractsNestedFilesWithProgress) {
    const fs::path work = fresh_dir("ok");
    const std::string big(300 * 1024, 'x');
    const std::string bytes = make_zip(work, {{"acecode.exe", big},
                                              {"channels/", ""},
                                              {"share/acecode/seed/a.txt", "seed"}});
    ASSERT_FALSE(bytes.empty());
    RecordingObserver observer;
    const fs::path out = work / "out";
    const auto result = extract_archive(bytes, out, &observer);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.files, 2u);
    EXPECT_EQ(result.bytes, big.size() + 4);
    EXPECT_EQ(read_file(out / "acecode.exe"), big);
    EXPECT_EQ(read_file(out / "share" / "acecode" / "seed" / "a.txt"), "seed");
    EXPECT_TRUE(fs::is_directory(out / "channels"));
    ASSERT_FALSE(observer.calls.empty());
    std::uint64_t last = 0;
    for (const auto& [done, total] : observer.calls) {
        EXPECT_GE(done, last);
        EXPECT_EQ(total, result.bytes);
        last = done;
    }
    EXPECT_EQ(observer.calls.back().first, result.bytes);
    std::error_code ec;
    fs::remove_all(work, ec);
}

// 场景:安装目录本身就很长(用户名长、自选深层目录,或开发机上的测试沙盒),加上包里最深的
// seed 文件后整条路径超过 260 个字符。
// 期望:照常解压成功、内容一致。回归:修复前 ofstream 打不开这类路径,安装停在「解压文件失败」。
TEST(PayloadArchive, ExtractsBeyondMaxPath) {
    const fs::path work = fresh_dir("long");
    const std::string deep =
        "share/acecode/seed/experts/opc-conversion-designer/skills/opc-conversion-loop/"
        "references/channel-playbook.md";
    const std::string bytes = make_zip(work, {{"acecode.exe", "ok"}, {deep, "deep"}});
    ASSERT_FALSE(bytes.empty());
    fs::path out = work;
    for (int i = 0; i < 6; ++i) out /= "a-rather-long-folder-name-for-max-path";
    const std::string full = acecode::path_to_utf8(out) + "/" + deep;
    ASSERT_GT(full.size(), 260u) << "测试路径不够长,覆盖不到 MAX_PATH";
    const auto result = extract_archive(bytes, out, nullptr);
    ASSERT_TRUE(result.ok) << result.error;
    std::ifstream in(acecode::to_extended_length_path(out / acecode::path_from_utf8(deep).make_preferred()),
                     std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    EXPECT_EQ(text.str(), "deep");
    std::error_code ec;
    fs::remove_all(acecode::to_extended_length_path(work), ec);
}

// 场景:zip 里混进 ../evil.txt(先列一个正常文件)。
// 期望:整包拒绝、报出条目名;安装目录的上一级没有 evil.txt,正常文件也没有被写出
// (检查在写任何文件之前完成,防止半个包落盘)。
TEST(PayloadArchive, RejectsTraversalBeforeWritingAnything) {
    const fs::path work = fresh_dir("evil");
    const std::string bytes = make_zip(work, {{"acecode.exe", "ok"}, {"../evil.txt", "boom"}});
    ASSERT_FALSE(bytes.empty());
    const fs::path out = work / "out";
    const auto result = extract_archive(bytes, out, nullptr);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("evil.txt"), std::string::npos);
    EXPECT_FALSE(fs::exists(work / "evil.txt"));
    EXPECT_FALSE(fs::exists(out / "acecode.exe"));
    std::error_code ec;
    fs::remove_all(work, ec);
}

// 场景:逐个检查条目名。
// 期望:普通相对路径与反斜杠分隔的相对路径接受;绝对路径、盘符、冒号(备用数据流)、
// ..、Windows 设备名(CON、com1.txt)、末尾点号或空格的名字拒绝。
TEST(PayloadArchive, EntryNameSafety) {
    EXPECT_TRUE(is_safe_archive_entry("acecode.exe"));
    EXPECT_TRUE(is_safe_archive_entry("share/acecode/models_dev/api.json"));
    EXPECT_TRUE(is_safe_archive_entry("share\\acecode\\seed\\x.md"));
    EXPECT_TRUE(is_safe_archive_entry("channels/"));
    EXPECT_FALSE(is_safe_archive_entry("/etc/passwd"));
    EXPECT_FALSE(is_safe_archive_entry("C:/Windows/x.dll"));
    EXPECT_FALSE(is_safe_archive_entry("acecode.exe:evil"));
    EXPECT_FALSE(is_safe_archive_entry("a/../../b"));
    EXPECT_FALSE(is_safe_archive_entry("CON"));
    EXPECT_FALSE(is_safe_archive_entry("logs/com1.txt"));
    EXPECT_FALSE(is_safe_archive_entry("trailing."));
    EXPECT_FALSE(is_safe_archive_entry("trailing "));
    EXPECT_FALSE(is_safe_archive_entry(""));
}

// 场景:资源区里的安装包被截断或替换成任意字节。
// 期望:报错而不是崩溃或解出部分文件。
TEST(PayloadArchive, RejectsNonZipBytes) {
    const fs::path work = fresh_dir("junk");
    const auto result = extract_archive("definitely not a zip archive", work / "out", nullptr);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
    std::error_code ec;
    fs::remove_all(work, ec);
}

// 场景:make_setup.py 写入的描述 JSON,以及几种被改坏的版本。
// 期望:合法描述往返一致;未知目标平台、非十六进制哈希、缺大小、版本号含字母时拒绝。
TEST(PayloadArchive, PayloadInfoValidation) {
    PayloadInfo info;
    info.version = "1.0.20";
    info.target = "windows-x64";
    info.payload_sha256 = std::string(64, 'a');
    info.uninstaller_sha256 = std::string(64, 'b');
    info.payload_size = 1234;
    std::string error;
    const auto parsed = parse_payload_info(serialize_payload_info(info), &error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->version, "1.0.20");
    EXPECT_EQ(parsed->target, "windows-x64");
    EXPECT_EQ(parsed->payload_size, 1234u);

    auto bad = info;
    bad.target = "linux-x64";
    EXPECT_FALSE(parse_payload_info(serialize_payload_info(bad), &error).has_value());
    bad = info;
    bad.payload_sha256 = std::string(64, 'Z');
    EXPECT_FALSE(parse_payload_info(serialize_payload_info(bad), &error).has_value());
    bad = info;
    bad.payload_size = 0;
    EXPECT_FALSE(parse_payload_info(serialize_payload_info(bad), &error).has_value());
    bad = info;
    bad.version = "1.0.20-beta";
    EXPECT_FALSE(parse_payload_info(serialize_payload_info(bad), &error).has_value());
    EXPECT_FALSE(parse_payload_info("not json", &error).has_value());
}

// 场景:FIPS 180-2 的 "abc" 与空串测试向量。
// 期望:与标准摘要一致(安装前靠它确认资源区没被改动)。
TEST(PayloadArchive, Sha256MatchesKnownVectors) {
    EXPECT_EQ(sha256_of("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_of(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}
