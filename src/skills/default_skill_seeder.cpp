#include "default_skill_seeder.hpp"

#include "../utils/atomic_file.hpp"
#include "../utils/logger.hpp"
#include "../utils/sha256.hpp"
#include "../utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace acecode {

namespace {

constexpr const char* kSeedStateFile = ".seed_skills_state.json";
constexpr const char* kSeedVersionFile = "seed.version";
constexpr const char* kSeedUpdateLockFile = ".seed_skills_update.lock";
constexpr const char* kSeedStagingDir = ".seed_skills_staging";
constexpr const char* kSeedBackupDir = ".seed_skills_backup";
constexpr std::size_t kMaxSeedVersionBytes = 128;

struct ParsedSeedVersion {
    std::string text;
    std::string date;
    std::uint64_t revision = 0;
};

struct PreviousSeedState {
    std::string result;
    std::string installed_tree_sha256;
    std::string skill_md_hash;
    bool acecode_owned = false;
};

std::mutex g_seed_reconciliation_mutex;

void append_error(DefaultSkillSeedInstallResult& result,
                  const std::string& message) {
    if (message.empty()) return;
    if (!result.error.empty()) result.error += "; ";
    result.error += message;
}

std::string trim_ascii(std::string value) {
    auto is_space = [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (!value.empty() &&
           is_space(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           is_space(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

template <typename Integer>
bool parse_decimal(std::string_view text, Integer& value) {
    if (text.empty()) return false;
    for (char ch : text) {
        if (ch < '0' || ch > '9') return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool is_leap_year(unsigned year) {
    return year % 400u == 0u || (year % 4u == 0u && year % 100u != 0u);
}

std::optional<ParsedSeedVersion> parse_seed_version(std::string text) {
    text = trim_ascii(std::move(text));
    if (text.size() < 12 || text[4] != '-' || text[7] != '-' ||
        text[10] != '.') {
        return std::nullopt;
    }

    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    std::uint64_t revision = 0;
    if (!parse_decimal<unsigned>(std::string_view(text).substr(0, 4), year) ||
        !parse_decimal<unsigned>(std::string_view(text).substr(5, 2), month) ||
        !parse_decimal<unsigned>(std::string_view(text).substr(8, 2), day) ||
        !parse_decimal<std::uint64_t>(
            std::string_view(text).substr(11), revision)) {
        return std::nullopt;
    }

    static constexpr std::array<unsigned, 12> kDaysPerMonth = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    if (month == 0 || month > kDaysPerMonth.size()) return std::nullopt;
    unsigned max_day = kDaysPerMonth[month - 1];
    if (month == 2 && is_leap_year(year)) max_day = 29;
    if (day == 0 || day > max_day) return std::nullopt;

    return ParsedSeedVersion{
        text,
        text.substr(0, 10),
        revision,
    };
}

int compare_seed_versions(const ParsedSeedVersion& lhs,
                          const ParsedSeedVersion& rhs) {
    if (lhs.date < rhs.date) return -1;
    if (lhs.date > rhs.date) return 1;
    if (lhs.revision < rhs.revision) return -1;
    if (lhs.revision > rhs.revision) return 1;
    return 0;
}

std::optional<std::string> read_small_text_file(const fs::path& path,
                                                std::string& error) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        error = "failed to open " + path_to_utf8_generic(path);
        return std::nullopt;
    }

    std::array<char, kMaxSeedVersionBytes + 1> buffer{};
    ifs.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = ifs.gcount();
    if (count > static_cast<std::streamsize>(kMaxSeedVersionBytes)) {
        error = "file is too large: " + path_to_utf8_generic(path);
        return std::nullopt;
    }
    if (ifs.bad()) {
        error = "failed to read " + path_to_utf8_generic(path);
        return std::nullopt;
    }
    return std::string(buffer.data(), static_cast<std::size_t>(count));
}

std::optional<ParsedSeedVersion> read_seed_version(const fs::path& path,
                                                   std::string& error) {
    auto text = read_small_text_file(path, error);
    if (!text) return std::nullopt;
    auto parsed = parse_seed_version(std::move(*text));
    if (!parsed) {
        error = "invalid seed version in " + path_to_utf8_generic(path);
    }
    return parsed;
}

bool seed_dir_has_all_skills(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& seed : default_skill_seeds()) {
        if (!fs::is_regular_file(dir / seed.relative_path / "SKILL.md", ec)) {
            return false;
        }
    }
    return true;
}

fs::path normalized_path(const fs::path& path) {
    std::error_code ec;
    fs::path normalized = fs::weakly_canonical(path, ec);
    if (ec) normalized = path.lexically_normal();
    return normalized;
}

std::optional<fs::path> valid_seed_dir(const fs::path& path) {
    fs::path normalized = normalized_path(path);
    if (seed_dir_has_all_skills(normalized)) return normalized;
    return std::nullopt;
}

std::optional<std::string> hash_file_fnv1a64(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return std::nullopt;

    std::uint64_t hash = 14695981039346656037ULL;
    char buffer[4096];
    while (ifs.good()) {
        ifs.read(buffer, sizeof(buffer));
        const std::streamsize count = ifs.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ULL;
        }
    }
    if (ifs.bad()) return std::nullopt;

    std::ostringstream oss;
    oss << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16)
        << hash;
    return oss.str();
}

void sha256_update_u64(Sha256& sha, std::uint64_t value) {
    std::array<unsigned char, 8> bytes{};
    for (int i = 7; i >= 0; --i) {
        bytes[static_cast<std::size_t>(7 - i)] =
            static_cast<unsigned char>((value >> (i * 8)) & 0xffu);
    }
    sha.update(bytes.data(), bytes.size());
}

std::optional<std::string> hash_directory_tree(const fs::path& root,
                                               std::string& error) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        error = "not a directory: " + path_to_utf8_generic(root);
        return std::nullopt;
    }

    struct TreeEntry {
        std::string relative_path;
        fs::path path;
        bool is_directory = false;
    };
    std::vector<TreeEntry> entries;

    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code status_ec;
        const fs::file_status status = it->symlink_status(status_ec);
        if (status_ec) {
            error = "failed to inspect seed path: " + status_ec.message();
            return std::nullopt;
        }
        if (fs::is_symlink(status)) {
            error = "seed directories must not contain symlinks: " +
                    path_to_utf8_generic(it->path());
            return std::nullopt;
        }
        if (fs::is_regular_file(status) || fs::is_directory(status)) {
            std::error_code relative_ec;
            const fs::path relative =
                fs::relative(it->path(), root, relative_ec);
            if (relative_ec) {
                error = "failed to make seed path relative: " +
                        relative_ec.message();
                return std::nullopt;
            }
            entries.push_back({
                path_to_utf8_generic(relative),
                it->path(),
                fs::is_directory(status),
            });
        } else {
            error = "unsupported file type in seed directory: " +
                    path_to_utf8_generic(it->path());
            return std::nullopt;
        }
        it.increment(ec);
    }
    if (ec) {
        error = "failed to scan seed directory: " + ec.message();
        return std::nullopt;
    }

