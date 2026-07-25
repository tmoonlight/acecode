#include "config_mutation.hpp"

#include "../utils/utf8_path.hpp"

#include <cerrno>
#include <filesystem>
#include <mutex>
#include <stdexcept>
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

namespace acecode {
namespace {

std::mutex g_config_mutation_mutex;

std::string resolved_config_path(const std::string& explicit_path) {
    if (!explicit_path.empty()) return explicit_path;
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / "config.json");
}

class ConfigFileLock {
public:
    explicit ConfigFileLock(const std::string& config_path) {
        const std::filesystem::path lock_path =
            path_from_utf8(config_path + ".lock");
        std::error_code ec;
        if (!lock_path.parent_path().empty()) {
            std::filesystem::create_directories(lock_path.parent_path(), ec);
            if (ec) {
                throw std::runtime_error(
                    "failed to create config lock directory: " + ec.message());
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
            throw std::runtime_error(
                "failed to open config lock: error " +
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
                "failed to acquire config lock: error " +
                std::to_string(error));
        }
#else
        fd_ = ::open(
            path_to_utf8(lock_path).c_str(),
            O_CREAT | O_RDWR,
            S_IRUSR | S_IWUSR);
        if (fd_ < 0) {
            throw std::runtime_error(
                "failed to open config lock: " +
                std::error_code(errno, std::generic_category()).message());
        }
        while (::flock(fd_, LOCK_EX) != 0) {
            if (errno == EINTR) continue;
            const std::string error =
                std::error_code(errno, std::generic_category()).message();
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("failed to acquire config lock: " + error);
        }
#endif
    }

    ConfigFileLock(const ConfigFileLock&) = delete;
    ConfigFileLock& operator=(const ConfigFileLock&) = delete;

    ~ConfigFileLock() {
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

ConfigMutationResult mutate_config(
    const ConfigMutator& mutator,
    const std::string& explicit_path,
    const AppConfig* seed_if_missing) {
    ConfigMutationResult result;
    if (!mutator) {
        result.error_kind = ConfigMutationErrorKind::Validation;
        result.error = "config mutator is empty";
        return result;
    }

    try {
        std::lock_guard<std::mutex> process_lock(g_config_mutation_mutex);
        const std::string config_path = resolved_config_path(explicit_path);
        ConfigFileLock file_lock(config_path);

        std::error_code exists_error;
        const bool config_exists =
            std::filesystem::exists(path_from_utf8(config_path), exists_error) &&
            !exists_error;
        AppConfig latest = !config_exists && seed_if_missing
            ? *seed_if_missing
            : load_config_from_path(config_path, false);
        std::string validation_error;
        const bool changed = mutator(latest, validation_error);
        if (!validation_error.empty()) {
            result.error_kind = ConfigMutationErrorKind::Validation;
            result.error = std::move(validation_error);
            return result;
        }

        if (changed) {
            save_config(latest, config_path);
        }
        result.ok = true;
        result.changed = changed;
        result.config = std::move(latest);
        return result;
    } catch (const std::exception& e) {
        result.error_kind = ConfigMutationErrorKind::Persistence;
        result.error = e.what();
        return result;
    }
}

} // namespace acecode
