#include "paths.hpp"

#include "atomic_file.hpp"
#include "constants.hpp"
#include "encoding.hpp"
#include "logger.hpp"
#include "utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace acecode {

std::string expand_path(const std::string& raw) {
    if (raw.empty()) return raw;

    std::string result;
    result.reserve(raw.size());
    size_t i = 0;

    // ${VAR} substitution first so later ~ expansion sees final text.
    while (i < raw.size()) {
        if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
            size_t end = raw.find('}', i + 2);
            if (end == std::string::npos) {
                result.push_back(raw[i++]);
                continue;
            }
            std::string var_name = raw.substr(i + 2, end - (i + 2));
            std::string val = getenv_utf8(var_name.c_str());
            if (!val.empty()) {
                result.append(val);
            } else {
                result.append(raw, i, end + 1 - i); // leave ${VAR} untouched
            }
            i = end + 1;
        } else {
            result.push_back(raw[i++]);
        }
    }

    if (!result.empty() && result.front() == '~') {
#ifdef _WIN32
        std::string home = getenv_utf8("USERPROFILE");
#else
        std::string home = getenv_utf8("HOME");
#endif
        if (!home.empty()) {
            result = home + result.substr(1);
        }
    }

    return result;
}

std::vector<std::string> get_project_dirs_up_to_home(const std::string& cwd) {
    namespace fs = std::filesystem;
    std::vector<std::string> dirs;
    if (cwd.empty()) return dirs;

    std::error_code ec;
    fs::path abs = fs::absolute(path_from_utf8(cwd), ec).lexically_normal();
    if (ec || abs.empty()) abs = path_from_utf8(cwd).lexically_normal();

    fs::path home_path;
#ifdef _WIN32
    std::string home_env = getenv_utf8("USERPROFILE");
#else
    std::string home_env = getenv_utf8("HOME");
#endif
    if (!home_env.empty()) {
        std::error_code hec;
        home_path = fs::absolute(path_from_utf8(home_env), hec).lexically_normal();
        if (hec) home_path = path_from_utf8(home_env).lexically_normal();
    }

    // Walk up from cwd; stop at/above HOME (the user-global root is added
    // separately) or once we hit a filesystem root. Deepest first.
    //
    // HOME 比较除了字面相等还做一次物理等价判定(fs::equivalent):subst /
    // 网络映射盘会让同一物理目录出现两个字面路径(如 N:\Users\shao ↔
    // C:\Users\shao)。cwd 走映射盘视图时若只做字符串比较,项目链会越过
    // HOME 一路冲到盘根,把 HOME 级目录(~/.claude 等)全部卷成"项目链",
    // 技能/项目指令的 project vs global 归类随之全错。
    fs::path cur = abs;
    while (true) {
        if (!home_path.empty()) {
            if (cur == home_path) break;
            std::error_code eqec;
            if (fs::equivalent(cur, home_path, eqec) && !eqec) break;
        }
        dirs.push_back(path_to_utf8(cur));
        fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return dirs;
}

namespace {

// 进程级单例。RunMode 用 atomic 是因为 set_run_mode 可能在 ServiceMain 早期由
// SCM 线程调,而 get_run_mode 在 worker 线程读 — 不加保护理论上算 data race。
std::atomic<RunMode> g_mode{RunMode::User};
std::atomic<bool>    g_set_once{false};

// run_dir override:string 不能 atomic,加 mutex。daemon 启动早期 set 一次,
// 之后所有 get_run_dir() 调都读这个值;频率不高,锁开销可忽略。
std::mutex  g_run_dir_mu;
std::string g_run_dir_override;

} // namespace

void set_run_mode(RunMode mode) {
    bool expected = false;
    if (g_set_once.compare_exchange_strong(expected, true)) {
        g_mode.store(mode);
        return;
    }
    // 二次调用 — 不改值,只警告。pre-logger-init 时这条 LOG_WARN 会被吞掉,
    // 但生产代码里调到这里几乎肯定是 bug,测试会断到。
    int cur = static_cast<int>(g_mode.load());
    int req = static_cast<int>(mode);
    LOG_WARN(std::string("set_run_mode called more than once; ignoring (current=")
             + std::to_string(cur) + " requested=" + std::to_string(req) + ")");
}

RunMode get_run_mode() {
    return g_mode.load();
}

std::string resolve_default_data_dir(RunMode mode) {
    namespace fs = std::filesystem;

#if defined(_WIN32)
    if (mode == RunMode::Service) {
        std::string base = getenv_utf8("PROGRAMDATA");
        if (base.empty()) base = "C:\\ProgramData";
        return path_to_utf8(path_from_utf8(base) / "acecode");
    }
    // RunMode::User — 与历史 get_acecode_dir() 行为完全一致
    if (std::string userprofile = getenv_utf8("USERPROFILE"); !userprofile.empty()) {
        return path_to_utf8(path_from_utf8(userprofile) / ".acecode");
    }
    std::string drive = getenv_utf8("HOMEDRIVE");
    std::string path  = getenv_utf8("HOMEPATH");
    if (!drive.empty() && !path.empty()) {
        return path_to_utf8(path_from_utf8(drive + path) / ".acecode");
    }
    return path_to_utf8(path_from_utf8(".") / ".acecode");

#elif defined(__APPLE__)
    if (mode == RunMode::Service) {
        return "/Library/Application Support/acecode";
    }
    std::string home = getenv_utf8("HOME");
    return path_to_utf8(path_from_utf8(home.empty() ? "." : home) / ".acecode");

#else // Linux 或其他 POSIX
    if (mode == RunMode::Service) {
        return "/var/lib/acecode";
    }
    std::string home = getenv_utf8("HOME");
    return path_to_utf8(path_from_utf8(home.empty() ? "." : home) / ".acecode");
#endif
}

// ── 数据目录重定向指针 ─────────────────────────────────────────────────────

std::string data_dir_redirect_path(const std::string& default_dir) {
    return path_to_utf8(path_from_utf8(default_dir) / kDataDirRedirectFileName);
}

std::optional<DataDirRedirect> read_data_dir_redirect(const std::string& default_dir) {
    namespace fs = std::filesystem;
    const fs::path file = path_from_utf8(data_dir_redirect_path(default_dir));
    std::error_code ec;
    if (!fs::is_regular_file(file, ec) || ec) return std::nullopt;
    std::ifstream ifs(file, std::ios::binary);
    if (!ifs.is_open()) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(ifs)),
                           std::istreambuf_iterator<char>());
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;

    DataDirRedirect r;
    if (j.contains("data_dir") && j["data_dir"].is_string()) {
        r.data_dir = j["data_dir"].get<std::string>();
    }
    if (r.data_dir.empty()) return std::nullopt;
    if (j.contains("previous_data_dir") && j["previous_data_dir"].is_string()) {
        r.previous_data_dir = j["previous_data_dir"].get<std::string>();
    }
    if (j.contains("migrated_at_ms") && j["migrated_at_ms"].is_number_integer()) {
        r.migrated_at_ms = j["migrated_at_ms"].get<long long>();
    }
    if (j.contains("previous_size_bytes") && j["previous_size_bytes"].is_number_unsigned()) {
        r.previous_size_bytes = j["previous_size_bytes"].get<unsigned long long>();
    } else if (j.contains("previous_size_bytes") &&
               j["previous_size_bytes"].is_number_integer()) {
        const long long v = j["previous_size_bytes"].get<long long>();
        r.previous_size_bytes = v > 0 ? static_cast<unsigned long long>(v) : 0ULL;
    }
    if (j.contains("cleanup_pending") && j["cleanup_pending"].is_boolean()) {
        r.cleanup_pending = j["cleanup_pending"].get<bool>();
    }
    return r;
}