    std::sort(entries.begin(), entries.end(),
              [](const TreeEntry& lhs, const TreeEntry& rhs) {
                  if (lhs.relative_path != rhs.relative_path) {
                      return lhs.relative_path < rhs.relative_path;
                  }
                  return lhs.is_directory < rhs.is_directory;
              });

    Sha256 sha;
    std::array<char, 64 * 1024> buffer{};
    for (const auto& entry : entries) {
        const unsigned char entry_type =
            entry.is_directory ? 0u : 1u;
        sha.update(&entry_type, 1);
        sha256_update_u64(sha,
                          static_cast<std::uint64_t>(
                              entry.relative_path.size()));
        sha.update(entry.relative_path);
        if (entry.is_directory) continue;

        std::error_code size_ec;
        const std::uintmax_t size = fs::file_size(entry.path, size_ec);
        if (size_ec || size > UINT64_MAX) {
            error = "failed to read seed file size: " +
                    path_to_utf8_generic(entry.path);
            return std::nullopt;
        }
        sha256_update_u64(sha, static_cast<std::uint64_t>(size));

        std::ifstream ifs(entry.path, std::ios::binary);
        if (!ifs.is_open()) {
            error = "failed to open seed file: " +
                    path_to_utf8_generic(entry.path);
            return std::nullopt;
        }
        while (ifs) {
            ifs.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = ifs.gcount();
            if (count > 0) {
                sha.update(
                    reinterpret_cast<const unsigned char*>(buffer.data()),
                    static_cast<std::size_t>(count));
            }
        }
        if (ifs.bad()) {
            error = "failed to hash seed file: " +
                    path_to_utf8_generic(entry.path);
            return std::nullopt;
        }
    }

