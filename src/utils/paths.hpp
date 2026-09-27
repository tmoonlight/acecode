#pragma once

// 进程级 RunMode 抽象 + 跨平台数据目录解析(spec design.md Decision 8)。
//
// 目的: ServiceMain 入口需要在做任何文件 IO 之前把数据根从 ~/.acecode/ 切到
// 系统级位置(Windows ProgramData / macOS /Library/Application Support /
// Linux /var/lib),否则 LocalSystem 身份的 daemon 写的产物落在 systemprofile
// 下,TUI 看不见。User 模式(默认)行为与历史完全一致 — TUI 与 standalone
// daemon 不受影响。
//
// set_run_mode() 是 once-only — 二次调用打 LOG_WARN 后忽略;这是一道防呆,避免
// 进程半路改根目录把状态散到两套路径下。测试通过 override_run_mode_for_test()
// 绕过 once 保护,fixture TearDown 应调 reset_run_mode_for_test() 清场。

#include <optional>
#include <string>
#include <vector>

namespace acecode {

// ── 数据目录重定向指针(openspec: data-directory-relocation)────────────────
//
// 用户在 Settings → 配置 里迁移数据目录后,默认目录里留下一个
// `data-dir.redirect.json`,所有入口(TUI / daemon / headless / Desktop 壳)在
// resolve_data_dir() 里读它决定真正的数据根。指针本身永远待在**平台默认目录**,
// 因为那是进程在读到任何配置之前唯一知道的位置;删掉它即回滚到默认目录
// (迁移是复制不是移动,旧数据仍在原地)。
inline constexpr const char* kDataDirRedirectFileName = "data-dir.redirect.json";

struct DataDirRedirect {
    std::string data_dir;                    // 新数据根(绝对路径),必填
    std::string previous_data_dir;           // 迁移前的数据根(可空)
    long long migrated_at_ms = 0;            // 迁移完成时间(epoch ms,0 = 未知)
    unsigned long long previous_size_bytes = 0;  // 迁移时旧目录的占用(复制量 + 未迁移的排除项 / 缓存)
    bool cleanup_pending = false;            // 是否还没问过用户要不要删旧目录
};

// 指针文件完整路径:<default_dir>/data-dir.redirect.json。
std::string data_dir_redirect_path(const std::string& default_dir);

// 读指针。文件不存在 / 非法 JSON / 缺 data_dir → nullopt。不校验目标目录是否存在。
std::optional<DataDirRedirect> read_data_dir_redirect(const std::string& default_dir);

// 原子写指针(tmp + rename)。成功返回 true。
bool write_data_dir_redirect(const std::string& default_dir, const DataDirRedirect& redirect);

// 删除指针(不存在也算成功)。
bool remove_data_dir_redirect(const std::string& default_dir);

// Expand ~ and ${ENV} style variables in a path string. Returns the expanded
// form; missing env vars are left as-is (per hermes convention).
std::string expand_path(const std::string& raw);

// Collect project-level directories from cwd up to (but not including) the
// user's home directory. Returned deepest-first so cwd-level skills take
// precedence over ancestor-level skills when scanned in order. HOME itself is
// excluded because the user-global skills root (`~/.acecode/skills`) is
// registered separately.
std::vector<std::string> get_project_dirs_up_to_home(const std::string& cwd);

enum class RunMode {
    User    = 0, // 默认 — TUI / standalone daemon / `acecode daemon --foreground`
    Service = 1, // SCM ServiceMain 拉起的 worker
};

// 进程启动早期调一次。第二次起被吞掉(LOG_WARN 提示)。
void set_run_mode(RunMode mode);

// 当前进程的 RunMode(默认 User)。
RunMode get_run_mode();

// 纯函数: 算给定 mode 下的数据根目录(不创建目录,调用方自己 create_directories)。
//
// Windows | User    : %USERPROFILE%\.acecode\ (USERPROFILE 缺失退到 HOMEDRIVE+HOMEPATH,再缺失退到 .\.acecode)
//         | Service : %PROGRAMDATA%\acecode\  (≈ C:\ProgramData\acecode\,缺失退到字面 C:\ProgramData)
// macOS   | User    : $HOME/.acecode/         (缺失退到 ./.acecode)
//         | Service : /Library/Application Support/acecode
// Linux   | User    : $HOME/.acecode/         (缺失退到 ./.acecode)
//         | Service : /var/lib/acecode
//
// 这是**平台默认**目录,不看重定向指针。指针文件永远放在这里。
std::string resolve_default_data_dir(RunMode mode);

// 生效的数据根 = 默认目录 + 重定向指针:指针指向的绝对路径存在时用它,否则用
// 默认目录并 LOG_WARN 一次。结果按 (mode, 默认目录) 缓存,进程内只读一次指针;
// 默认目录随环境变量变化时会重新解析(测试改 HOME 之后仍正确)。
std::string resolve_data_dir(RunMode mode);

// 重定向解析告警的补记。resolve_data_dir 在「指针目标不可用」和「指针文件存在
// 但读不出 / 内容无效」两种情况下回退默认目录;这发生在 Logger 初始化之前(日志
// 目录本身就由它决定),当场的 LOG_WARN 会被静默丢掉。所以首次发生时把告警文本
// 另存一份,等入口初始化日志之后再补记。
//
// take_...:取出待补告警并清空(没有则 nullopt)。
// log_deferred_...:取出后以 `[paths] (deferred) ` 前缀 LOG_WARN;没有则什么都不做。
// 调用点:worker / main(TUI)/ headless / Windows 服务(service_win.cpp)四处
// init_with_rotation 的下一行。服务那处不能省:它的 config 校验失败会在 run_worker 之前返回。
// 不要挪进 environment bootstrap:daemon 的 bootstrap 早于日志初始化。
std::optional<std::string> take_data_dir_resolution_warning();
void log_deferred_data_dir_resolution_warning();

// 测试专用:清掉 resolve_data_dir 的缓存(写了指针之后重新解析),同时清空待补
// 的解析告警,避免跨测试污染。
void reset_data_dir_cache_for_test();

// 进程级 run/ 目录覆盖。非空时 get_run_dir() 直接返回这个,而不再
// 用 <data_dir>/run/ 默认。
//
// 用途: desktop 多 workspace 模式下,每个 workspace 的 daemon 必须有独立的
// runtime files (heartbeat / pid / port / token / GUID lock),否则第一个 daemon
// 启动后 validate_can_start 会拒绝其它 workspace 的 daemon。daemon 的 cli
// 解析到 --run-dir=<path> 时在 worker 启动早期调本函数,把 run/ 切到
// ~/.acecode/projects/<workspace_hash>/run/。
//
// config / sessions / memory / logs 等其它子目录仍走 <data_dir>(共享),只
// 隔离 run/ — 这是修复孤儿 daemon 互锁问题的最小必要面。
//
// 空字符串 = 清除 override。线程安全(内部加锁)。
void set_run_dir_override(const std::string& path);
std::string get_run_dir_override();

// ── 数据目录快捷函数(原 config.hpp,refactor20260927 P2-04 移到这里;运行期行为不变)
// Get the path to ~/.acecode/ directory
std::string get_acecode_dir();

// Get the path to ~/.acecode/run/ (creates it if missing on first call site —
// callers are responsible for filesystem::create_directories when needed).
std::string get_run_dir();

// Get the path to ~/.acecode/logs/ (callers handle create_directories).
std::string get_logs_dir();

// === 测试专用 helper(只在测试代码用,生产路径不应调) ===

// 直接覆盖当前 RunMode,绕过 once-only 保护;返回原值方便 test fixture 还原。
RunMode override_run_mode_for_test(RunMode mode);

// 把 RunMode 与 once-only 标记都重置回初始(User + 未 set 过)。
// fixture TearDown 调,确保测试间互不污染。
void reset_run_mode_for_test();

} // namespace acecode