bool write_data_dir_redirect(const std::string& default_dir, const DataDirRedirect& redirect) {
    if (redirect.data_dir.empty()) return false;
    nlohmann::json j = nlohmann::json::object();
    j["data_dir"] = redirect.data_dir;
    if (!redirect.previous_data_dir.empty()) j["previous_data_dir"] = redirect.previous_data_dir;
    if (redirect.migrated_at_ms != 0) j["migrated_at_ms"] = redirect.migrated_at_ms;
    if (redirect.previous_size_bytes != 0) j["previous_size_bytes"] = redirect.previous_size_bytes;
    j["cleanup_pending"] = redirect.cleanup_pending;
    return atomic_write_file(data_dir_redirect_path(default_dir), j.dump(2) + "\n");
}

bool remove_data_dir_redirect(const std::string& default_dir) {
    std::error_code ec;
    std::filesystem::remove(path_from_utf8(data_dir_redirect_path(default_dir)), ec);
    return !ec;
}

namespace {

// resolve_data_dir 的进程级缓存:按 RunMode 各一格,记住当时的默认目录;默认
// 目录变了(测试改 HOME)就重新解析,否则进程内只读一次指针文件。
struct DataDirCacheEntry {
    std::string default_dir;
    std::string resolved;
    bool warned = false;  // 指针无效的 LOG_WARN 只打一次
};
std::mutex g_data_dir_mu;
std::optional<DataDirCacheEntry> g_data_dir_cache[2];
// 待补记的解析告警(见 paths.hpp::take_data_dir_resolution_warning),由
// g_data_dir_mu 保护。只保留首条:同一进程里第一次解析失败的原因最有诊断价值。
std::optional<std::string> g_pending_resolution_warning;

// 调用方持有 g_data_dir_mu。已有待补告警时不覆盖。
void stash_resolution_warning_locked(const std::string& text) {
    if (!g_pending_resolution_warning) g_pending_resolution_warning = text;
}

// 指针目标可用 = 非空、绝对路径、且目录存在。相对路径一律拒绝:进程 cwd 因
// workspace 而异,相对指针会让不同入口解析到不同地方。
bool redirect_target_usable(const std::string& target) {
    namespace fs = std::filesystem;
    if (target.empty()) return false;
    const fs::path p = path_from_utf8(target);
    if (!p.is_absolute()) return false;
    std::error_code ec;
    return fs::is_directory(p, ec) && !ec;
}

}  // namespace

