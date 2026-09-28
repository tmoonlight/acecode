#pragma once

// 安全审计存储(openspec add-security-center D1)。
//
// 进程级单例,SQLite 落在 `<data_dir>/security/audit.sqlite3`。唯一的记录入口是
// AgentLoop 的审批门:bash 的每次决策、写文件工具与其它需确认工具的决策、沙盒
// 拒绝、规则写回与会话授权。只读工具的自动放行不记 —— 否则每回合几十条读把
// 日志淹掉,用户要看的是「AI 改了什么、凭什么」。
//
// 未 configure(测试 / 未接线)时 record() 是 no-op;AgentLoop 经 set_audit_sink
// 注入接收器,单测用 lambda 收集,不碰磁盘。

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace acecode::security {

// category
constexpr const char* kAuditCategoryCommand = "command";   // bash
constexpr const char* kAuditCategoryFile = "file";         // file_write / file_edit / apply_patch
constexpr const char* kAuditCategoryTool = "tool";         // 其它需确认的工具
constexpr const char* kAuditCategorySandbox = "sandbox";   // 沙盒拒绝
constexpr const char* kAuditCategoryRule = "rule";         // 规则写回 / 会话授权
// decision
constexpr const char* kAuditDecisionAllow = "allow";
constexpr const char* kAuditDecisionAllowSession = "allow_session";
constexpr const char* kAuditDecisionAllowScoped = "allow_scoped";
constexpr const char* kAuditDecisionAllowRemember = "allow_remember";
constexpr const char* kAuditDecisionDeny = "deny";
constexpr const char* kAuditDecisionForbidden = "forbidden";
constexpr const char* kAuditDecisionBlocked = "blocked";
// source
constexpr const char* kAuditSourceAuto = "auto";
constexpr const char* kAuditSourceRule = "rule";
constexpr const char* kAuditSourceSession = "session";
constexpr const char* kAuditSourceUser = "user";
constexpr const char* kAuditSourceHook = "hook";
constexpr const char* kAuditSourceHeadless = "headless";
constexpr const char* kAuditSourceGoal = "goal";
constexpr const char* kAuditSourceSandbox = "sandbox";
constexpr const char* kAuditSourceNone = "none";

struct AuditEntry {
    std::int64_t id = 0;
    std::int64_t ts_ms = 0;
    std::string category;
    std::string decision;
    std::string source;
    std::string reason;      // 机器可读原因(exec decision reason / dangerous_path / ...)
    std::string tool;
    std::string target;      // 命令原文或首个路径;沙盒拒绝时是被拒路径(抽不到为空)
    std::string session_id;
    std::string cwd;
    std::string sandbox;     // 执行沙盒:read-only / workspace-write / full-access / 空
    nlohmann::json detail = nlohmann::json::object();
};

nlohmann::json audit_entry_to_json(const AuditEntry& entry);

struct AuditQuery {
    std::string category;        // 空 = 全部
    std::string decision;        // 空 = 全部
    std::int64_t since_ms = 0;   // 0 = 不限
    std::int64_t before_id = 0;  // 游标分页:只取 id < before_id;0 = 从最新开始
    std::string text;            // target / reason / tool 的子串
    int limit = 100;             // clamp 到 [1, kMaxQueryLimit]
};

struct AuditPage {
    std::vector<AuditEntry> entries;   // id 倒序
    bool has_more = false;
    std::int64_t total = 0;            // 满足筛选(不含 before_id)的总数
};

struct AuditBlockedPath {
    std::string path;
    std::int64_t count = 0;
    std::int64_t last_ts_ms = 0;
};

struct AuditSummary {
    std::int64_t total = 0;
    std::map<std::string, std::int64_t> by_decision;
    std::map<std::string, std::int64_t> by_category;
    std::int64_t last_ts_ms = 0;
    std::vector<AuditBlockedPath> blocked_paths;   // category=sandbox、target 非空,按最近时间排
};

using AuditSink = std::function<void(const AuditEntry&)>;

class AuditLog {
public:
    static constexpr std::size_t kDefaultMaxEntries = 20000;
    static constexpr int kMaxQueryLimit = 20000;

    AuditLog();
    ~AuditLog();
    AuditLog(const AuditLog&) = delete;
    AuditLog& operator=(const AuditLog&) = delete;

    static AuditLog& instance();
    static std::string database_path_for(const std::string& data_dir);

    // 打开 `<data_dir>/security/audit.sqlite3`(目录不存在则创建)。失败时保持
    // 未配置状态,record 继续 no-op,错误进日志与 *error。
    bool configure(const std::string& data_dir, std::string* error = nullptr);
    // 直接打开指定文件(测试与导出工具用)。
    bool open_file(const std::string& db_path, std::string* error = nullptr);
    void close();
    bool available() const;
    std::string path() const;

    void set_max_entries(std::size_t value);
    std::size_t max_entries() const;

    // 写入。entry.ts_ms 为 0 时取当前时间;返回后 entry.id 已回填。
    bool record(AuditEntry& entry, std::string* error = nullptr);
    bool record(AuditEntry&& entry, std::string* error = nullptr) { return record(entry, error); }

    AuditPage query(const AuditQuery& query, std::string* error = nullptr) const;
    AuditSummary summary(std::size_t blocked_path_limit = 20, std::string* error = nullptr) const;
    std::int64_t count(std::string* error = nullptr) const;
    bool clear(std::string* error = nullptr);

private:
    bool open_locked(const std::string& db_path, std::string* error);
    bool prune_locked(std::string* error);
    void close_locked();

    mutable std::mutex mu_;
    sqlite3* db_ = nullptr;
    std::string path_;
    std::size_t max_entries_ = kDefaultMaxEntries;
    std::int64_t inserts_since_prune_ = 0;
};

inline AuditLog& audit_log() { return AuditLog::instance(); }

std::int64_t audit_now_ms();

// 导出:JSONL 每行一个 audit_entry_to_json 对象;CSV 带表头,字段按 RFC 4180 转义。
std::string render_audit_jsonl(const std::vector<AuditEntry>& entries);
std::string render_audit_csv(const std::vector<AuditEntry>& entries);

} // namespace acecode::security
