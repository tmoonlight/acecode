#include "skills/skill_usage_store.hpp"

#include "utils/atomic_file.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

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

constexpr int kStateVersion = 1;
constexpr std::size_t kMaxStateFileBytes = 1024 * 1024;  // 1 MB

class SkillUsageWriteLock {
public:
    explicit SkillUsageWriteLock(const std::string& state_path) {
        fs::path lock_path;
        try {
            lock_path = path_from_utf8(state_path + ".lock");
        } catch (const std::exception& e) {
            error_ = "failed to resolve skill usage lock path: " +
                std::string(e.what());
            return;
        }

        std::error_code ec;
        if (!lock_path.parent_path().empty()) {
            fs::create_directories(lock_path.parent_path(), ec);
            if (ec) {
                error_ = "failed to create skill usage lock directory: " +
                    ec.message();
                return;
            }
        }

#ifdef _WIN32
        handle_ = ::CreateFileW(
            lock_path.wstring().c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error_ = "failed to open skill usage lock: error " +
                std::to_string(::GetLastError());
            return;
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
            error_ = "failed to acquire skill usage lock: error " +
                std::to_string(error);
            return;
        }
#else
        fd_ = ::open(
            path_to_utf8(lock_path).c_str(),
            O_CREAT | O_RDWR,
            S_IRUSR | S_IWUSR);
        if (fd_ < 0) {
            error_ = "failed to open skill usage lock: " +
                std::error_code(errno, std::generic_category()).message();
            return;
        }
        while (::flock(fd_, LOCK_EX) != 0) {
            if (errno == EINTR) continue;
            error_ = "failed to acquire skill usage lock: " +
                std::error_code(errno, std::generic_category()).message();
            ::close(fd_);
            fd_ = -1;
            return;
        }
#endif
        acquired_ = true;
    }

    SkillUsageWriteLock(const SkillUsageWriteLock&) = delete;
    SkillUsageWriteLock& operator=(const SkillUsageWriteLock&) = delete;

    ~SkillUsageWriteLock() {
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

    bool acquired() const { return acquired_; }
    const std::string& error() const { return error_; }

private:
    bool acquired_ = false;
    std::string error_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

// Read the state file into a JSON object. Returns an empty object when the
// file is absent, oversized, corrupted, or has an unsupported version so the
// feature degrades gracefully instead of failing the caller.
nlohmann::json load_state_or_empty(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return nlohmann::json::object();
    const auto state_size = fs::file_size(path, ec);
    if (ec) return nlohmann::json::object();
    if (state_size > kMaxStateFileBytes) {
        LOG_WARN("[skill_usage] state file too large, ignoring");
        return nlohmann::json::object();
    }
    std::ifstream ifs(path);
    if (!ifs) return nlohmann::json::object();
    try {
        auto j = nlohmann::json::parse(ifs);
        if (!j.is_object() || j.value("version", 0) != kStateVersion) {
            LOG_WARN("[skill_usage] state file version mismatch, resetting");
            return nlohmann::json::object();
        }
        return j;
    } catch (const nlohmann::json::exception& e) {
        LOG_WARN("[skill_usage] state file parse error: " +
                 std::string(e.what()));
        return nlohmann::json::object();
    }
}

std::uint64_t uint64_member_or(const nlohmann::json& object,
                               const char* key,
                               std::uint64_t fallback = 0) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    if (it == object.end()) return fallback;
    if (it->is_number_unsigned()) {
        return it->get<std::uint64_t>();
    }
    if (it->is_number_integer()) {
        const auto value = it->get<std::int64_t>();
        return value >= 0 ? static_cast<std::uint64_t>(value) : fallback;
    }
    return fallback;
}

bool bool_member_or(const nlohmann::json& object,
                    const char* key,
                    bool fallback = false) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean()
        ? it->get<bool>()
        : fallback;
}

std::string string_member_or(const nlohmann::json& object,
                             const char* key,
                             std::string fallback = {}) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    return it != object.end() && it->is_string()
        ? it->get<std::string>()
        : fallback;
}

