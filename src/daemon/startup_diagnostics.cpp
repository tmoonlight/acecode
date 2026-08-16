#include "startup_diagnostics.hpp"

#include "../config/config.hpp"
#include "../utils/utf8_path.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode::daemon {
namespace {

std::string local_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << millis.count();
    return oss.str();
}

std::int64_t startup_elapsed_ms() {
    static const auto started_at = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at).count();
}

} // namespace

void append_startup_diagnostic(const std::string& message) {
    std::error_code ec;
    fs::path run_dir = path_from_utf8(get_run_dir());
    fs::create_directories(run_dir, ec);
    if (ec) return;

    std::ofstream out(run_dir / "daemon-startup.log",
                      std::ios::binary | std::ios::app);
    if (!out.is_open()) return;
    out << local_timestamp() << " elapsed_ms=" << startup_elapsed_ms()
        << " " << message << "\n";
}

} // namespace acecode::daemon