    return sha.final_hex();
}

bool directory_contains_only_skill_md(const fs::path& directory) {
    std::error_code ec;
    std::size_t file_count = 0;
    fs::recursive_directory_iterator it(
        directory, fs::directory_options::none, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code status_ec;
        const fs::file_status status = it->symlink_status(status_ec);
        if (status_ec || fs::is_symlink(status)) return false;
        if (fs::is_regular_file(status)) {
            std::error_code relative_ec;
            const fs::path relative =
                fs::relative(it->path(), directory, relative_ec);
            if (relative_ec ||
                path_to_utf8_generic(relative) != "SKILL.md") {
                return false;
            }
            ++file_count;
        } else if (!fs::is_directory(status)) {
            return false;
        }
        it.increment(ec);
    }
    return !ec && file_count == 1;
}

std::unordered_map<std::string, PreviousSeedState> read_previous_seed_state(
    const fs::path& state_path) {
    std::unordered_map<std::string, PreviousSeedState> previous;
    std::error_code ec;
    if (!fs::is_regular_file(state_path, ec)) return previous;

    try {
        std::ifstream ifs(state_path, std::ios::binary);
        if (!ifs.is_open()) return previous;
        const nlohmann::json state = nlohmann::json::parse(ifs);
        const auto skills_it = state.find("skills");
        if (skills_it == state.end() || !skills_it->is_array()) {
            return previous;
        }
        for (const auto& item : *skills_it) {
            if (!item.is_object()) continue;
            const std::string relative_path =
                item.value("relative_path", std::string{});
            if (relative_path.empty()) continue;

            PreviousSeedState entry;
            entry.result = item.value("result", std::string{});
            entry.installed_tree_sha256 =
                item.value("installed_tree_sha256", std::string{});
            entry.skill_md_hash =
                item.value("skill_md_hash", std::string{});
            if (item.contains("acecode_owned") &&
                item["acecode_owned"].is_boolean()) {
                entry.acecode_owned = item["acecode_owned"].get<bool>();
            } else {
                entry.acecode_owned =
                    entry.result == "installed" ||
                    entry.result == "updated" ||
                    entry.result == "unchanged";
            }
            previous[relative_path] = std::move(entry);
        }
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[skills] Ignoring unreadable legacy seed state: ") +
                 e.what());
        previous.clear();
    }
    return previous;
}

bool previous_state_proves_pristine(const PreviousSeedState* previous,
                                    const fs::path& target_dir,
                                    const std::string& target_tree_sha256) {
    if (!previous || !previous->acecode_owned) return false;
    if (!previous->installed_tree_sha256.empty()) {
        return previous->installed_tree_sha256 == target_tree_sha256;
    }
    if (previous->skill_md_hash.empty() ||
        !directory_contains_only_skill_md(target_dir)) {
        return false;
    }
    auto current_hash = hash_file_fnv1a64(target_dir / "SKILL.md");
    return current_hash && *current_hash == previous->skill_md_hash;
}

