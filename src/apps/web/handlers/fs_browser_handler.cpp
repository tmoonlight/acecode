#include "fs_browser_handler.hpp"

#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/statvfs.h>
#  include <unistd.h>
#endif

namespace acecode::web {

namespace fs = std::filesystem;

namespace {

std::string generic_utf8(const fs::path& p) {
#ifdef _WIN32
    return acecode::wide_to_utf8(p.generic_wstring());
#else
    return p.generic_string();
#endif
}

std::string trim_ascii(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 把一个已知绝对的 fs::path 变成显示形态。lexically_normal 会把 "C:/Users/" 保留成带
// 尾斜杠的形式("C:/Users/" 的最后一个分量是空 filename),这里统一剥掉;判断「是不是
// 盘根」用 relative_path().empty():"C:" 与 "/" 的 relative_path 都为空,"C:/Users" 不为空。
std::string finish_display(fs::path p) {
    p = p.lexically_normal();
    std::string s = generic_utf8(p);
    if (s.size() > 1 && s.back() == '/') {
        const std::string candidate = s.substr(0, s.size() - 1);
        if (!path_from_utf8(candidate).relative_path().empty()) s = candidate;
    }
#ifdef _WIN32
    if (s.size() >= 2 && s[1] == ':' && std::isalpha(static_cast<unsigned char>(s[0]))) {
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    }
#endif
    return s;
}

// fs::file_time_type → unix epoch ms(与 files_handler 同款:C++17 没有标准转换)。
std::int64_t file_time_to_unix_ms(fs::file_time_type ftime) {
    using namespace std::chrono;
    const auto ftime_now    = fs::file_time_type::clock::now();
    const auto sysclock_now = system_clock::now();
    const auto delta = ftime - ftime_now;
    const auto sys_time = sysclock_now + duration_cast<system_clock::duration>(delta);
    return duration_cast<milliseconds>(sys_time.time_since_epoch()).count();
}

// 读取链接目录的目标。MSVC 的 read_symlink 对 junction(IO_REPARSE_TAG_MOUNT_POINT)与
// symlink 都支持,但可能带 NT 内部前缀 \??\ 或 \\?\,generic 化后变成 /??/ 与 //?/,这里剥掉。
// 取不到(OneDrive 占位、AppExecLink 等奇怪的 reparse point)就返回 nullopt,条目仍按目录列出。
std::optional<std::string> read_link_target(const fs::path& p) {
    std::error_code ec;
    const fs::path target = fs::read_symlink(p, ec);
    if (ec || target.empty()) return std::nullopt;
    std::string s = generic_utf8(target);
    for (const char* prefix : {"/??/", "//?/"}) {
        const std::string pre(prefix);
        if (s.rfind(pre, 0) == 0) { s = s.substr(pre.size()); break; }
    }
    const fs::path cleaned = path_from_utf8(s);
    if (cleaned.is_absolute()) return finish_display(cleaned);
    return s;
}

// std::error_code::message() 在中文 Windows 上返回 ANSI 代码页字节("系统找不到指定的
// 文件。"是 GBK),直接塞进 nlohmann::json 会在 dump 时抛 invalid UTF-8,整个请求变成
// Crow 的裸 500。统一转成 UTF-8 再入 detail。
std::string error_text(const std::error_code& ec) {
    return acecode::ensure_utf8(ec.message());
}

std::string fold_ascii_lower(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

#ifdef _WIN32
// 枚举盘符期间关掉「驱动器中没有磁盘,请插入」这类系统模态框 —— 否则 daemon 进程会被
// 一个看不见的对话框卡住,HTTP 请求永远不返回。线程级设置,析构时恢复。
struct ThreadErrorModeGuard {
    DWORD old_mode = 0;
    bool applied = false;
    ThreadErrorModeGuard() {
        applied = ::SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX,
                                       &old_mode) != 0;
    }
    ~ThreadErrorModeGuard() {
        if (applied) ::SetThreadErrorMode(old_mode, nullptr);
    }
};
#else
std::optional<std::pair<std::uint64_t, std::uint64_t>> statvfs_capacity(const std::string& p) {
    struct statvfs sv {};
    if (::statvfs(p.c_str(), &sv) != 0) return std::nullopt;
    const std::uint64_t frs = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
    return std::make_pair(static_cast<std::uint64_t>(sv.f_blocks) * frs,
                          static_cast<std::uint64_t>(sv.f_bavail) * frs);
}
#endif

} // namespace

std::optional<std::string> normalize_browse_path(const std::string& path_utf8) {
    const std::string text = trim_ascii(path_utf8);
    if (text.empty()) return std::nullopt;
    const fs::path p = path_from_utf8(text);
    // Windows 上 "C:"(无斜杠)与 "/foo"(无盘符)都不是绝对路径,一并拒绝。
    if (!p.is_absolute()) return std::nullopt;
    return finish_display(p);
}

std::string browse_parent_path(const std::string& normalized) {
    const fs::path p = path_from_utf8(normalized);
    if (p.relative_path().empty()) return {};
    return finish_display(p.parent_path());
}

std::variant<FsBrowseResult, FsBrowseError>
browse_directory(const std::string& path_utf8, bool show_hidden, std::size_t max_entries) {
    const auto normalized = normalize_browse_path(path_utf8);
    if (!normalized) {
        return FsBrowseError{FsBrowseErrorKind::NotAbsolute, "path must be an absolute path"};
    }
    const fs::path dir = path_from_utf8(*normalized);

    std::error_code ec;
    const auto st = fs::status(dir, ec);  // 跟随 junction / symlink 判断目标是否是目录
    if (ec) {
        if (ec == std::errc::permission_denied) {
            return FsBrowseError{FsBrowseErrorKind::AccessDenied, error_text(ec)};
        }
        return FsBrowseError{FsBrowseErrorKind::NotFound, error_text(ec)};
    }
    if (!fs::exists(st)) {
        return FsBrowseError{FsBrowseErrorKind::NotFound, "directory not found"};
    }
    if (!fs::is_directory(st)) {
        return FsBrowseError{FsBrowseErrorKind::NotDirectory, "not a directory"};
    }

    fs::directory_iterator it(dir, ec);
    if (ec) {
        if (ec == std::errc::permission_denied) {
            return FsBrowseError{FsBrowseErrorKind::AccessDenied, error_text(ec)};
        }
        return FsBrowseError{FsBrowseErrorKind::IoError, error_text(ec)};
    }

    FsBrowseResult result;
    result.path = *normalized;
    result.parent = browse_parent_path(*normalized);
    const std::string prefix = result.path.back() == '/' ? result.path : result.path + "/";

    fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;  // 单项错误不致命,返回已收集
        const fs::path entry_path = it->path();
        const std::string name = generic_utf8(entry_path.filename());
        if (name.empty()) continue;

        bool hidden = name.front() == '.';
        bool linked = false;
        bool is_dir = false;
#ifdef _WIN32
        const DWORD attrs = ::GetFileAttributesW(entry_path.c_str());
        const bool attrs_ok = attrs != INVALID_FILE_ATTRIBUTES;
        if (attrs_ok) {
            if (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) hidden = true;
            if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) linked = true;
            // 按属性判目录:悬空的 junction 用 is_directory 跟随会失败,但它对用户来说仍是目录。
            is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        } else {
            std::error_code dec;
            is_dir = fs::is_directory(entry_path, dec) && !dec;
        }
#else
        {
            std::error_code dec;
            is_dir = fs::is_directory(entry_path, dec) && !dec;  // 跟随 symlink
        }
#endif
        {
            std::error_code lec;
            const auto ls = it->symlink_status(lec);
            if (!lec && fs::is_symlink(ls)) linked = true;
        }
        if (hidden && !show_hidden) continue;
        if (result.entries.size() >= max_entries) {
            result.truncated = true;
            break;
        }

