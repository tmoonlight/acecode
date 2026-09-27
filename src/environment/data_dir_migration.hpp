#pragma once

// 数据目录迁移(openspec: data-directory-relocation)。
//
// 流程:校验目标 → 后台复制整个数据目录(排除 run/、tmp/、*.lock 与指针文件本身)
// → 在**平台默认目录**写 data-dir.redirect.json → 要求重启。Windows 上复制与清理的
// 文件 IO 走扩展长度路径(to_extended_length_path),超过 MAX_PATH 的文件也能复制 / 删除;
// 但 ACECode 运行时不是 longPathAware,这只保证迁移成功,不保证运行期能读到这些文件。复制不是移动:失败时
// 删掉半成品目标即可重试,回滚只需删指针。迁移后首次启动若旧数据超过 100 MB,
// 由 status 端点报 cleanup 提示,用户决定删还是留。
//
// 排除规则:顶层 run/、tmp/、edge-app-profile/(webapp 兼容模式每次启动都重建的 Edge
// profile)、指针文件本身、任何 *.lock,以及 cache/no-workspace/<id>/.acecode/tmp(无工作区
// 会话的 ACECODE_TMPDIR,可再生;代价是旧目录删除后历史里对这些 scratch 文件的别名引用失效)。
//
// 尽力复制:agent-browser/webview2 是 Desktop 启动即建立、持续在写的 WebView2 profile,
// 迁移又只能从 Desktop 发起,它会让变更复检每次都失败。所以这个子树单独容错遍历、跳过
// 可再生缓存,复制失败(not-found 除外)只计入 skipped_files,且不参与变更复检;其它路径
// 仍然严格失败。
//
// SQLite:文件头是 SQLite 的库走在线 backup 拿一致快照,快照成功后同名 -wal / -shm /
// -journal 一律不复制 —— 把热 journal 原样放在一致快照旁边,SQLite 打开时会把旧页回滚进
// 快照把它写坏。
//
// 纯逻辑(校验、排除规则、阈值)不依赖 web 层,进 acecode_testable 单测。

#include "daemon/runtime_files.hpp"
#include "utils/paths.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <shared_mutex>