bool copy_tree_no_overwrite(const fs::path& source_dir,
                            const fs::path& target_dir,
                            std::string& error) {
    std::error_code ec;
    if (fs::exists(target_dir, ec)) {
        error = "target_exists";
        return false;
    }
    fs::create_directories(target_dir, ec);
    if (ec) {
        error = "create_target_failed: " + ec.message();
        return false;
    }

    fs::recursive_directory_iterator it(
        source_dir, fs::directory_options::none, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code status_ec;
        const fs::file_status status = it->symlink_status(status_ec);
        if (status_ec) {
            error = "inspect_source_failed: " + status_ec.message();
            return false;
        }
        if (fs::is_symlink(status)) {
            error = "source_symlink_not_supported";
            return false;
        }

        std::error_code relative_ec;
        const fs::path relative =
            fs::relative(it->path(), source_dir, relative_ec);
        if (relative_ec) {
            error = "relative_path_failed: " + relative_ec.message();
            return false;
        }
        const fs::path target = target_dir / relative;

        if (fs::is_directory(status)) {
            fs::create_directories(target, ec);
            if (ec) {
                error = "create_directory_failed: " + ec.message();
                return false;
            }
        } else if (fs::is_regular_file(status)) {
            if (target.has_parent_path()) {
                fs::create_directories(target.parent_path(), ec);
                if (ec) {
                    error = "create_parent_failed: " + ec.message();
                    return false;
                }
            }
            fs::copy_file(it->path(), target, fs::copy_options::none, ec);
            if (ec) {
                error = "copy_file_failed: " + ec.message();
                return false;
            }
        } else {
            error = "unsupported_source_file_type";
            return false;
        }
        it.increment(ec);
    }
    if (ec) {
        error = "scan_source_failed: " + ec.message();
        return false;
    }
    return true;
}

bool stage_seed_directory(const fs::path& source_dir,
                          const fs::path& stage_dir,
                          const std::string& source_tree_sha256,
                          std::string& error) {
    std::error_code ec;
    fs::remove_all(stage_dir, ec);
    if (ec) {
        error = "remove_staging_failed: " + ec.message();
        return false;
    }
    if (!copy_tree_no_overwrite(source_dir, stage_dir, error)) return false;

    std::string hash_error;
    auto stage_hash = hash_directory_tree(stage_dir, hash_error);
    if (!stage_hash) {
        error = std::move(hash_error);
        return false;
    }
    if (*stage_hash != source_tree_sha256) {
        error = "staged seed hash mismatch";
        return false;
    }
    return true;
}

void remove_empty_ancestors(fs::path path, const fs::path& stop) {
    std::error_code ec;
    while (!path.empty()) {
        fs::remove(path, ec);
        if (ec) break;
        if (path == stop) break;
        path = path.parent_path();
    }
}

bool recover_interrupted_seed_update(
    const fs::path& acecode_home,
    const fs::path& target_root,
    DefaultSkillSeedInstallResult& result) {
    const fs::path staging_root = acecode_home / kSeedStagingDir;
    const fs::path backup_root = acecode_home / kSeedBackupDir;
    std::error_code ec;

    fs::remove_all(staging_root, ec);
    if (ec) {
        append_error(result, "failed to clean seed staging: " + ec.message());
        return false;
    }

    for (const auto& seed : default_skill_seeds()) {
        const fs::path backup_dir = backup_root / seed.relative_path;
        const fs::path target_dir = target_root / seed.relative_path;
        const bool backup_exists = fs::exists(backup_dir, ec);
        if (ec) {
            append_error(result, "failed to inspect seed backup: " + ec.message());
            return false;
        }
        if (!backup_exists) continue;

        const bool target_exists = fs::exists(target_dir, ec);
        if (ec) {
            append_error(result, "failed to inspect seed target: " + ec.message());
            return false;
        }
        if (target_exists) {
            fs::remove_all(backup_dir, ec);
            if (ec) {
                append_error(result,
                             "failed to remove completed seed backup: " +
                                 ec.message());
                return false;
            }
        } else {
            fs::create_directories(target_dir.parent_path(), ec);
            if (ec) {
                append_error(result,
                             "failed to restore seed backup parent: " +
                                 ec.message());
                return false;
            }
            fs::rename(backup_dir, target_dir, ec);
            if (ec) {
                append_error(result,
                             "failed to restore interrupted seed update: " +
                                 ec.message());
                return false;
            }
        }
        remove_empty_ancestors(backup_dir.parent_path(), backup_root);
    }
    return true;
}

