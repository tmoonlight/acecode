// 本文件覆盖 GUI 升级任务链路(真正下载 / 解压 / 安装的那条)在失败时产出的错误文本编码。
// run_upgrade_command 把错误写进 errors 流,/api/update/start 的任务线程把它整理成 job.error,
// 随后 /api/update/job、/api/update/jobs/<id>、/api/update/jobs/<id>/cancel 都会序列化它。
// nlohmann::json::dump() 默认是严格模式,任何一个非 UTF-8 字节都会抛 type_error.316,
// 失败的任务一直留在内存里,前端每次轮询都是 500。所以这些文本必须在产生处就是合法 UTF-8:
// OS 错误文本(ec.message())单独经 ensure_utf8 再拼接,路径一律 path_to_utf8。
//
// 场景:
//   1. 安装时备份移动失败(文件被占用),路径在中文目录下(仅 Windows):错误里的两条路径
//      必须是 UTF-8 原文,OS 文本转码后拼接;诊断日志里 backup_move 的 error 字段不能是 U+FFFD。
//   2. 安装时备份目录建不出来(父路径被同名普通文件占住):"failed to create backup directory: "
//      后面的 OS 文本是合法 UTF-8。
//   3. 安装目录位于用户数据目录内、且目录名是中文:拒绝信息里的路径是 UTF-8 原文。
//   4. 解压时 staging 目录建不出来:"failed to create staging directory: " 的 OS 文本是合法 UTF-8。
//   5. 解压时条目的父目录建不出来(staging 里有同名普通文件):OS 文本是合法 UTF-8。
//   6. 校验 staging 时目录不存在:"failed to inspect staged package: " 的 OS 文本是合法 UTF-8。
//   7. run_upgrade_command 端到端:update workspace 建不出来,errors 流(即 GUI 的 job.error 来源)
//      是合法 UTF-8,诊断日志里 upgrade_finished 的 error 不带 U+FFFD。
//
// MSVC 的 ec.message() 与 path::string() 走 ANSI 代码页:只有中文 Windows(ACP 936)上它们才是
// GBK,能复现修复前的失败;英文 Windows 上中文路径的 path::string() 会直接抛 system_error
// (同样是修复前的失败);Linux / macOS 上两者本来就是 UTF-8,用例照常通过。

#include "config/config.hpp"
#include "upgrade/apply.hpp"
#include "upgrade/diagnostics.hpp"
#include "upgrade/manifest.hpp"
#include "upgrade/package.hpp"
#include "upgrade/upgrade.hpp"
#include "utils/encoding.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <zip.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace acecode::upgrade;

namespace {

// U+FFFD 的 UTF-8 编码。诊断日志按 error_handler_t::replace 写盘,非法字节会变成它:
// 日志里出现它,说明 OS 文本没有在产生处转码。
const std::string kReplacementChar = "\xEF\xBF\xBD";

// 失败信息里可能带着修复前的原始 GBK 字节;原样输出会让 gtest 的 XML 报告损坏,
// 不是合法 UTF-8 时把高位字节转义成 \xHH。
std::string printable(const std::string& text) {
    if (acecode::is_valid_utf8(text)) return text;
    static const char* const kHex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char byte : text) {
        if (byte < 0x80) {
            out.push_back(static_cast<char>(byte));
        } else {
            out += "\\x";
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0F]);
        }
    }
    return out;
}

struct TempDir {
    fs::path path;