        FsBrowseEntry e;
        e.name = name;
        e.path = prefix + name;
        e.kind = is_dir ? "dir" : "file";
        e.hidden = hidden;
        if (is_dir && linked) e.link_target = read_link_target(entry_path);
        if (!is_dir) {
            std::error_code sec;
            const auto sz = fs::file_size(entry_path, sec);
            if (!sec) e.size = static_cast<std::uint64_t>(sz);
        }
        {
            std::error_code tec;
            const auto ft = fs::last_write_time(entry_path, tec);
            if (!tec) e.modified_ms = file_time_to_unix_ms(ft);
        }
        result.entries.push_back(std::move(e));
    }

    // 目录优先,然后按名称不区分大小写排序(与资源管理器一致);完全同名只差大小写时按原样兜底。
    std::sort(result.entries.begin(), result.entries.end(),
              [](const FsBrowseEntry& a, const FsBrowseEntry& b) {
                  const bool da = a.kind == "dir";
                  const bool db = b.kind == "dir";
                  if (da != db) return da;
                  const std::string fa = fold_ascii_lower(a.name);
                  const std::string fb = fold_ascii_lower(b.name);
                  if (fa != fb) return fa < fb;
                  return a.name < b.name;
              });
    return result;
}

std::vector<FsRootEntry> enumerate_roots() {
    std::vector<FsRootEntry> out;
#ifdef _WIN32
    ThreadErrorModeGuard guard;
    const DWORD mask = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t letter = static_cast<wchar_t>(L'A' + i);
        const std::wstring root = std::wstring(1, letter) + L":\\";
        const UINT type = ::GetDriveTypeW(root.c_str());
        FsRootEntry e;
        e.path = std::string(1, static_cast<char>('A' + i)) + ":/";
        switch (type) {
            case DRIVE_FIXED:     e.drive_type = "fixed";     break;
            case DRIVE_REMOVABLE: e.drive_type = "removable"; break;
            case DRIVE_REMOTE:    e.drive_type = "remote";    break;
            case DRIVE_CDROM:     e.drive_type = "cdrom";     break;
            case DRIVE_RAMDISK:   e.drive_type = "ramdisk";   break;
            default:              continue;  // DRIVE_UNKNOWN / DRIVE_NO_ROOT_DIR
        }
        // 断开的网络映射盘上 GetVolumeInformationW / GetDiskFreeSpaceExW 可能阻塞数秒,
        // 远程盘一律不查,只给路径与类型。
        if (type != DRIVE_REMOTE) {
            wchar_t label[MAX_PATH + 1] = {0};
            if (::GetVolumeInformationW(root.c_str(), label, MAX_PATH + 1,
                                        nullptr, nullptr, nullptr, nullptr, 0)) {
                e.label = acecode::wide_to_utf8(label);
            }
            ULARGE_INTEGER avail{};
            ULARGE_INTEGER total{};
            if (::GetDiskFreeSpaceExW(root.c_str(), &avail, &total, nullptr)) {
                e.total_bytes = static_cast<std::uint64_t>(total.QuadPart);
                e.free_bytes  = static_cast<std::uint64_t>(avail.QuadPart);
            }
        }
        out.push_back(std::move(e));
    }