bool publish_staged_seed(const fs::path& stage_dir,
                         const fs::path& target_dir,
                         const fs::path& backup_dir,
                         bool update_existing,
                         std::string& error) {
    std::error_code ec;
    fs::create_directories(target_dir.parent_path(), ec);
    if (ec) {
        error = "create_target_parent_failed: " + ec.message();
        return false;
    }

    if (!update_existing) {
        fs::rename(stage_dir, target_dir, ec);
        if (ec) {
            error = "publish_seed_failed: " + ec.message();
            return false;
        }
        return true;
    }

    fs::create_directories(backup_dir.parent_path(), ec);
    if (ec) {
        error = "create_backup_parent_failed: " + ec.message();
        return false;
    }
    fs::rename(target_dir, backup_dir, ec);
    if (ec) {
        error = "backup_existing_seed_failed: " + ec.message();
        return false;
    }

    fs::rename(stage_dir, target_dir, ec);
    if (ec) {
        const std::string publish_error = ec.message();
        std::error_code restore_ec;
        fs::rename(backup_dir, target_dir, restore_ec);
        error = "publish_updated_seed_failed: " + publish_error;
        if (restore_ec) {
            error += "; restore_failed: " + restore_ec.message();
        }
        return false;
    }

    fs::remove_all(backup_dir, ec);
    if (ec) {
        LOG_WARN("[skills] Failed to remove seed update backup: " +
                 ec.message());
    }
    return true;
}

bool write_seed_state(
    DefaultSkillSeedInstallResult& result,
    bool completed,
    const std::unordered_map<std::string, PreviousSeedState>& previous) {
    nlohmann::json state;
    state["schema_version"] = 2;
    state["bundle_version"] = result.bundle_version;
    state["completed"] = completed;
    state["seed_skills_dir"] =
        path_to_utf8_generic(result.seed_skills_dir);
    state["target_root"] = path_to_utf8_generic(result.target_root);
    state["generated_at_unix"] =
        static_cast<long long>(std::time(nullptr));
    state["skills"] = nlohmann::json::array();

    for (const auto& outcome : result.outcomes) {
        nlohmann::json item;
        item["name"] = outcome.name;
        item["source_id"] = outcome.source_id;
        item["relative_path"] = outcome.relative_path;
        item["result"] = outcome.result;

        bool acecode_owned = outcome.acecode_owned;
        std::string installed_tree_sha256 =
            outcome.installed_tree_sha256;
        std::string legacy_skill_md_hash;
        if (outcome.result == "error" ||
            outcome.result == "missing_source") {
            const auto previous_it =
                previous.find(outcome.relative_path);
            if (previous_it != previous.end() &&
                previous_it->second.acecode_owned) {
                acecode_owned = true;
                if (installed_tree_sha256.empty()) {
                    installed_tree_sha256 =
                        previous_it->second.installed_tree_sha256;
                }
                legacy_skill_md_hash =
                    previous_it->second.skill_md_hash;
            }
        }

        item["acecode_owned"] = acecode_owned;
        if (!outcome.message.empty()) item["message"] = outcome.message;
        if (!outcome.source_tree_sha256.empty()) {
            item["source_tree_sha256"] = outcome.source_tree_sha256;
        }
        if (!installed_tree_sha256.empty()) {
            item["installed_tree_sha256"] =
                installed_tree_sha256;
        }
        if (!legacy_skill_md_hash.empty()) {
            item["skill_md_hash"] = legacy_skill_md_hash;
        }
        state["skills"].push_back(std::move(item));
    }

    if (!atomic_write_file(
            path_to_utf8(result.state_path), state.dump(2) + "\n")) {
        append_error(result, "failed to atomically write seed state");
        return false;
    }
    result.state_written = true;
    return true;
}

class SeedUpdateFileLock {
public:
    explicit SeedUpdateFileLock(const fs::path& acecode_home) {
        std::error_code ec;
        fs::create_directories(acecode_home, ec);
        if (ec) {
            throw std::runtime_error(
                "failed to create ACECode home for seed lock: " +
                ec.message());
        }
        const fs::path lock_path = acecode_home / kSeedUpdateLockFile;

#ifdef _WIN32
        handle_ = ::CreateFileW(
            lock_path.wstring().c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error(
                "failed to open seed update lock: error " +
                std::to_string(::GetLastError()));
        }

        OVERLAPPED overlapped{};
        if (!::LockFileEx(
                handle_,
                LOCKFILE_EXCLUSIVE_LOCK,
                0,
                MAXDWORD,
                MAXDWORD,
                &overlapped)) {
            const DWORD error = ::GetLastError();
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "failed to acquire seed update lock: error " +
                std::to_string(error));
        }
#else
        fd_ = ::open(
            path_to_utf8(lock_path).c_str(),
            O_CREAT | O_RDWR,
            S_IRUSR | S_IWUSR);
        if (fd_ < 0) {
            throw std::runtime_error(
                "failed to open seed update lock: " +
                std::error_code(errno, std::generic_category()).message());
        }
        while (::flock(fd_, LOCK_EX) != 0) {
            if (errno == EINTR) continue;
            const std::string error =
                std::error_code(errno, std::generic_category()).message();
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(
                "failed to acquire seed update lock: " + error);
        }
#endif
    }

    SeedUpdateFileLock(const SeedUpdateFileLock&) = delete;
    SeedUpdateFileLock& operator=(const SeedUpdateFileLock&) = delete;

    ~SeedUpdateFileLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED overlapped{};
            ::UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
            ::CloseHandle(handle_);
        }