    explicit TempDir(const std::string& label) {
        std::random_device rd;
        path = fs::temp_directory_path() / (label + "-" + std::to_string(rd()));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << body;
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

// 把 HOME / USERPROFILE 指到临时目录,让 get_acecode_dir()(用户数据目录、updates 工作区)
// 落在里面;Windows 上用宽字符接口设置,临时目录本身含中文时也不会被按 ANSI 代码页解错。
class ScopedHomeOverride {
public:
    explicit ScopedHomeOverride(const fs::path& home) {
        fs::create_directories(home);
        acecode::reset_run_mode_for_test();
        std::string current;
        if (acecode::getenv_utf8(kHomeEnvName, current)) previous_ = current;
        set_home(acecode::path_to_utf8(home));
    }

    ~ScopedHomeOverride() {
        if (previous_) {
            set_home(*previous_);
        } else {
#ifdef _WIN32
            _wputenv_s(L"USERPROFILE", L"");
#else
            unsetenv(kHomeEnvName);
#endif
        }
        acecode::reset_run_mode_for_test();
    }

    ScopedHomeOverride(const ScopedHomeOverride&) = delete;
    ScopedHomeOverride& operator=(const ScopedHomeOverride&) = delete;

private:
#ifdef _WIN32
    static constexpr const char* kHomeEnvName = "USERPROFILE";
    static void set_home(const std::string& utf8) {
        _wputenv_s(L"USERPROFILE", acecode::utf8_to_wide(utf8).c_str());
    }
#else
    static constexpr const char* kHomeEnvName = "HOME";
    static void set_home(const std::string& utf8) { setenv(kHomeEnvName, utf8.c_str(), 1); }
#endif

    std::optional<std::string> previous_;
};

// 只服务 /aceupdate.json 的本地清单服务器。
struct LocalManifestServer {
    httplib::Server svr;
    int port = 0;
    std::thread th;

    explicit LocalManifestServer(std::string body) {
        svr.Get("/aceupdate.json", [body](const httplib::Request&, httplib::Response& res) {
            res.set_content(body, "application/json");
        });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }

    ~LocalManifestServer() {
        svr.stop();
        if (th.joinable()) th.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/";
    }
};

// 当前平台有一个 9.9.9 可用包的清单;sha256 是占位值,用例在下载之前就失败。
std::string available_update_manifest() {
    const std::string target = manifest_target_for_platform(current_target());
    return R"({
      "schema_version": 1,
      "latest": "9.9.9",
      "releases": [
        {"version": "9.9.9", "packages": [
          {"target": ")" + target + R"(", "file": "acecode.zip", "sha256": ")" +
           std::string(64, 'a') + R"("}
        ]}
      ]
    })";
}

// 写一个只含一个普通文件条目的 zip(unix 模式 0100644,与发布包的条目类型一致)。
bool write_zip_with_entry(const fs::path& archive_path,
                          const std::string& entry_name,
                          const std::string& body) {
    int error = 0;
    zip_t* archive = zip_open(acecode::path_to_utf8(archive_path).c_str(),
                              ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!archive) return false;
    zip_source_t* source = zip_source_buffer(archive, body.data(), body.size(), 0);
    if (!source) {
        zip_discard(archive);
        return false;
    }
    const zip_int64_t index = zip_file_add(archive, entry_name.c_str(), source, ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_source_free(source);
        zip_discard(archive);
        return false;
    }
    if (zip_file_set_external_attributes(archive, static_cast<zip_uint64_t>(index), 0,
                                         ZIP_OPSYS_UNIX, 0100644u << 16u) != 0) {
        zip_discard(archive);
        return false;
    }
    return zip_close(archive) == 0;
}

// 诊断日志每行一条 JSON 记录(按 replace 写盘,每行都能严格解析);返回第一条 event 相同、
// 且满足 match(为空则不额外筛选)的记录。
std::optional<nlohmann::json> find_log_record(
    const std::string& log_path, const std::string& event,
    const std::function<bool(const nlohmann::json&)>& match = {}) {
    std::istringstream lines(read_file(acecode::path_from_utf8(log_path)));
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        auto record = nlohmann::json::parse(line);
        if (record.value("event", std::string{}) == event && (!match || match(record))) {
            return record;
        }
    }
    return std::nullopt;
}

} // namespace

