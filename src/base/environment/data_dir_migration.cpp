#include "utils/joining_thread.hpp"
#include "platform/unique_sqlite.hpp"
#include "data_dir_migration.hpp"

#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"
#include "utils/state_file.hpp"
#include "platform/process/os_process.hpp"
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <system_error>
#include <vector>
#include <set>

namespace acecode::environment {

namespace fs = std::filesystem;

namespace {
std::shared_mutex write_gate;
std::atomic<bool> writes_blocked{false};
}
std::shared_mutex& data_dir_write_mutex() { return write_gate; }
bool data_dir_writes_blocked() { return writes_blocked.load(); }
void reset_data_dir_write_gate_for_test() { writes_blocked.store(false); set_state_file_writes_paused(false); }

std::string migration_os_error_text(const std::error_code& ec) {
    return ensure_utf8(ec.message());
}

std::string describe_daemon_pid_holder(const fs::path& data_root, const fs::path& pid_file,
                                       long long pid) {
    // legacy = 相对数据目录的第一段是 projects(projects/<hash>/run/**)。
    const fs::path relative = pid_file.lexically_relative(data_root);
    const bool legacy = !relative.empty() && path_to_utf8(*relative.begin()) == "projects";
    return "pid=" + std::to_string(pid) + " file=" + path_to_utf8(pid_file) +
           " legacy=" + (legacy ? "yes" : "no");
}

bool data_dir_has_other_daemons(const std::string& directory, std::string* holder) {
    const auto root = path_from_utf8(directory);
    std::vector<fs::path> runs{root / "run"};
    std::error_code ec;
    if (fs::is_directory(root / "projects", ec)) {
        for (const auto& project : fs::directory_iterator(root / "projects", ec)) runs.push_back(project.path() / "run");
    }
    for (const auto& run : runs) {
        if (!fs::is_directory(run, ec)) continue;
        for (fs::recursive_directory_iterator it(run, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                // 扫不清就按「有占用」处理(fail-closed),但要把原因带进拒绝日志。
                if (holder) *holder = "scan error: " + path_to_utf8(run) + ": " + migration_os_error_text(ec);
                return true;
            }
            if (it->is_symlink(ec)) { it.disable_recursion_pending(); continue; }
            if (it->path().filename() != "daemon.pid") continue;
            // 同目录的 pid / heartbeat 一起读,交给 evaluate_daemon_pid_holder 判定;
            // 进程身份与启动时间只对「活着且不是自己」的 pid 才去探测。
            const auto snapshot = daemon::read_runtime_snapshot(path_to_utf8(it->path().parent_path()));
            const std::int64_t pid = snapshot.pid.value_or(0);
            const std::int64_t self = static_cast<std::int64_t>(daemon::current_pid());
            bool alive = false;
            auto identity = daemon::DaemonProcessIdentity::Unknown;
            std::optional<std::int64_t> start_ms;
            if (pid > 0 && pid != self) {
                alive = daemon::is_pid_alive(static_cast<daemon::pid_t_compat>(pid));
                if (alive) {
                    identity = daemon::inspect_daemon_process_identity(pid);
                    start_ms = daemon::process_start_time_ms(pid);
                }
            }
            const auto verdict = evaluate_daemon_pid_holder(snapshot, self, alive, identity, start_ms);
            const std::string described = describe_daemon_pid_holder(root, it->path(), pid);
            if (verdict.blocks) {
                if (holder) *holder = described + " reason=" + verdict.reason;
                return true;
            }
            LOG_INFO("[data-dir] daemon.pid does not block: " + described + " reason=" + verdict.reason);
        }
    }
    return false;
}

DaemonPidHolderVerdict evaluate_daemon_pid_holder(
    const daemon::RuntimeSnapshot& snapshot,
    std::int64_t current_pid,
    bool pid_alive,
    daemon::DaemonProcessIdentity identity,
    const std::optional<std::int64_t>& start_ms) {
    if (!snapshot.pid.has_value() || *snapshot.pid <= 0) return {false, "invalid pid"};
    if (*snapshot.pid == current_pid) return {false, "current process"};
    if (!pid_alive) return {false, "pid not alive"};
    if (daemon::runtime_pid_reuse_is_proven(snapshot, identity, start_ms)) {
        return {false, identity == daemon::DaemonProcessIdentity::Mismatch
                           ? "pid reused: process is not acecode"
                           : "pid reused: process started after heartbeat"};
    }
    return {true, identity == daemon::DaemonProcessIdentity::Match
                      ? "live acecode process"
                      : "process identity unknown"};
}

namespace {

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// 比较键:generic 形态 + 尾部分隔符;Windows 再小写。
std::string prefix_key(const fs::path& p) {
    std::string key = path_to_utf8_generic(p);
    if (key.empty() || key.back() != '/') key.push_back('/');
#ifdef _WIN32
    for (auto& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
#endif
    return key;
}

fs::path canonical_or_normal(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    if (ec || c.empty()) c = p.lexically_normal();
    return c;
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string lower_ascii(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// SQLite 打开路径默认保持展示形态:daemon 自己的长连接(audit_log、state store)用的是
// 普通路径,同一进程再用 `\\?\` 形态打开同一个 WAL 库,SQLite 会按全路径建出两个
// winShmNode,改变现有的并发行为。只有普通形态已经逼近 MAX_PATH 时才换扩展形态;
// 阈值取 240 而不是 260,给 SQLite 追加的 -journal / -wal 后缀(8 字节)与结尾 NUL 留余量。
constexpr std::size_t kSqliteExtendedPathThreshold = 240;

std::string sqlite_open_path(const fs::path& display, const fs::path& io) {
    return path_to_utf8(display.native().size() >= kSqliteExtendedPathThreshold ? io : display);
}

// 运行期普通 API 的 MAX_PATH 上限;迁移后最终路径达到它的文件只统计告警。
constexpr std::size_t kWindowsMaxPath = 260;

// 尽力复制模式下的 SQLite busy_timeout。0912 反馈日志里 live profile 的 backup 都很快
// 完成,短超时只是防止某个被独占锁的库把整次迁移拖住;失败后还有原始复制兜底。
constexpr int kBestEffortSqliteBusyTimeoutMs = 500;
constexpr int kSqliteBusyTimeoutMs = 5000;

// skipped_files 的样例只进日志,留几条足够定位。
constexpr std::size_t kSkippedSampleLimit = 5;

struct MigrationItem {
    fs::path relative;
    bool is_dir = false;
    std::uintmax_t size = 0;
    fs::file_time_type modified{};
    bool is_link = false;
};

bool is_not_found(const std::error_code& ec) {
    return ec == std::errc::no_such_file_or_directory;
}

// 统计一个不迁移、但仍留在旧目录里的条目(排除项、可再生浏览器缓存)的字节数,计入
// 指针的 previous_size_bytes —— 迁移是复制不是移动,这些文件在清理前一直占着旧目录。
// 只求近似:任何错误都跳过那一项,不跟随符号链接,永远不让迁移失败。
std::uintmax_t measure_left_behind_bytes(const fs::path& io_path) {
    std::error_code ec;
    const auto status = fs::symlink_status(io_path, ec);
    if (ec) return 0;
    if (fs::is_regular_file(status)) {
        const auto size = fs::file_size(io_path, ec);
        return ec ? 0 : size;
    }
    if (!fs::is_directory(status)) return 0;
    std::uintmax_t total = 0;
    std::vector<fs::path> pending{io_path};
    while (!pending.empty()) {
        const fs::path dir = pending.back();
        pending.pop_back();
        std::error_code it_ec;
        fs::directory_iterator it(dir, it_ec), end;
        for (; !it_ec && it != end; it.increment(it_ec)) {
            std::error_code type_ec;
            if (it->is_symlink(type_ec)) continue;
            if (it->is_directory(type_ec)) {
                pending.push_back(it->path());
                continue;
            }
            if (!it->is_regular_file(type_ec)) continue;
            const auto size = it->file_size(type_ec);
            if (!type_ec) total += size;
        }
    }
    return total;
}

// 尽力复制子树(agent-browser/webview2)的容错遍历:用栈 + directory_iterator(ec) 手动
// 走,任何一层出错只记日志然后继续。不能交给主枚举的 recursive_directory_iterator:浏览器
// 在枚举中途删掉子目录时 increment 会报错,整个迁移随之失败。结果按 relative 排序,保证
// 父目录在子项之前、主库排在它的 -wal / -shm / -journal 之前(快照后才能跳过旁路文件)。
// 被跳过的可再生缓存与排除项不复制,但其大小累加进 left_behind_bytes。
void collect_best_effort_items(const fs::path& source_io, const fs::path& root_relative,
                               std::vector<MigrationItem>& out,
                               std::uintmax_t& left_behind_bytes) {
    out.push_back({root_relative, true});
    std::vector<fs::path> pending{root_relative};
    while (!pending.empty()) {
        const fs::path dir_relative = pending.back();
        pending.pop_back();
        std::error_code ec;
        fs::directory_iterator it(source_io / dir_relative, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::path relative = dir_relative / it->path().filename();
            if (migration_excludes_entry(relative) ||
                migration_skips_rebuildable_browser_entry(relative)) {
                left_behind_bytes += measure_left_behind_bytes(it->path());
                continue;
            }
            std::error_code type_ec;
            if (it->is_symlink(type_ec)) {
                out.push_back({relative, it->is_directory(type_ec), 0, {}, true});
                continue;
            }
            if (it->is_directory(type_ec)) {
                out.push_back({relative, true});
                pending.push_back(relative);
                continue;
            }
            if (!it->is_regular_file(type_ec)) continue;  // profile 里不该有设备 / 管道,跳过
            const std::uintmax_t size = it->file_size(type_ec);
            if (type_ec) continue;  // 枚举与读元数据之间被删掉了
            out.push_back({relative, false, size});
        }
        if (ec && !is_not_found(ec)) {
            LOG_WARN("[data-dir] best-effort enumeration error: " + path_to_utf8(dir_relative) +
                     ": " + migration_os_error_text(ec));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const MigrationItem& a, const MigrationItem& b) { return a.relative < b.relative; });
}

}  // namespace

std::string make_migration_staging_name() {
    std::string id = generate_uuid();
    id.resize(8);  // uuid 前 8 位恒为 hex,没有分隔符
    return std::string(kMigrationStagingPrefix) + id;
}

const char* migration_target_error_code(MigrationTargetError error) {
    switch (error) {
        case MigrationTargetError::None:            return "OK";
        case MigrationTargetError::NotAbsolute:     return "TARGET_NOT_ABSOLUTE";
        case MigrationTargetError::SameAsCurrent:   return "TARGET_SAME_AS_CURRENT";
        case MigrationTargetError::InsideCurrent:   return "TARGET_INSIDE_CURRENT";
        case MigrationTargetError::ContainsCurrent: return "TARGET_CONTAINS_CURRENT";
        case MigrationTargetError::NotADirectory:   return "TARGET_NOT_A_DIRECTORY";
        case MigrationTargetError::NotEmpty:        return "TARGET_NOT_EMPTY";
        case MigrationTargetError::NotWritable:     return "TARGET_NOT_WRITABLE";
    }
    return "TARGET_INVALID";
}

MigrationTargetCheck validate_migration_target(const std::string& current_dir,
                                               const std::string& target) {
    MigrationTargetCheck out;
    auto fail = [&](MigrationTargetError e, std::string message) {
        out.error = e;
        out.message = std::move(message);
        return out;
    };

    const fs::path raw_target = path_from_utf8(target);
    if (target.empty() || !raw_target.is_absolute()) {
        return fail(MigrationTargetError::NotAbsolute, "target must be an absolute path");
    }
    const fs::path target_c = canonical_or_normal(raw_target);
    const fs::path current_c = canonical_or_normal(path_from_utf8(current_dir));
    out.normalized_target = path_to_utf8(target_c);

    const std::string target_key = prefix_key(target_c);
    const std::string current_key = prefix_key(current_c);
    if (target_key == current_key) {
        return fail(MigrationTargetError::SameAsCurrent, "target is the current data directory");
    }
    if (starts_with(target_key, current_key)) {
        return fail(MigrationTargetError::InsideCurrent,
                    "target is inside the current data directory");
    }
    if (starts_with(current_key, target_key)) {
        return fail(MigrationTargetError::ContainsCurrent,
                    "target contains the current data directory");
    }

    std::error_code ec;
    if (fs::exists(target_c, ec)) {
        if (!fs::is_directory(target_c, ec)) {
            return fail(MigrationTargetError::NotADirectory, "target exists and is not a directory");
        }
        for (const auto& entry : fs::directory_iterator(target_c, ec)) {
            (void)entry;
            return fail(MigrationTargetError::NotEmpty, "target directory is not empty");
        }
        if (ec) {
            return fail(MigrationTargetError::NotWritable,
                        "cannot list target directory: " + migration_os_error_text(ec));
        }
    } else {
        fs::create_directories(target_c, ec);
        if (ec) {
            return fail(MigrationTargetError::NotWritable,
                        "cannot create target directory: " + migration_os_error_text(ec));
        }
    }

    // 真写一个探针文件确认可写(只读盘 / 权限不足在这里暴露,而不是复制到一半)。
    const fs::path probe = target_c / ".acecode-write-probe";
    {
        std::ofstream ofs(probe, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return fail(MigrationTargetError::NotWritable, "target directory is not writable");
        }
        ofs << "probe";
        if (!ofs) {
            ofs.close();
            fs::remove(probe, ec);
            return fail(MigrationTargetError::NotWritable, "target directory is not writable");
        }
    }
    fs::remove(probe, ec);
    return out;
}

bool migration_excludes_entry(const fs::path& relative) {
    if (relative.empty()) return false;
    const std::string first = path_to_utf8(*relative.begin());
    // edge-app-profile:webapp 兼容模式下 Edge --app 的 user-data-dir,每次启动都用全新的
    // PID 子目录并清掉旧的,持续在写又没有要保留的数据,整个排除。
    if (first == "run" || first == "tmp" || first == "edge-app-profile") return true;
    if (first == kDataDirRedirectFileName && std::distance(relative.begin(), relative.end()) == 1) {
        return true;
    }
    // cache/no-workspace/<id>/.acecode/tmp/**:无工作区会话的 ACECODE_TMPDIR(临时脚本、
    // Agent Browser 截图),bash 每次调用按需重建。只锚定这一层:同一会话目录下 Agent 给
    // 用户的产出文件要继续迁移。
    std::string segments[5];
    std::size_t count = 0;
    for (auto part = relative.begin(); part != relative.end() && count < 5; ++part) {
        segments[count++] = path_to_utf8(*part);
    }
    if (count == 5 && segments[0] == "cache" && segments[1] == "no-workspace" &&
        segments[3] == ".acecode" && segments[4] == "tmp") {
        return true;
    }
    const std::string name = lower_ascii(path_to_utf8(relative.filename()));
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".lock") == 0) return true;
    return false;
}

bool migration_is_best_effort_root(const fs::path& relative) {
    return path_to_utf8_generic(relative) == "agent-browser/webview2";
}

bool migration_skips_rebuildable_browser_entry(const fs::path& relative) {
    static const char* const kRebuildableDirs[] = {
        "cache", "code cache", "gpucache", "grshadercache", "graphitedawncache", "dawncache",
        "dawngraphitecache", "dawnwebgpucache", "shadercache", "crashpad",
    };
    for (const auto& part : relative) {
        const std::string segment = lower_ascii(path_to_utf8(part));
        for (const char* dir : kRebuildableDirs) {
            if (segment == dir) return true;
        }
    }
    const std::string name = lower_ascii(path_to_utf8(relative.filename()));
    return name == "lockfile" || name == "lock";
}

bool migration_is_sqlite_family(const fs::path& relative) {
    const std::string name = lower_ascii(path_to_utf8(relative.filename()));
    auto ends_with = [&](const std::string& s) {
        return name.size() >= s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0;
    };
    // -journal 也归入这一组:排在主库之后复制,主库快照成功时才能按旁路文件跳过它。
    for (const auto* suffix : {".sqlite3", ".sqlite", ".db"}) {
        const std::string base(suffix);
        if (ends_with(base) || ends_with(base + "-wal") || ends_with(base + "-shm") ||
                ends_with(base + "-journal")) return true;
    }
    return false;
}

MigrationProgress run_data_dir_migration(const std::string& current_dir,
                                         const std::string& default_dir,
                                         const std::string& target,
                                         const MigrationProgressFn& on_progress) {
    MigrationProgress progress;
    progress.state = "running";
    progress.target = target;
    progress.started_at_ms = now_ms();
    fs::path owned_staging;

    auto fail = [&](std::string message) {
        progress.state = "failed";
        progress.error = std::move(message);
        progress.finished_at_ms = now_ms();
        // Only delete the private staging directory created by this operation.
        std::error_code rm_ec;
        if (!owned_staging.empty()) fs::remove_all(owned_staging, rm_ec);
        LOG_WARN("[data-dir] migration failed: " + progress.error);
        return progress;
    };

    const auto check = validate_migration_target(current_dir, target);
    if (check.error != MigrationTargetError::None) {
        return fail(std::string(migration_target_error_code(check.error)) + ": " + check.message);
    }
    // 每个路径有两种形态:展示形态(source / dest / final_dest)用于错误文案、指针、日志
    // 与前缀比较;IO 形态(*_io,Windows 上是 `\\?\` 扩展长度路径)只用于文件系统调用,
    // 超过 MAX_PATH 的深层文件才能被枚举 / 复制 / 删除。
    const fs::path source = path_from_utf8(current_dir);
    const fs::path source_io = to_extended_length_path(source);
    {
        std::error_code dir_ec;
        if (!fs::is_directory(source_io, dir_ec)) return fail("source directory does not exist");
    }
    const fs::path final_dest = path_from_utf8(check.normalized_target);
    const fs::path final_io = to_extended_length_path(final_dest);
    const fs::path dest = final_dest.parent_path() / make_migration_staging_name();
    const fs::path dest_io = to_extended_length_path(dest);
    std::error_code staging_error;
    if (!fs::create_directory(dest_io, staging_error) || staging_error) return fail("cannot create migration staging directory");
    owned_staging = dest_io;
    progress.target = check.normalized_target;

    // 第一遍:清点(总字节数供进度条),同时把文件分成普通、sqlite 与尽力复制三组。
    using Item = MigrationItem;
    std::vector<Item> regular;
    std::vector<Item> sqlite;
    std::vector<Item> best_effort;  // agent-browser/webview2 子树,容错遍历得到
    // 最终路径 >= MAX_PATH 的文件:迁移能复制过去,但运行期普通 API 读不到,只告警留痕。
    std::size_t over_max_path_files = 0;
    std::string over_max_path_sample;
    // 不迁移但仍留在旧目录里的字节(run/、tmp/、edge-app-profile/、锁文件、无工作区会话
    // 临时目录、浏览器可再生缓存)。指针的 previous_size_bytes = 清点总量 + 它:旧目录的
    // 清理提示按 100 MiB 阈值判断,只记复制量会把大块缓存漏掉,该提示的时候不提示。
    std::uintmax_t left_behind_bytes = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(source_io, ec), end;
         it != end; it.increment(ec)) {
        if (ec) return fail("cannot enumerate source: " + migration_os_error_text(ec));
        const fs::path relative = it->path().lexically_relative(source_io);
        if (migration_excludes_entry(relative)) {
            // 指针文件清理时保留,不算旧数据。
            if (relative != fs::path(kDataDirRedirectFileName)) {
                left_behind_bytes += measure_left_behind_bytes(it->path());
            }
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        std::error_code type_ec;
        if (it->is_symlink(type_ec)) {
            regular.push_back({relative, it->is_directory(type_ec), 0, {}, true});
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(type_ec)) {
            if (migration_is_best_effort_root(relative)) {
                it.disable_recursion_pending();
                collect_best_effort_items(source_io, relative, best_effort, left_behind_bytes);
                continue;
            }
            regular.push_back({relative, true, 0});
            continue;
        }
        if (!it->is_regular_file(type_ec)) return fail("source contains an unsupported file: " + path_to_utf8(relative));
        const std::uintmax_t size = it->file_size(type_ec);
        if (type_ec) return fail("cannot read source file metadata");
        Item item{relative, false, size, it->last_write_time(type_ec)};
        if (type_ec) return fail("cannot read source file timestamp");
#ifdef _WIN32
        if (const fs::path final_path = final_dest / relative; final_path.native().size() >= kWindowsMaxPath) {
            if (over_max_path_files++ == 0) over_max_path_sample = path_to_utf8(final_path);
        }
#endif
        if (migration_is_sqlite_family(relative)) sqlite.push_back(item);
        else regular.push_back(item);
        progress.total_bytes += item.size;
    }
    if (ec) return fail("cannot enumerate source: " + migration_os_error_text(ec));
    for (const auto& item : best_effort) {
        if (item.is_dir || item.is_link) continue;
        progress.total_bytes += item.size;
#ifdef _WIN32
        if (const fs::path final_path = final_dest / item.relative; final_path.native().size() >= kWindowsMaxPath) {
            if (over_max_path_files++ == 0) over_max_path_sample = path_to_utf8(final_path);
        }
#endif
    }

    auto report = [&]() {
        if (on_progress) on_progress(progress.copied_bytes, progress.total_bytes);
    };
    report();

    std::set<std::string> snapshots;
    std::vector<std::string> skipped_samples;
    // 复制一个条目,返回空串 = 继续,非空 = 整个迁移失败的原因。
    // best_effort(仅 agent-browser/webview2 子树):源端读不到时不失败 —— not-found(浏览器
    // 在枚举与复制之间删掉了临时文件)视为正常、不计数,其它错误(共享冲突、拒绝访问)计入
    // skipped_files;无论成败都推进进度,进度条能走满。目标端建目录失败仍然整体失败。
    auto copy_one = [&](const Item& item, bool best_effort) -> std::string {
        // from / to 是展示形态(错误文案、符号链接目标计算);*_io 只给文件系统调用。
        const fs::path from = source / item.relative;
        const fs::path to = dest / item.relative;
        const fs::path from_io = source_io / item.relative;
        const fs::path to_io = dest_io / item.relative;
        std::error_code cec;
        auto skip = [&](const std::error_code& why_ec, const std::string& why) -> std::string {
            std::error_code rm_ec;
            fs::remove(to_io, rm_ec);  // 删掉可能留下的半成品
            if (!is_not_found(why_ec)) {
                ++progress.skipped_files;
                if (skipped_samples.size() < kSkippedSampleLimit) {
                    skipped_samples.push_back(path_to_utf8(item.relative) + " (" + why + ")");
                }
            }
            progress.copied_bytes += item.size;
            report();
            return {};
        };
        if (item.is_link) {
            fs::create_directories(to_io.parent_path(), cec);
            auto link = fs::read_symlink(from_io, cec);
            if (cec && best_effort) return skip(cec, migration_os_error_text(cec));
            if (cec) return "cannot read symbolic link: " + path_to_utf8(from);
            auto resolved = canonical_or_normal(link.is_absolute() ? link : from.parent_path() / link);
            const auto source_root = canonical_or_normal(source);
            if (starts_with(prefix_key(resolved), prefix_key(source_root))) {
                resolved = final_dest / resolved.lexically_relative(source_root);
            }
            // 链接内容写展示形态:它会被运行期普通 API 解析,不能带 `\\?\`。
            if (item.is_dir) fs::create_directory_symlink(resolved, to_io, cec);
            else fs::create_symlink(resolved, to_io, cec);
            if (cec && best_effort) return skip(cec, migration_os_error_text(cec));
            if (cec) return "cannot preserve symbolic link: " + path_to_utf8(from) + ": " + migration_os_error_text(cec);
            return {};
        }
        if (item.is_dir) {
            fs::create_directories(to_io, cec);
            if (cec) return "cannot create " + path_to_utf8(to) + ": " + migration_os_error_text(cec);
            return {};
        }
        fs::create_directories(to_io.parent_path(), cec);
        if (cec) return "cannot create " + path_to_utf8(to.parent_path()) + ": " + migration_os_error_text(cec);
        const std::string relative_name = path_to_utf8(item.relative);
        // 主库已快照:它的 -wal / -shm / -journal 一律不复制。快照本身是一致的,旁边再放一个
        // 原样复制的热 journal,SQLite 打开时会把旧页回滚进快照把它写坏。
        for (const auto& database : snapshots) {
            if (relative_name == database + "-wal" || relative_name == database + "-shm" ||
                    relative_name == database + "-journal") {
                progress.copied_bytes += item.size; report(); return {};
            }
        }
        char header[16]{};
        std::ifstream header_stream(from_io, std::ios::binary);
        header_stream.read(header, sizeof(header));
        const bool is_database = header_stream.gcount() == 16 &&
            std::string(header, 16) == std::string("SQLite format 3\0", 16);
        header_stream.close();
        if (is_database) {
            platform::UniqueSqlite input;
            platform::UniqueSqlite output;
            const int opened = sqlite3_open_v2(sqlite_open_path(from, from_io).c_str(), input.put(), SQLITE_OPEN_READONLY, nullptr);
            int result = opened;
            if (opened == SQLITE_OK) result = sqlite3_open(sqlite_open_path(to, to_io).c_str(), output.put());
            if (result == SQLITE_OK) {
                sqlite3_busy_timeout(input.get(), best_effort ? kBestEffortSqliteBusyTimeoutMs : kSqliteBusyTimeoutMs);
                auto* backup = sqlite3_backup_init(output.get(), "main", input.get(), "main");
                if (backup) {
                    result = sqlite3_backup_step(backup, -1);
                    const int finished = sqlite3_backup_finish(backup);
                    if (result == SQLITE_DONE) result = finished;
                } else result = sqlite3_errcode(output.get());
            }
            if (output) output.reset();
            if (input) input.reset();
            if (result == SQLITE_OK) {
                snapshots.insert(relative_name);
            } else if (best_effort) {
                // 快照失败 → 删掉半成品,退回原始复制(不记快照,旁路文件随后照常复制)。
                std::error_code rm_ec;
                fs::remove(to_io, rm_ec);
                fs::path journal = to_io;
                journal += "-journal";
                fs::remove(journal, rm_ec);
                fs::copy_file(from_io, to_io, fs::copy_options::overwrite_existing, cec);
            } else {
                return "cannot snapshot database: " + path_to_utf8(from);
            }
        } else {
            fs::copy_file(from_io, to_io, fs::copy_options::none, cec);
        }
        if (cec && best_effort) return skip(cec, migration_os_error_text(cec));
        if (cec) return "cannot copy " + path_to_utf8(from) + ": " + migration_os_error_text(cec);
        progress.copied_bytes += item.size;
        report();
        return {};
    };

    for (const auto& item : regular) {
        if (std::string err = copy_one(item, false); !err.empty()) return fail(err);
    }
    // 尽力子树放在普通文件之后、sqlite 列表之前;collect_best_effort_items 已排好序,
    // 子树内的主库先于它的旁路文件。
    for (const auto& item : best_effort) {
        if (std::string err = copy_one(item, true); !err.empty()) return fail(err);
    }
    if (progress.skipped_files > 0) {
        std::string samples;
        for (const auto& sample : skipped_samples) samples += (samples.empty() ? "" : "; ") + sample;
        LOG_WARN("[data-dir] best-effort copy skipped files: count=" +
                 std::to_string(progress.skipped_files) + " samples=" + samples);
    }
    // Snapshot each main database before skipping its WAL/SHM/journal sidecars.
    std::sort(sqlite.begin(), sqlite.end(), [](const Item& a, const Item& b) { return a.relative < b.relative; });
    for (const auto& item : sqlite) {
        if (std::string err = copy_one(item, false); !err.empty()) return fail(err);
    }

    // Refuse a changing source rather than publishing a partial conversation/config.
    // 尽力子树(best_effort)不在 regular 里,不参与大小 / mtime 比对。
    for (const auto& item : regular) {
        if (item.is_dir || item.is_link || path_to_utf8(*item.relative.begin()) == "logs") continue;
        const auto file = source_io / item.relative;
        if (fs::file_size(file, ec) != item.size || ec ||
                fs::last_write_time(file, ec) != item.modified || ec) {
            return fail("source changed during migration: " + path_to_utf8(item.relative) + "; retry when ACECode is idle");
        }
    }
    std::set<fs::path> expected;
    for (const auto& item : regular) expected.insert(item.relative);
    for (const auto& item : sqlite) expected.insert(item.relative);
    for (fs::recursive_directory_iterator it(source_io, ec), end; it != end; it.increment(ec)) {
        if (ec) return fail("cannot recheck source: " + migration_os_error_text(ec));
        const auto relative = it->path().lexically_relative(source_io);
        if (migration_excludes_entry(relative) || path_to_utf8(*relative.begin()) == "logs") {
            it.disable_recursion_pending();
            continue;
        }
        // 尽力子树持续在写,不参与「新增条目」比对。
        if (migration_is_best_effort_root(relative)) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_symlink(ec)) it.disable_recursion_pending();
        // 带上路径:0912 反馈第一次失败只有这句话,无从定位是谁在写。
        if (!expected.count(relative)) {
            return fail("source changed during migration: " + path_to_utf8(relative) +
                        "; retry when ACECode is idle");
        }
    }
    if (ec) return fail("cannot recheck source: " + migration_os_error_text(ec));
    // Revalidate the actual target: the user may have added a file while copying.
    const auto final_check = validate_migration_target(current_dir, check.normalized_target);
    if (final_check.error != MigrationTargetError::None) return fail(final_check.message);
    if (!fs::remove(final_io, ec) || ec) return fail("target is no longer empty");
    fs::rename(dest_io, final_io, ec);
    if (ec) return fail("cannot publish copied workspace: " + migration_os_error_text(ec));
    owned_staging.clear();