#else
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

} // namespace

const std::vector<DefaultSkillSeed>& default_skill_seeds() {
    static const std::vector<DefaultSkillSeed> seeds = {
        {"find-skills",
         "claude-code-haha:find-skills@76d21ddf33ef7927294cdc019b83b6d263a19ac6",
         fs::path("skill-management") / "find-skills"},
        {"skill-installer",
         "codex-system:skill-installer@2026-04-30",
         fs::path("skill-management") / "skill-installer"},
        {"skill-creator",
         "codex-system:skill-creator@2026-04-30",
         fs::path("skill-management") / "skill-creator"},
        {"native-mcp",
         "hermes-agent:mcp/native-mcp@4eecaf06e48834e105cbd989ae0bae5a2a618c1d",
         fs::path("mcp") / "native-mcp"},
        {"mcporter",
         "hermes-agent:mcp/mcporter@4eecaf06e48834e105cbd989ae0bae5a2a618c1d",
         fs::path("mcp") / "mcporter"},
        {"acecode-tui-usage",
         "acecode:acecode-tui-usage@2026-07-20",
         fs::path("acecode") / "acecode-tui-usage"},
        {"acecode-desktop-usage",
         "acecode:acecode-desktop-usage@2026-07-20",
         fs::path("acecode") / "acecode-desktop-usage"},
        {"vision-image-reader",
         "acecode:vision-image-reader@2026-05-28",
         fs::path("acecode") / "vision-image-reader"},
    };
    return seeds;
}

std::optional<fs::path> find_default_skill_seed_dir(
    const std::string& argv0_dir) {
    const std::string env = getenv_utf8("ACECODE_SEED_SKILLS_DIR");
    if (!env.empty()) {
        if (auto found = valid_seed_dir(path_from_utf8(env))) return found;
    }

    if (!argv0_dir.empty()) {
        const fs::path install_candidate =
            path_from_utf8(argv0_dir) / ".." / "share" /
            "acecode" / "seed" / "skills";
        if (auto found = valid_seed_dir(install_candidate)) return found;

        fs::path probe = path_from_utf8(argv0_dir);
        for (int i = 0; i < 5; ++i) {
            const fs::path dev_candidate =
                probe / "assets" / "seed" / "skills";
            if (auto found = valid_seed_dir(dev_candidate)) return found;
            const fs::path parent = probe.parent_path();
            if (parent == probe) break;
            probe = parent;
        }
    }

#ifndef _WIN32
    if (auto found =
            valid_seed_dir(fs::path("/usr/share/acecode/seed/skills"))) {
        return found;
    }
#endif

    return std::nullopt;
}

fs::path default_skill_seed_state_path(const fs::path& acecode_home) {
    return acecode_home / kSeedStateFile;
}

fs::path default_skill_seed_version_path(const fs::path& acecode_home) {
    return acecode_home / kSeedVersionFile;
}

fs::path packaged_default_skill_seed_version_path(
    const fs::path& seed_skills_dir) {
    return seed_skills_dir.parent_path() / kSeedVersionFile;
}