namespace acecode::environment {

std::shared_mutex& data_dir_write_mutex();
bool data_dir_writes_blocked();
void reset_data_dir_write_gate_for_test();
// 数据目录下(run/** 与 projects/*/run/**)是否有别的 daemon 在用。命中时若 holder 非空,
// 填入 describe_daemon_pid_holder 的描述;扫描出错(按「有占用」处理)时填出错原因。
// holder 只用于拒绝日志,不进响应体。
bool data_dir_has_other_daemons(const std::string& directory, std::string* holder = nullptr);

// 拒绝日志里的占用者描述:`pid=<n> file=<utf8> legacy=<yes|no>`。legacy 表示 pid 文件
// 位于 projects/*/run(旧版 per-workspace daemon 的遗留目录;Desktop 现在用
// run/desktop-shared),用来区分「真有别的实例」与「遗留 pid 被无关进程复用」。
std::string describe_daemon_pid_holder(const std::filesystem::path& data_root,
                                       const std::filesystem::path& pid_file,
                                       long long pid);

// 某个 daemon.pid 是否构成「别的实例在用」。纯函数,进程探测结果由调用方传入。
//   - pid 无效、已死、或就是自己 → 不拦;
//   - daemon::runtime_pid_reuse_is_proven 为真(进程镜像已不是 acecode,或进程启动晚于
//     该目录心跳)→ 不拦,reason 以 "pid reused" 开头;与 Desktop daemon_pool 的复用
//     判定同一口径;
//   - 其余(Match,或身份未知、没有心跳)一律拦,fail-closed。
// 起因:projects/*/run 下旧版 per-workspace daemon 的遗留 pid 文件没人清理,Windows 的
// PID 复用又很频繁,只看「pid 存活」时它一撞上无关进程,迁移就永远 OTHER_INSTANCES_ACTIVE。
struct DaemonPidHolderVerdict {
    bool blocks = false;
    std::string reason;
};
DaemonPidHolderVerdict evaluate_daemon_pid_holder(
    const daemon::RuntimeSnapshot& snapshot,
    std::int64_t current_pid,
    bool pid_alive,
    daemon::DaemonProcessIdentity identity,
    const std::optional<std::int64_t>& start_ms);

// OS 错误文本(ec.message())统一转 UTF-8。MSVC 的 system_category().message() 走 ANSI
// 代码页,中文 Windows 上是 GBK;原样进 progress.error 后 json dump 抛 type_error.316,
// /migration 与 /data-dir 每次轮询都 500。只转换 OS 文本这一段再拼接:对拼好的整串调
// ensure_utf8 时,整串里的 UTF-8 中文路径会让它按 GBK 重新解码,路径反而变乱码。
std::string migration_os_error_text(const std::error_code& ec);

inline constexpr unsigned long long kCleanupPromptThresholdBytes = 100ULL * 1024 * 1024;

// 私有 staging 目录名前缀(建在目标的父目录里,复制完整后整体 rename 成目标)。
// 曾经是 ".acecode-migration-" + 完整 uuid,共 55 字符,正是它把 staging 下的深层路径
// 先于最终路径顶破 MAX_PATH。现在前缀 + 8 位 hex 共 21 字符。IO 虽然已走扩展长度路径,
// 短名仍有意义:失败清理、杀软扫描、用户手工查看时路径不会比最终路径先撞线。
inline constexpr const char* kMigrationStagingPrefix = ".acecode-mig-";

// kMigrationStagingPrefix + uuid 前 8 位 hex。
std::string make_migration_staging_name();

enum class MigrationTargetError {
    None,
    NotAbsolute,     // 目标不是绝对路径
    SameAsCurrent,   // 目标就是当前数据目录
    InsideCurrent,   // 目标在当前数据目录里面(复制会递归自己)
    ContainsCurrent, // 目标包含当前数据目录
    NotADirectory,   // 目标存在但不是目录
    NotEmpty,        // 目标是非空目录(不合并进已有数据)
    NotWritable,     // 目标不可写
};

// 给 REST 用的稳定错误码:TARGET_NOT_ABSOLUTE / TARGET_SAME_AS_CURRENT /
// TARGET_INSIDE_CURRENT / TARGET_CONTAINS_CURRENT / TARGET_NOT_A_DIRECTORY /
// TARGET_NOT_EMPTY / TARGET_NOT_WRITABLE。
const char* migration_target_error_code(MigrationTargetError error);

struct MigrationTargetCheck {
    MigrationTargetError error = MigrationTargetError::None;
    std::string message;            // 人类可读原因
    std::string normalized_target;  // weakly_canonical 后的目标(供后续复制使用)
};

// 校验并规范化目标。通过时目标目录已存在(不存在会创建)且已验证可写。
// 路径比较全部先 weakly_canonical,junction / 大小写差异不会绕过检查。
MigrationTargetCheck validate_migration_target(const std::string& current_dir,
                                               const std::string& target);

// 相对数据根的条目是否被排除:顶层 run/、tmp/、edge-app-profile/、指针文件、任何 *.lock,
// 以及锚定到 cache/no-workspace/<id>/.acecode/tmp 的无工作区会话临时目录(同一会话目录下的
// 其它产出文件照常复制)。
bool migration_excludes_entry(const std::filesystem::path& relative);

// 相对数据根的路径是否恰好是尽力复制子树的根(generic 形态 == "agent-browser/webview2")。
bool migration_is_best_effort_root(const std::filesystem::path& relative);

// 尽力复制子树里可以直接跳过的可再生条目:任一路径段是 Chromium 缓存目录(Cache、
// Code Cache、GPUCache、GrShaderCache、GraphiteDawnCache、DawnCache、DawnGraphiteCache、
// DawnWebGPUCache、ShaderCache、Crashpad,大小写不敏感),或文件名是 lockfile / LOCK。
bool migration_skips_rebuildable_browser_entry(const std::filesystem::path& relative);

// sqlite 主库 / wal / shm / journal 要作为一组最后复制,尽量拿到一致快照。
bool migration_is_sqlite_family(const std::filesystem::path& relative);

struct MigrationProgress {
    std::string state = "idle";  // idle | running | done | failed
    std::string target;
    unsigned long long copied_bytes = 0;
    unsigned long long total_bytes = 0;
    // 尽力复制子树里复制失败而被跳过的文件数(not-found 不计)。> 0 时 Agent Browser
    // 重启后可能需要重新登录;ACECode 自己的数据不受影响。
    unsigned long long skipped_files = 0;
    std::string error;
    bool restart_required = false;
    long long started_at_ms = 0;
    long long finished_at_ms = 0;
};

using MigrationProgressFn = std::function<void(unsigned long long copied,
                                               unsigned long long total)>;

// 同步执行完整迁移(worker 线程与单测共用):复制 + 写指针。失败时删除目标目录。
MigrationProgress run_data_dir_migration(const std::string& current_dir,
                                         const std::string& default_dir,
                                         const std::string& target,
                                         const MigrationProgressFn& on_progress = {});

// 后台任务壳:同一时间只允许一个迁移。
class DataDirMigrationJob {
public:
    DataDirMigrationJob() = default;
    ~DataDirMigrationJob();
    DataDirMigrationJob(const DataDirMigrationJob&) = delete;
    DataDirMigrationJob& operator=(const DataDirMigrationJob&) = delete;

    // 启动后台复制。已有任务在跑 → false 并写 error。
    bool start(const std::string& current_dir, const std::string& default_dir,
               const std::string& target, std::string* error,
               std::function<void()> before_copy = {}, std::function<void()> on_failure = {});
    bool active() const;
    std::optional<MigrationProgress> progress() const;  // 从未启动 → nullopt
    void wait_for_test();                               // 等后台线程结束

private:
    mutable std::mutex mu_;
    std::optional<MigrationProgress> progress_;
    std::thread thread_;
    std::atomic<bool> active_{false};
};

// 清理旧目录内容。previous_dir 与 default_dir 是同一目录时保留指针文件。
// 返回错误串,空 = 成功(目录不存在也算成功)。
std::string cleanup_previous_data_dir(const std::string& previous_dir,
                                      const std::string& default_dir);

// 把指针的 cleanup_pending 置 false(用户选了「保留」或删除已完成)。
std::string acknowledge_data_dir_cleanup(const std::string& default_dir);

struct DataDirStatus {
    std::string effective_dir;
    std::string default_dir;
    bool redirect_active = false;
    std::string redirect_target;          // 指针目标(可能与 effective 不同:目标不可用时)
    std::string previous_dir;             // 指针记录的旧目录(存在时才填)
    bool previous_exists = false;
    unsigned long long previous_size_bytes = 0;
    long long migrated_at_ms = 0;
    bool cleanup_pending = false;
    bool cleanup_prompt = false;          // pending && previous 存在 && 大小超阈值
};

DataDirStatus data_dir_status(RunMode mode);

}  // namespace acecode::environment