#else
    {
        FsRootEntry e;
        e.path = "/";
        e.drive_type = "root";
        if (auto cap = statvfs_capacity("/")) {
            e.total_bytes = cap->first;
            e.free_bytes  = cap->second;
        }
        out.push_back(std::move(e));
    }
#  ifdef __APPLE__
    {
        std::error_code ec;
        fs::directory_iterator it("/Volumes", ec);
        fs::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code dec;
            if (!fs::is_directory(it->path(), dec) || dec) continue;
            FsRootEntry e;
            e.path = finish_display(it->path());
            e.label = generic_utf8(it->path().filename());
            e.drive_type = "volume";
            if (auto cap = statvfs_capacity(e.path)) {
                e.total_bytes = cap->first;
                e.free_bytes  = cap->second;
            }
            out.push_back(std::move(e));
        }
    }
#  endif
#endif
    return out;
}

std::string host_name() {
    for (const char* name : {"COMPUTERNAME", "HOSTNAME"}) {
        const std::string value = trim_ascii(acecode::getenv_utf8(name));
        if (!value.empty()) return value;
    }
#ifndef _WIN32
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') return std::string(buf);
#endif
    return {};
}

std::string host_os_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string home_directory() {
#ifdef _WIN32
    std::string home = acecode::getenv_utf8("USERPROFILE");
    if (home.empty()) {
        const std::string drive = acecode::getenv_utf8("HOMEDRIVE");
        const std::string path  = acecode::getenv_utf8("HOMEPATH");
        if (!drive.empty() && !path.empty()) home = drive + path;
    }
#else
    std::string home = acecode::getenv_utf8("HOME");
#endif
    if (home.empty()) return {};
    const auto normalized = normalize_browse_path(home);
    return normalized ? *normalized : std::string{};
}

std::optional<std::string> desktop_directory(const std::string& home) {
    if (home.empty()) return std::nullopt;
    const fs::path candidate = path_from_utf8(home) / "Desktop";
    std::error_code ec;
    if (fs::is_directory(candidate, ec) && !ec) return finish_display(candidate);
    return std::nullopt;
}

} // namespace acecode::web