DefaultSkillSeedInstallResult reconcile_default_global_skills(
    const fs::path& acecode_home,
    const fs::path& seed_skills_dir) {
    DefaultSkillSeedInstallResult result;
    result.seed_skills_dir = normalized_path(seed_skills_dir);
    result.target_root = acecode_home / "skills";
    result.state_path = default_skill_seed_state_path(acecode_home);
    result.version_path = default_skill_seed_version_path(acecode_home);

    std::lock_guard<std::mutex> process_lock(g_seed_reconciliation_mutex);
    try {
        SeedUpdateFileLock file_lock(acecode_home);

        if (!recover_interrupted_seed_update(
                acecode_home, result.target_root, result)) {
            result.attempted = true;
            return result;
        }

        std::string bundled_version_error;
        auto bundled_version = read_seed_version(
            packaged_default_skill_seed_version_path(result.seed_skills_dir),
            bundled_version_error);
        if (!bundled_version) {
            result.attempted = true;
            append_error(result, std::move(bundled_version_error));
            return result;
        }
        result.bundle_version = bundled_version->text;

        std::optional<ParsedSeedVersion> user_version;
        std::error_code version_exists_ec;
        if (fs::is_regular_file(result.version_path, version_exists_ec)) {
            std::string user_version_error;
            user_version =
                read_seed_version(result.version_path, user_version_error);
            if (user_version) {
                result.user_version = user_version->text;
            } else {
                LOG_WARN("[skills] " + user_version_error +
                         "; reconciling seed bundle");
            }
        }

        if (user_version) {
            const int comparison =
                compare_seed_versions(*user_version, *bundled_version);
            if (comparison == 0) return result;
            if (comparison > 0) {
                result.downgrade_skipped = true;
                return result;
            }
        }

        result.attempted = true;
        std::error_code ec;
        fs::create_directories(result.target_root, ec);
        if (ec) {
            append_error(
                result,
                "failed to create global skills root: " + ec.message());
            return result;
        }

        const auto previous =
            read_previous_seed_state(result.state_path);
        const fs::path staging_root = acecode_home / kSeedStagingDir;
        const fs::path backup_root = acecode_home / kSeedBackupDir;
        bool completed = true;

        for (const auto& seed : default_skill_seeds()) {
            DefaultSkillSeedOutcome outcome;
            outcome.name = seed.name;
            outcome.source_id = seed.source_id;
            outcome.relative_path =
                path_to_utf8_generic(seed.relative_path);

            const fs::path source_dir =
                result.seed_skills_dir / seed.relative_path;
            const fs::path source_md = source_dir / "SKILL.md";
            const fs::path target_dir =
                result.target_root / seed.relative_path;
            const fs::path stage_dir =
                staging_root / seed.relative_path;
            const fs::path backup_dir =
                backup_root / seed.relative_path;

            if (!fs::is_regular_file(source_md, ec)) {
                outcome.result = "missing_source";
                outcome.message = path_to_utf8_generic(source_md);
                completed = false;
                append_error(
                    result,
                    "default skill source is missing: " +
                        path_to_utf8_generic(source_md));
                result.outcomes.push_back(std::move(outcome));
                continue;
            }

            std::string source_hash_error;
            auto source_hash =
                hash_directory_tree(source_dir, source_hash_error);
            if (!source_hash) {
                outcome.result = "error";
                outcome.message = source_hash_error;
                completed = false;
                append_error(result, source_hash_error);
                result.outcomes.push_back(std::move(outcome));
                continue;
            }
            outcome.source_tree_sha256 = *source_hash;

            const bool target_exists = fs::exists(target_dir, ec);
            if (ec) {
                outcome.result = "error";
                outcome.message =
                    "failed to inspect target: " + ec.message();
                completed = false;
                append_error(result, outcome.message);
                result.outcomes.push_back(std::move(outcome));
                continue;
            }

            if (!target_exists) {
                std::string stage_error;
                if (!stage_seed_directory(
                        source_dir, stage_dir, *source_hash, stage_error)) {
                    outcome.result = "error";
                    outcome.message = stage_error;
                    completed = false;
                    append_error(result, stage_error);
                    result.outcomes.push_back(std::move(outcome));
                    continue;
                }

                if (fs::exists(target_dir, ec)) {
                    outcome.result = "preserved_user_modified";
                    outcome.message = "target appeared during reconciliation";
                    std::string target_hash_error;
                    if (auto target_hash =
                            hash_directory_tree(
                                target_dir, target_hash_error)) {
                        outcome.installed_tree_sha256 = *target_hash;
                    }
                    fs::remove_all(stage_dir, ec);
                    result.outcomes.push_back(std::move(outcome));
                    continue;
                }

                std::string publish_error;
                if (!publish_staged_seed(
                        stage_dir,
                        target_dir,
                        backup_dir,
                        false,
                        publish_error)) {
                    outcome.result = "error";
                    outcome.message = publish_error;
                    completed = false;
                    append_error(result, publish_error);
                    result.outcomes.push_back(std::move(outcome));
                    continue;
                }
                outcome.result = "installed";
                outcome.acecode_owned = true;
                outcome.installed_tree_sha256 = *source_hash;
                result.outcomes.push_back(std::move(outcome));
                continue;
            }

            std::string target_hash_error;
            auto target_hash =
                hash_directory_tree(target_dir, target_hash_error);
            if (!target_hash) {
                outcome.result = "error";
                outcome.message = target_hash_error;
                completed = false;
                append_error(result, target_hash_error);
                result.outcomes.push_back(std::move(outcome));
                continue;
            }
            outcome.installed_tree_sha256 = *target_hash;

            const auto previous_it =
                previous.find(outcome.relative_path);
            const PreviousSeedState* previous_entry =
                previous_it == previous.end() ? nullptr : &previous_it->second;
            if (!previous_state_proves_pristine(
                    previous_entry, target_dir, *target_hash)) {
                outcome.result = "preserved_user_modified";
                outcome.message =
                    previous_entry
                        ? "installed content differs from recorded seed state"
                        : "existing target is not owned by ACECode";
                result.outcomes.push_back(std::move(outcome));
                continue;
            }

            if (*target_hash == *source_hash) {
                outcome.result = "unchanged";
                outcome.acecode_owned = true;
                result.outcomes.push_back(std::move(outcome));
                continue;
            }

            std::string stage_error;
            if (!stage_seed_directory(
                    source_dir, stage_dir, *source_hash, stage_error)) {
                outcome.result = "error";
                outcome.message = stage_error;
                outcome.acecode_owned = true;
                completed = false;
                append_error(result, stage_error);
                result.outcomes.push_back(std::move(outcome));
                continue;
            }

            std::string publish_error;
            if (!publish_staged_seed(
                    stage_dir,
                    target_dir,
                    backup_dir,
                    true,
                    publish_error)) {
                outcome.result = "error";
                outcome.message = publish_error;
                outcome.acecode_owned = true;
                completed = false;
                append_error(result, publish_error);
                result.outcomes.push_back(std::move(outcome));
                continue;
            }

            outcome.result = "updated";
            outcome.acecode_owned = true;
            outcome.installed_tree_sha256 = *source_hash;
            result.outcomes.push_back(std::move(outcome));
        }

        fs::remove_all(staging_root, ec);
        if (ec) {
            completed = false;
            append_error(
                result, "failed to clean seed staging: " + ec.message());
        }

        if (!write_seed_state(result, completed, previous)) {
            completed = false;
        }
        if (completed && result.state_written) {
            if (!atomic_write_file(
                    path_to_utf8(result.version_path),
                    result.bundle_version + "\n")) {
                append_error(
                    result,
                    "failed to atomically write seed version marker");
            } else {
                result.version_written = true;
            }
        }
    } catch (const std::exception& e) {
        result.attempted = true;
        append_error(result, e.what());
    }
    return result;
}

DefaultSkillSeedInstallResult reconcile_default_global_skills_on_startup(
    const fs::path& acecode_home,
    const std::string& argv0_dir) {
    DefaultSkillSeedInstallResult result;
    result.target_root = acecode_home / "skills";
    result.state_path = default_skill_seed_state_path(acecode_home);
    result.version_path = default_skill_seed_version_path(acecode_home);

    auto seed_dir = find_default_skill_seed_dir(argv0_dir);
    if (!seed_dir) {
        result.attempted = true;
        result.error = "default skill seed bundle not found";
        LOG_WARN(
            "[skills] Default skill seed bundle not found; "
            "skipping startup reconciliation");
        return result;
    }

    return reconcile_default_global_skills(acecode_home, *seed_dir);
}

} // namespace acecode