std::string resolve_data_dir(RunMode mode) {
    const std::string default_dir = resolve_default_data_dir(mode);
    const int slot = (mode == RunMode::Service) ? 1 : 0;

    std::lock_guard<std::mutex> lk(g_data_dir_mu);
    auto& entry = g_data_dir_cache[slot];
    if (entry && entry->default_dir == default_dir) return entry->resolved;

    DataDirCacheEntry fresh;
    fresh.default_dir = default_dir;
    fresh.resolved = default_dir;
    const bool already_warned = entry && entry->warned;
    if (auto redirect = read_data_dir_redirect(default_dir)) {
        if (redirect_target_usable(redirect->data_dir)) {
            std::string normalized = path_to_utf8(
                path_from_utf8(redirect->data_dir).lexically_normal());
            // 去掉尾部分隔符("D:\data\" → "D:\data"),盘符根目录除外。
            while (normalized.size() > 3 &&
                   (normalized.back() == '\\' || normalized.back() == '/')) {
                normalized.pop_back();
            }
            fresh.resolved = normalized;
        } else {
            if (!already_warned) {
                const std::string text =
                    "data-dir redirect target unusable, using default: target=" +
                    redirect->data_dir + " default=" + default_dir;
                // 日志已初始化时(例如运行期重新解析)当场就能记下;启动早期会被
                // Logger 丢掉,由待补告警在 init_with_rotation 之后补记。
                LOG_WARN("[paths] " + text);
                stash_resolution_warning_locked(text);
            }
            fresh.warned = true;
        }
    } else {
        // read 返回 nullopt 有两种情况:指针不存在(正常,静默)和指针存在但读不出
        // / 不是合法 JSON / 缺 data_dir。后者以前完全静默回退默认目录,用户只看到
        // 「迁移后数据不见了」,日志里一个字都没有。
        const std::string pointer = data_dir_redirect_path(default_dir);
        std::error_code ec;
        if (std::filesystem::is_regular_file(path_from_utf8(pointer), ec) && !ec) {
            if (!already_warned) {
                const std::string text =
                    "data-dir redirect pointer unreadable or invalid, using default: file=" +
                    pointer;
                LOG_WARN("[paths] " + text);
                stash_resolution_warning_locked(text);
            }
            fresh.warned = true;
        }
    }
    entry = fresh;
    return entry->resolved;
}

std::optional<std::string> take_data_dir_resolution_warning() {
    std::lock_guard<std::mutex> lk(g_data_dir_mu);
    std::optional<std::string> out = std::move(g_pending_resolution_warning);
    g_pending_resolution_warning.reset();
    return out;
}

void log_deferred_data_dir_resolution_warning() {
    // 锁外写日志:take 已经把文本移出,Logger 自己有锁,不必嵌套 g_data_dir_mu。
    if (auto text = take_data_dir_resolution_warning()) {
        LOG_WARN("[paths] (deferred) " + *text);
    }
}

void reset_data_dir_cache_for_test() {
    std::lock_guard<std::mutex> lk(g_data_dir_mu);
    g_data_dir_cache[0].reset();
    g_data_dir_cache[1].reset();
    g_pending_resolution_warning.reset();
}

void set_run_dir_override(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_run_dir_mu);
    g_run_dir_override = path;
}

std::string get_run_dir_override() {
    std::lock_guard<std::mutex> lk(g_run_dir_mu);
    return g_run_dir_override;
}

std::string get_acecode_dir() {
    // 数据目录路径解析全部委托给 paths.cpp,RunMode 决定 User vs Service 根目录
    // (Decision 8)。User 模式行为与历史一致 — TUI / standalone daemon 不受影响。
    return resolve_data_dir(get_run_mode());
}

std::string get_run_dir() {
    // desktop 多 workspace 模式下 daemon 启动时会调 set_run_dir_override,
    // 把 run/ 切到 per-workspace 路径(避免共享 ~/.acecode/run/ 互相覆盖锁文件)。
    auto override_path = get_run_dir_override();
    if (!override_path.empty()) return override_path;
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / constants::SUBDIR_RUN);
}

std::string get_logs_dir() {
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / constants::SUBDIR_LOGS);
}

RunMode override_run_mode_for_test(RunMode mode) {
    RunMode prev = g_mode.load();
    g_mode.store(mode);
    return prev;
}

void reset_run_mode_for_test() {
    g_mode.store(RunMode::User);
    g_set_once.store(false);
    {
        std::lock_guard<std::mutex> lk(g_run_dir_mu);
        g_run_dir_override.clear();
    }
    reset_data_dir_cache_for_test();
}

} // namespace acecode