    DataDirRedirect redirect;
    redirect.data_dir = check.normalized_target;
    redirect.previous_data_dir = path_to_utf8(canonical_or_normal(source));
    redirect.migrated_at_ms = now_ms();
    // 旧目录的实际占用:清点到的全部文件(copied_bytes 走满后等于 total_bytes,含跳过项与
    // 快照后未复制的旁路文件,它们都还在旧目录里)+ 从未清点的排除项与可再生缓存。
    redirect.previous_size_bytes = progress.total_bytes + left_behind_bytes;
    redirect.cleanup_pending = true;
    if (!write_data_dir_redirect(default_dir, redirect)) {
        // Keep a verified copy on pointer failure; never recursively remove a public path.
        return fail("cannot write redirect pointer; copied data remains at " + check.normalized_target);
    }

    progress.state = "done";
    progress.restart_required = true;
    progress.finished_at_ms = now_ms();
    LOG_INFO("[data-dir] migration complete: target=" + check.normalized_target +
             " bytes=" + std::to_string(progress.copied_bytes) +
             " previous_size_bytes=" + std::to_string(redirect.previous_size_bytes));
    if (over_max_path_files > 0) {
        // ACECode 运行时不是 longPathAware:这些文件已复制,但运行期普通 API 可能读不到。
        LOG_WARN("[data-dir] migrated files with paths >= 260 characters: count=" +
                 std::to_string(over_max_path_files) + " sample=" + over_max_path_sample);
    }
    return progress;
}

DataDirMigrationJob::~DataDirMigrationJob() {
    if (thread_.joinable()) thread_.join();
}

bool DataDirMigrationJob::start(const std::string& current_dir, const std::string& default_dir,
                                const std::string& target, std::string* error,
                                std::function<void()> before_copy, std::function<void()> on_failure) {
    std::lock_guard<std::mutex> lk(mu_);
    if (active_.load() || (progress_ && progress_->restart_required)) {
        if (error) *error = "a data directory migration is already running";
        return false;
    }
    if (thread_.joinable()) thread_.join();  // 上一次已结束的线程回收
    MigrationProgress initial;
    initial.state = "running";
    initial.target = target;
    initial.started_at_ms = now_ms();
    progress_ = initial;
    active_.store(true);
    writes_blocked.store(true);
    set_state_file_writes_paused(true);
    thread_ = acecode::JoiningThread([this, current_dir, default_dir, target, before_copy, on_failure]() {
        MigrationProgress result;
        try { if (before_copy) before_copy(); result = run_data_dir_migration(
            current_dir, default_dir, target,
            [this](unsigned long long copied, unsigned long long total) {
                std::lock_guard<std::mutex> plk(mu_);
                if (progress_) {
                    progress_->copied_bytes = copied;
                    progress_->total_bytes = total;
                }
            }); } catch (const std::exception& e) {
            result.state = "failed";
            result.error = ensure_utf8(e.what());
            result.target = target;
        }
        {
            std::lock_guard<std::mutex> plk(mu_);
            progress_ = result;
        }
        active_.store(false);
        if (result.state != "done") {
            writes_blocked.store(false);
            set_state_file_writes_paused(false);
            try { if (on_failure) on_failure(); }
            catch (const std::exception& e) { LOG_WARN("[data-dir] resume failed: " + ensure_utf8(e.what())); }
        }
    });
    return true;
}

bool DataDirMigrationJob::active() const {
    return active_.load();
}

std::optional<MigrationProgress> DataDirMigrationJob::progress() const {
    std::lock_guard<std::mutex> lk(mu_);
    return progress_;
}

void DataDirMigrationJob::wait_for_test() {
    acecode::JoiningThread t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (thread_.joinable()) t = std::move(thread_);
    }
    if (t.joinable()) t.join();
}