#ifdef _WIN32
// 场景 1:安装替换时旧文件被别的进程占用,升级目录在中文路径下(例如 C:\Users\张三\...)。
// 触发:install / staging / backup 都在「张三-升级」目录下;CreateFileW 只给 FILE_SHARE_READ
//      占住 install\z-blocked.txt,backup_existing_path 的 rename 需要 DELETE 访问,失败于
//      ERROR_SHARING_VIOLATION(32),错误 = "failed to move <src> to <dest>: " + ec.message()。
// 期望:apply_staged_update 返回 false;error 与
//      "failed to move " + path_to_utf8(src) + " to " + path_to_utf8(dest) + ": " +
//      ensure_utf8(错误码 32 的系统文本) 逐字节相同,是合法 UTF-8,严格 dump 不抛异常;
//      诊断日志里 backup_move 记录(error_code=32)的 error 字段非空且不含 U+FFFD。
// 回归:修复前 src.string() / dest.string() 与 ec.message() 在中文 Windows 上都是 GBK,
//      error 经 run_upgrade_command 进 job.error,任务轮询接口 500;日志里系统文本则全变成
//      U+FFFD。英文 Windows 上「张三」无法映射到 ANSI 代码页,path::string() 直接抛 system_error。
TEST(UpdateJobUtf8, ApplyMoveFailureUnderChinesePathIsUtf8) {
    TempDir tmp("acecode-update-job-move");
    const fs::path root = tmp.path / acecode::path_from_utf8("张三-升级");
    const fs::path install = root / "install";
    const fs::path staging = root / "staging";
    const fs::path backup = root / "backup";
    write_file(install / "acecode.exe", "old exe");
    write_file(staging / "acecode.exe", "new exe");
    write_file(install / "z-blocked.txt", "old locked file");
    write_file(staging / "z-blocked.txt", "replacement");
    DiagnosticLog diagnostics("apply_utf8_test", tmp.path / "logs");

    struct FileHandle {
        HANDLE value;
        ~FileHandle() {
            if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value);
        }
    } lock{::CreateFileW((install / "z-blocked.txt").c_str(), GENERIC_READ, FILE_SHARE_READ,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    ASSERT_NE(lock.value, INVALID_HANDLE_VALUE);

    std::string error;
    EXPECT_FALSE(apply_staged_update(staging, install, backup, "windows-x64", &error,
                                     &diagnostics));

    const std::string os_text = acecode::ensure_utf8(
        std::error_code(ERROR_SHARING_VIOLATION, std::system_category()).message());
    EXPECT_EQ(error, "failed to move " + acecode::path_to_utf8(install / "z-blocked.txt") +
                         " to " + acecode::path_to_utf8(backup / "z-blocked.txt") + ": " +
                         os_text)
        << printable(error);
    EXPECT_TRUE(acecode::is_valid_utf8(error)) << printable(error);
    EXPECT_NO_THROW(nlohmann::json({{"error", error}}).dump());

    // acecode.exe 先被成功移走(error_code=0),要找的是 z-blocked.txt 那条失败记录。
    const auto record = find_log_record(
        diagnostics.path(), "backup_move", [](const nlohmann::json& item) {
            return item.at("details").value("error_code", 0) != 0;
        });
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ((*record)["details"]["error_code"].get<int>(),
              static_cast<int>(ERROR_SHARING_VIOLATION));
    const std::string logged = (*record)["details"]["error"].get<std::string>();
    EXPECT_FALSE(logged.empty());
    EXPECT_EQ(logged.find(kReplacementChar), std::string::npos) << logged;
    EXPECT_EQ(read_file(install / "acecode.exe"), "old exe");
}
#endif

// 场景 2:安装时备份目录建不出来 —— backup 的父路径被一个同名普通文件占住。
// 触发:backup = <tmp>/占位文件/backup,「占位文件」是普通文件;apply_staged_update 的
//      create_directories(backup_dir) 失败,错误 = "failed to create backup directory: " +
//      ec.message()。
// 期望:返回 false;error 以该前缀开头且前缀后还有系统文本,是合法 UTF-8,严格 dump 不抛异常;
//      失败发生在任何替换之前,安装目录里的旧文件原样保留。
// 回归:中文 Windows 上 ec.message() 是 GBK(如「当文件已存在时，无法创建该文件。」),修复前原样
//      进 job.error,任务轮询接口 500。英文系统与 Linux / macOS 上该文本是 ASCII,用例照常通过。
TEST(UpdateJobUtf8, ApplyBackupDirectoryFailureTextIsUtf8) {
    TempDir tmp("acecode-update-job-backup");
    const fs::path install = tmp.path / "install";
    const fs::path staging = tmp.path / "staging";
    const fs::path blocker = tmp.path / acecode::path_from_utf8("占位文件");
    write_file(install / "acecode.exe", "old exe");
    write_file(staging / "acecode.exe", "new exe");
    write_file(blocker, "occupies the backup parent directory");

    std::string error;
    EXPECT_FALSE(apply_staged_update(staging, install, blocker / "backup", "windows-x64",
                                     &error));

    const std::string prefix = "failed to create backup directory: ";
    EXPECT_EQ(error.rfind(prefix, 0), 0u) << printable(error);
    EXPECT_GT(error.size(), prefix.size()) << printable(error);
    EXPECT_TRUE(acecode::is_valid_utf8(error)) << printable(error);
    EXPECT_NO_THROW(nlohmann::json({{"error", error}}).dump());
    EXPECT_EQ(read_file(install / "acecode.exe"), "old exe");
}

// 场景 3:安装目录在用户数据目录(<home>/.acecode)里面,且目录名是中文。
// 触发:install = <home>/.acecode/安装目录;staged 包只有 acecode.exe,
//      staged_paths_avoid_user_data 发现 <install>/acecode.exe 落在数据目录内,拒绝更新。
// 期望:返回 false;error 与 "refusing to update a package path that overlaps ACECode user data: "
//      + path_to_utf8(<install>/acecode.exe) 逐字节相同;安装目录不变,备份目录没有被创建。
// 回归:修复前用 target.string() 拼接,中文 Windows 上「安装目录」是 GBK,job.error 序列化 500;
//      英文 Windows 上 path::string() 无法映射中文直接抛 system_error。
TEST(UpdateJobUtf8, ApplyUserDataOverlapReportsUtf8Path) {
    TempDir tmp("acecode-update-job-user-data");
    ScopedHomeOverride scoped_home(tmp.path / "home");
    const fs::path install =
        tmp.path / "home" / ".acecode" / acecode::path_from_utf8("安装目录");
    const fs::path staging = tmp.path / "staging";
    const fs::path backup = tmp.path / "backup";
    write_file(install / "acecode.exe", "old exe");
    write_file(staging / "acecode.exe", "new exe");

    std::string error;
    EXPECT_FALSE(apply_staged_update(staging, install, backup, "windows-x64", &error));

    EXPECT_EQ(error, "refusing to update a package path that overlaps ACECode user data: " +
                         acecode::path_to_utf8(install / "acecode.exe"))
        << printable(error);
    EXPECT_TRUE(acecode::is_valid_utf8(error)) << printable(error);
    EXPECT_EQ(read_file(install / "acecode.exe"), "old exe");
    EXPECT_FALSE(fs::exists(backup));
}

// 场景 4:解压前 staging 目录建不出来 —— staging 的父路径被同名普通文件占住。
// 触发:staging = <tmp>/占位文件/staging;extract_zip_to_staging 第一步 create_directories 失败,
//      错误 = "failed to create staging directory: " + ec.message()(此时还没打开 zip)。
// 期望:返回 false;error 以该前缀开头、前缀后有系统文本,是合法 UTF-8,严格 dump 不抛异常。
// 回归:中文 Windows 上 ec.message() 是 GBK,修复前经 "invalid package: ..." 进 job.error,
//      任务轮询接口 500。英文系统与 Linux / macOS 上该文本是 ASCII,用例照常通过。
TEST(UpdateJobUtf8, ExtractStagingDirectoryFailureTextIsUtf8) {
    TempDir tmp("acecode-update-job-staging");
    const fs::path blocker = tmp.path / acecode::path_from_utf8("占位文件");
    write_file(blocker, "occupies the staging parent directory");

    std::string error;
    EXPECT_FALSE(extract_zip_to_staging(tmp.path / "update.zip", blocker / "staging", &error));

    const std::string prefix = "failed to create staging directory: ";
    EXPECT_EQ(error.rfind(prefix, 0), 0u) << printable(error);
    EXPECT_GT(error.size(), prefix.size()) << printable(error);
    EXPECT_TRUE(acecode::is_valid_utf8(error)) << printable(error);
    EXPECT_NO_THROW(nlohmann::json({{"error", error}}).dump());
}

// 场景 5:解压条目时父目录建不出来 —— staging 里已经有一个与目录同名的普通文件。
// 触发:zip 里只有 share/asset.txt;解压前在 staging 下写一个名为 share 的普通文件,
//      create_directories(<staging>/share) 失败,错误 =
//      "failed to create parent directory from zip: " + ec.message()。
// 期望:返回 false;error 以该前缀开头、前缀后有系统文本,是合法 UTF-8,严格 dump 不抛异常。
// 回归:中文 Windows 上 ec.message() 是 GBK(「当文件已存在时，无法创建该文件。」),修复前
//      原样进 job.error,任务轮询接口 500。英文系统与 Linux / macOS 上用例照常通过。
TEST(UpdateJobUtf8, ExtractParentDirectoryFailureTextIsUtf8) {
    TempDir tmp("acecode-update-job-extract");
    const fs::path archive = tmp.path / "update.zip";
    const fs::path staging = tmp.path / "staging";
    ASSERT_TRUE(write_zip_with_entry(archive, "share/asset.txt", "asset"));
    write_file(staging / "share", "occupies the share directory");

    std::string error;
    EXPECT_FALSE(extract_zip_to_staging(archive, staging, &error));

    const std::string prefix = "failed to create parent directory from zip: ";
    EXPECT_EQ(error.rfind(prefix, 0), 0u) << printable(error);
    EXPECT_GT(error.size(), prefix.size()) << printable(error);
    EXPECT_TRUE(acecode::is_valid_utf8(error)) << printable(error);
    EXPECT_NO_THROW(nlohmann::json({{"error", error}}).dump());
}

// 场景 6:校验 staging 时目录根本不存在(解压目录被安全软件清掉等)。
// 触发:validate_staged_package(<tmp>/不存在的目录) —— 根下没有 acecode.exe,接着
//      directory_iterator 打不开目录,错误 = "failed to inspect staged package: " + ec.message()。
// 期望:返回 nullopt;error 以该前缀开头、前缀后有系统文本,是合法 UTF-8,严格 dump 不抛异常。
// 回归:中文 Windows 上 ec.message() 是 GBK(「系统找不到指定的路径。」),修复前原样进
//      job.error,任务轮询接口 500。英文系统与 Linux / macOS 上用例照常通过。
TEST(UpdateJobUtf8, ValidateMissingStagingDirectoryTextIsUtf8) {
    TempDir tmp("acecode-update-job-validate");

    std::string error;
    const auto staged = validate_staged_package(
        tmp.path / acecode::path_from_utf8("不存在的目录"), "windows-x64", &error);

    EXPECT_FALSE(staged.has_value());
    const std::string prefix = "failed to inspect staged package: ";
    EXPECT_EQ(error.rfind(prefix, 0), 0u) << printable(error);
    EXPECT_GT(error.size(), prefix.size()) << printable(error);
    EXPECT_TRUE(acecode::is_valid_utf8(error)) << printable(error);
    EXPECT_NO_THROW(nlohmann::json({{"error", error}}).dump());
}

// 场景 7:run_upgrade_command 端到端 —— 清单显示有更新,但 update workspace 建不出来。
// 触发:HOME 指向临时目录,<home>/.acecode/updates 被同名普通文件占住;本地清单服务器返回
//      当前平台的 9.9.9。run_upgrade_command 选中包后 create_directories(<updates>/acecode-update-*)
//      失败,errors 流写入 "acecode upgrade: failed to create update workspace: " + ec.message(),
//      GUI 任务线程正是把这个流整理成 job.error。
// 期望:返回 1;err 带该前缀,是合法 UTF-8,严格 dump 不抛异常;诊断日志里 upgrade_finished
//      记录的 error 字段带同一前缀且不含 U+FFFD。失败发生在下载之前,不会碰测试程序所在目录。
// 回归:中文 Windows 上 ec.message() 是 GBK,修复前 job.error 序列化 500,日志里则是一串 U+FFFD。
//      英文系统与 Linux / macOS 上该文本是 ASCII,用例照常通过。
TEST(UpdateJobUtf8, UpgradeWorkspaceFailureTextIsUtf8) {
    TempDir tmp("acecode-update-job-workspace");
    const fs::path home = tmp.path / "home";
    ScopedHomeOverride scoped_home(home);
    write_file(home / ".acecode" / "updates", "occupies the update workspace directory");
    LocalManifestServer server(available_update_manifest());
    acecode::AppConfig cfg;
    cfg.network.proxy_mode = "off";
    cfg.upgrade.base_url = server.base_url();
    DiagnosticLog diagnostics("upgrade", tmp.path / "logs");

    std::ostringstream out;
    std::ostringstream err;
    const int code = run_upgrade_command(cfg, "", "0.1.2", out, err, false, {}, {}, &diagnostics);

    EXPECT_EQ(code, 1);
    const std::string errors = err.str();
    EXPECT_NE(errors.find("acecode upgrade: failed to create update workspace: "),
              std::string::npos)
        << printable(errors);
    EXPECT_TRUE(acecode::is_valid_utf8(errors)) << printable(errors);
    EXPECT_NO_THROW(nlohmann::json({{"error", errors}}).dump());

    const auto record = find_log_record(diagnostics.path(), "upgrade_finished");
    ASSERT_TRUE(record.has_value());
    const std::string logged = (*record)["details"]["error"].get<std::string>();
    EXPECT_NE(logged.find("failed to create update workspace: "), std::string::npos) << logged;
    EXPECT_EQ(logged.find(kReplacementChar), std::string::npos) << logged;
}