nlohmann::json& ensure_skills_object(nlohmann::json& state) {
    auto& skills = state["skills"];
    if (!skills.is_object()) {
        skills = nlohmann::json::object();
    }
    return skills;
}

nlohmann::json& ensure_skill_entry(nlohmann::json& skills,
                                   const std::string& skill_name) {
    auto& entry = skills[skill_name];
    if (!entry.is_object()) {
        entry = nlohmann::json::object();
    }
    return entry;
}

bool parse_decimal_field(const std::string& text,
                         std::size_t offset,
                         std::size_t length,
                         int& value) {
    if (offset + length > text.size()) return false;
    value = 0;
    for (std::size_t i = offset; i < offset + length; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<int>(c - '0');
    }
    return true;
}

bool is_leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int days_in_month(int year, int month) {
    static constexpr int kDays[] = {
        0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap_year(year)) return 29;
    return kDays[month];
}

}  // namespace

SkillUsageStore::SkillUsageStore(std::string state_path)
    : state_path_(std::move(state_path)) {}

bool SkillUsageStore::record(const std::string& skill_name,
                             const std::string& now_iso) {
    std::lock_guard<std::mutex> lock(mu_);
    try {
        SkillUsageWriteLock file_lock(state_path_);
        if (!file_lock.acquired()) {
            LOG_WARN("[skill_usage] " + file_lock.error());
            return false;
        }

        // The state must be re-read after acquiring the process-wide lock;
        // another TUI/daemon instance may have committed while we waited.
        auto state = load_state_or_empty(state_path_);
        state["version"] = kStateVersion;
        auto& skills = ensure_skills_object(state);
        auto& entry = ensure_skill_entry(skills, skill_name);
        const std::uint64_t current = uint64_member_or(entry, "useCount");
        entry["lastUsedAt"] = now_iso;
        entry["useCount"] =
            current == std::numeric_limits<std::uint64_t>::max()
            ? current
            : current + 1;
        if (!entry.contains("pinned") || !entry["pinned"].is_boolean()) {
            entry["pinned"] = false;
        }
        return atomic_write_file(state_path_, state.dump(2));
    } catch (const std::exception& e) {
        LOG_WARN("[skill_usage] record failed: " + std::string(e.what()));
        return false;
    } catch (...) {
        LOG_WARN("[skill_usage] record failed with unknown error");
        return false;
    }
}

bool SkillUsageStore::is_dormant(const std::string& skill_name,
                                 std::int64_t now_epoch_ms,
                                 std::int64_t idle_days_ms) const {
    if (idle_days_ms <= 0) return false;
    std::lock_guard<std::mutex> lock(mu_);
    try {
        const auto state = load_state_or_empty(state_path_);
        const auto skills_it = state.find("skills");
        if (skills_it == state.end() || !skills_it->is_object()) return false;
        const auto entry_it = skills_it->find(skill_name);
        if (entry_it == skills_it->end() || !entry_it->is_object() ||
            bool_member_or(*entry_it, "pinned")) {
            return false;
        }
        const std::string last_used =
            string_member_or(*entry_it, "lastUsedAt");
        const std::int64_t last_ms = parse_iso8601_to_epoch_ms(last_used);
        return last_ms > 0 && now_epoch_ms > last_ms &&
            (now_epoch_ms - last_ms) > idle_days_ms;
    } catch (const std::exception& e) {
        LOG_WARN("[skill_usage] dormancy read failed: " +
                 std::string(e.what()));
        return false;
    } catch (...) {
        LOG_WARN("[skill_usage] dormancy read failed with unknown error");
        return false;
    }
}

bool SkillUsageStore::set_pinned(const std::string& skill_name, bool pinned) {
    std::lock_guard<std::mutex> lock(mu_);
    try {
        SkillUsageWriteLock file_lock(state_path_);
        if (!file_lock.acquired()) {
            LOG_WARN("[skill_usage] " + file_lock.error());
            return false;
        }

        auto state = load_state_or_empty(state_path_);
        state["version"] = kStateVersion;
        auto& skills = ensure_skills_object(state);
        auto& entry = ensure_skill_entry(skills, skill_name);
        if (!entry.contains("lastUsedAt") ||
            !entry["lastUsedAt"].is_string()) {
            entry["lastUsedAt"] = "";
        }
        // Canonicalize malformed and negative counters instead of merely
        // presenting them as zero through get_summary().
        entry["useCount"] = uint64_member_or(entry, "useCount");
        entry["pinned"] = pinned;
        return atomic_write_file(state_path_, state.dump(2));
    } catch (const std::exception& e) {
        LOG_WARN("[skill_usage] set_pinned failed: " +
                 std::string(e.what()));
        return false;
    } catch (...) {
        LOG_WARN("[skill_usage] set_pinned failed with unknown error");
        return false;
    }
}