std::string cleanup_previous_data_dir(const std::string& previous_dir,
                                      const std::string& default_dir) {
    if (previous_dir.empty()) return {};
    const fs::path prev = path_from_utf8(previous_dir);
    const auto redirect = read_data_dir_redirect(default_dir);
    if (!redirect || redirect->previous_data_dir.empty() ||
        prefix_key(canonical_or_normal(prev)) != prefix_key(canonical_or_normal(path_from_utf8(redirect->previous_data_dir)))) {
        return "previous workspace does not match the migration record";
    }
    const auto previous_key = prefix_key(canonical_or_normal(prev));
    const auto active_key = prefix_key(canonical_or_normal(path_from_utf8(redirect->data_dir)));
    if (!prev.is_absolute() || prev == prev.root_path() || starts_with(active_key, previous_key) || starts_with(previous_key, active_key)) {
        return "refusing to remove the active workspace or its parent";
    }
    // 删除走扩展长度路径:Agent 用 node / python 等长路径感知工具生成的超长文件,普通
    // 路径删不掉,以前会报 CLEANUP_FAILED。错误文案仍用展示形态。
    const fs::path prev_io = to_extended_length_path(prev);
    std::error_code ec;
    if (!fs::is_directory(prev_io, ec)) return {};
    const bool keep_pointer =
        prefix_key(canonical_or_normal(prev)) ==
        prefix_key(canonical_or_normal(path_from_utf8(default_dir)));
    std::string first_error;
    for (const auto& entry : fs::directory_iterator(prev_io, ec)) {
        if (keep_pointer && entry.path().filename() == kDataDirRedirectFileName) continue;
        std::error_code rm_ec;
        fs::remove_all(entry.path(), rm_ec);
        if (rm_ec && first_error.empty()) {
            first_error = "cannot remove " + path_to_utf8(prev / entry.path().filename()) + ": " +
                          migration_os_error_text(rm_ec);
        }
    }
    if (ec && first_error.empty()) first_error = "cannot list " + previous_dir + ": " + migration_os_error_text(ec);
    if (!keep_pointer && first_error.empty()) {
        std::error_code rm_ec;
        fs::remove(prev_io, rm_ec);  // 旧目录本身也删(不是默认目录时)
    }
    return first_error;
}

