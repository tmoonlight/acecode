#include "state_file.hpp"

#include "atomic_file.hpp"
#include "logger.hpp"
#include "paths.hpp"
#include "utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
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

// 测试 hook:非空时绕过 resolve_data_dir() 路径,直接用这个绝对路径。
// 通过 set_state_file_path_for_test 设置/清除。
std::string& test_path_override() {
    static std::string p;
    return p;
}

std::mutex& state_file_mutex() {
    static std::mutex mu;
    return mu;
}

bool state_writes_paused = false; // guarded by state_file_mutex()

std::string state_file_path() {
    const auto& override_path = test_path_override();
    if (!override_path.empty()) return override_path;
    return path_to_utf8(path_from_utf8(resolve_data_dir(get_run_mode())) / "state.json");
}

class StateFileWriteLock {
public:
    explicit StateFileWriteLock(const std::string& state_path) {
        if (state_writes_paused) {
            error_ = "state writes paused for data directory migration";
            return;
        }
        const fs::path lock_path = path_from_utf8(state_path + ".lock");
        std::error_code ec;
        if (!lock_path.parent_path().empty()) {
            fs::create_directories(lock_path.parent_path(), ec);
            if (ec) {
                error_ =
                    "failed to create state lock directory: " + ec.message();
                return;
            }
        }

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
            error_ = "failed to open state lock: error " +
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
            error_ = "failed to acquire state lock: error " +
                std::to_string(error);
            return;
        }
#else
        fd_ = ::open(
            path_to_utf8(lock_path).c_str(),
            O_CREAT | O_RDWR,
            S_IRUSR | S_IWUSR);
        if (fd_ < 0) {
            error_ = "failed to open state lock: " +
                std::error_code(errno, std::generic_category()).message();
            return;
        }
        while (::flock(fd_, LOCK_EX) != 0) {
            if (errno == EINTR) continue;
            error_ = "failed to acquire state lock: " +
                std::error_code(errno, std::generic_category()).message();
            ::close(fd_);
            fd_ = -1;
            return;
        }
#endif
        acquired_ = true;
    }

    StateFileWriteLock(const StateFileWriteLock&) = delete;
    StateFileWriteLock& operator=(const StateFileWriteLock&) = delete;

    ~StateFileWriteLock() {
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

// 加载现有 state.json:解析失败 / 文件不存在 → 返回空 object。
// is_corrupted 在文件存在但解析失败时设为 true,让上层知道写回时要覆盖。
nlohmann::json load_state_or_empty(bool* is_corrupted = nullptr) {
    if (is_corrupted) *is_corrupted = false;

    std::string p = state_file_path();
    std::error_code ec;
    auto path = path_from_utf8(p);
    if (!fs::exists(path, ec) || ec) {
        return nlohmann::json::object();
    }
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return nlohmann::json::object();
    }
    std::stringstream buf;
    buf << ifs.rdbuf();
    std::string contents = buf.str();
    if (contents.empty()) {
        return nlohmann::json::object();
    }
    try {
        auto j = nlohmann::json::parse(contents);
        if (!j.is_object()) {
            if (is_corrupted) *is_corrupted = true;
            return nlohmann::json::object();
        }
        return j;
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[state_file] state.json parse failed: ") + e.what() +
                 ", treating as corrupted");
        if (is_corrupted) *is_corrupted = true;
        return nlohmann::json::object();
    }
}

} // namespace

void set_state_file_writes_paused(bool paused) {
    std::lock_guard<std::mutex> lock(state_file_mutex());
    state_writes_paused = paused;
}

bool read_state_flag(const std::string& key) {
    std::lock_guard<std::mutex> lock(state_file_mutex());
    auto j = load_state_or_empty();
    if (!j.contains(key)) return false;
    if (!j[key].is_boolean()) return false;
    return j[key].get<bool>();
}

bool try_write_state_flag(const std::string& key, bool value) {
    std::lock_guard<std::mutex> lock(state_file_mutex());
    const std::string p = state_file_path();
    StateFileWriteLock file_lock(p);
    if (!file_lock.acquired()) {
        LOG_WARN("[state_file] " + file_lock.error());
        return false;
    }
    bool corrupted = false;
    auto j = load_state_or_empty(&corrupted);
    if (corrupted) {
        LOG_WARN("[state_file] state.json corrupted, rewriting");
    }
    j[key] = value;

    std::error_code ec;
    fs::create_directories(path_from_utf8(p).parent_path(), ec);
    // 目录创建失败不致命 — atomic_write_file 失败时再处理。

    if (!atomic_write_file(p, j.dump(2))) {
        LOG_WARN("[state_file] failed to write " + p);
        return false;
    }
    return true;
}

StateFlagClaimResult try_claim_state_flag(const std::string& key) {
    std::lock_guard<std::mutex> lock(state_file_mutex());
    const std::string p = state_file_path();
    StateFileWriteLock file_lock(p);
    if (!file_lock.acquired()) {
        LOG_WARN("[state_file] " + file_lock.error());
        return {false, false};
    }
    bool corrupted = false;
    auto j = load_state_or_empty(&corrupted);
    if (corrupted) {
        LOG_WARN("[state_file] state.json corrupted, rewriting");
    }
    if (j.contains(key) && j[key].is_boolean() &&
        j[key].get<bool>()) {
        return {false, true};
    }

    j[key] = true;
    std::error_code ec;
    fs::create_directories(path_from_utf8(p).parent_path(), ec);
    if (!atomic_write_file(p, j.dump(2))) {
        LOG_WARN("[state_file] failed to claim flag '" + key +
                 "' in " + p);
        return {false, false};
    }
    return {true, true};
}

void write_state_flag(const std::string& key, bool value) {
    (void)try_write_state_flag(key, value);
}

void set_state_file_path_for_test(const std::string& path) {
    std::lock_guard<std::mutex> lock(state_file_mutex());
    test_path_override() = path;
}

nlohmann::json read_state_json() {
    std::lock_guard<std::mutex> lock(state_file_mutex());
    return load_state_or_empty();
}

bool update_state_json(const std::function<bool(nlohmann::json& state)>& mutate) {
    std::lock_guard<std::mutex> lock(state_file_mutex());
    const std::string path = state_file_path();
    StateFileWriteLock file_lock(path);
    if (!file_lock.acquired()) {
        LOG_WARN("[state_file] " + file_lock.error());
        return false;
    }
    bool corrupted = false;
    auto state = load_state_or_empty(&corrupted);
    if (corrupted) {
        LOG_WARN("[state_file] state.json corrupted, rewriting");
    }
    if (!mutate(state)) return true;

    std::error_code ec;
    fs::create_directories(path_from_utf8(path).parent_path(), ec);
    if (!atomic_write_file(path, state.dump(2))) {
        LOG_WARN("[state_file] failed to write " + path);
        return false;
    }
    return true;
}

} // namespace acecode