std::vector<SkillUsageSummary> SkillUsageStore::get_summary(
    std::int64_t now_epoch_ms, std::int64_t idle_days_ms) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SkillUsageSummary> out;
    try {
        const auto state = load_state_or_empty(state_path_);
        const auto skills_it = state.find("skills");
        if (skills_it == state.end() || !skills_it->is_object()) return out;
        for (const auto& [name, entry] : skills_it->items()) {
            if (!entry.is_object()) continue;
            SkillUsageSummary s;
            s.name = name;
            s.use_count = uint64_member_or(entry, "useCount");
            s.last_used_at = string_member_or(entry, "lastUsedAt");
            s.pinned = bool_member_or(entry, "pinned");
            if (idle_days_ms > 0 && !s.pinned && !s.last_used_at.empty()) {
                const std::int64_t last_ms =
                    parse_iso8601_to_epoch_ms(s.last_used_at);
                s.dormant = last_ms > 0 && now_epoch_ms > last_ms &&
                    (now_epoch_ms - last_ms) > idle_days_ms;
            }
            out.push_back(std::move(s));
        }
    } catch (const std::exception& e) {
        LOG_WARN("[skill_usage] summary read failed: " +
                 std::string(e.what()));
    } catch (...) {
        LOG_WARN("[skill_usage] summary read failed with unknown error");
    }
    return out;
}

void SkillUsageStore::reload() {
    std::lock_guard<std::mutex> lock(mu_);
    // The next access re-reads the file via load_state_or_empty; nothing
    // to invalidate here.
}

std::int64_t parse_iso8601_to_epoch_ms(const std::string& iso) {
    // Exact shape: YYYY-MM-DDTHH:MM:SSZ or 1-3 decimal fraction digits.
    if (iso.size() < 20 || iso.size() > 24 ||
        iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' ||
        iso[13] != ':' || iso[16] != ':') {
        return 0;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_decimal_field(iso, 0, 4, year) ||
        !parse_decimal_field(iso, 5, 2, month) ||
        !parse_decimal_field(iso, 8, 2, day) ||
        !parse_decimal_field(iso, 11, 2, hour) ||
        !parse_decimal_field(iso, 14, 2, minute) ||
        !parse_decimal_field(iso, 17, 2, second)) {
        return 0;
    }
    if (year < 1970 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month(year, month) || hour > 23 || minute > 59 ||
        second > 59) {
        return 0;
    }

    int milliseconds = 0;
    if (iso[19] == 'Z') {
        if (iso.size() != 20) return 0;
    } else if (iso[19] == '.') {
        if (iso.back() != 'Z') return 0;
        const std::size_t fraction_digits = iso.size() - 21;
        if (fraction_digits < 1 || fraction_digits > 3 ||
            !parse_decimal_field(iso, 20, fraction_digits, milliseconds)) {
            return 0;
        }
        if (fraction_digits == 1) milliseconds *= 100;
        if (fraction_digits == 2) milliseconds *= 10;
    } else {
        return 0;
    }

    std::tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = 0;
#ifdef _WIN32
    const std::time_t seconds_since_epoch = ::_mkgmtime(&tm);
#else
    const std::time_t seconds_since_epoch = ::timegm(&tm);
#endif
    if (seconds_since_epoch < 0) return 0;
    const auto seconds = static_cast<std::int64_t>(seconds_since_epoch);
    if (seconds >
        (std::numeric_limits<std::int64_t>::max() - milliseconds) / 1000) {
        return 0;
    }
    return seconds * 1000 + milliseconds;
}

}  // namespace acecode