std::string acknowledge_data_dir_cleanup(const std::string& default_dir) {
    auto redirect = read_data_dir_redirect(default_dir);
    if (!redirect) return {};
    if (!redirect->cleanup_pending) return {};
    redirect->cleanup_pending = false;
    if (!write_data_dir_redirect(default_dir, *redirect)) {
        return "cannot update redirect pointer in " + default_dir;
    }
    return {};
}

DataDirStatus data_dir_status(RunMode mode) {
    DataDirStatus s;
    s.default_dir = resolve_default_data_dir(mode);
    s.effective_dir = resolve_data_dir(mode);
    if (auto redirect = read_data_dir_redirect(s.default_dir)) {
        s.redirect_target = redirect->data_dir;
        s.redirect_active =
            prefix_key(canonical_or_normal(path_from_utf8(s.effective_dir))) ==
            prefix_key(canonical_or_normal(path_from_utf8(redirect->data_dir)));
        s.migrated_at_ms = redirect->migrated_at_ms;
        s.previous_size_bytes = redirect->previous_size_bytes;
        s.cleanup_pending = redirect->cleanup_pending;
        std::error_code ec;
        const fs::path prev = path_from_utf8(redirect->previous_data_dir);
        if (!redirect->previous_data_dir.empty() && fs::is_directory(prev, ec)) {
            s.previous_dir = redirect->previous_data_dir;
            s.previous_exists = true;
            // 默认目录只剩指针文件时不算"仍有旧数据"。
            bool has_content = false;
            for (const auto& entry : fs::directory_iterator(prev, ec)) {
                if (entry.path().filename() == kDataDirRedirectFileName) continue;
                has_content = true;
                break;
            }
            s.previous_exists = has_content;
        }
        s.cleanup_prompt = s.redirect_active && s.cleanup_pending && s.previous_exists &&
                           s.previous_size_bytes > kCleanupPromptThresholdBytes;
    }
    return s;
}

}  // namespace acecode::environment
