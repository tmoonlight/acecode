#include "audit_log.hpp"

#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>

namespace acecode::security {

namespace {

constexpr const char* kCreateSql =
    "CREATE TABLE IF NOT EXISTS audit_entries ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "ts_ms INTEGER NOT NULL,"
    "category TEXT NOT NULL,"
    "decision TEXT NOT NULL,"
    "source TEXT NOT NULL,"
    "reason TEXT NOT NULL DEFAULT '',"
    "tool TEXT NOT NULL DEFAULT '',"
    "target TEXT NOT NULL DEFAULT '',"
    "session_id TEXT NOT NULL DEFAULT '',"
    "cwd TEXT NOT NULL DEFAULT '',"
    "sandbox TEXT NOT NULL DEFAULT '',"
    "detail TEXT NOT NULL DEFAULT '{}'"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_audit_entries_ts ON audit_entries(ts_ms);"
    "CREATE INDEX IF NOT EXISTS idx_audit_entries_category ON audit_entries(category, id);";

// 每插入这么多条检查一次上限;上限本身是软的,多出几百条无所谓。
constexpr std::int64_t kPruneEvery = 256;

void set_error(std::string* error, const std::string& value) {
    if (error) *error = value;
}

std::string sqlite_error(sqlite3* db, const char* prefix) {
    std::string out = prefix;
    if (db) {
        out += ": ";
        out += sqlite3_errmsg(db);
    }
    return out;
}

bool exec_sql(sqlite3* db, const char* sql, std::string* error) {
    char* raw = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &raw);
    if (rc == SQLITE_OK) return true;
    std::string message = raw ? raw : sqlite3_errmsg(db);
    sqlite3_free(raw);
    set_error(error, message);
    return false;
}

class Statement {
public:
    Statement(sqlite3* db, const std::string& sql, std::string* error) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
            set_error(error, sqlite_error(db_, "prepare failed"));
            stmt_ = nullptr;
        }
    }
    ~Statement() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const { return stmt_; }
    explicit operator bool() const { return stmt_ != nullptr; }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

bool bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

bool bind_i64(sqlite3_stmt* stmt, int index, std::int64_t value) {
    return sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(value)) == SQLITE_OK;
}

std::string column_text(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char*>(text) : std::string{};
}

AuditEntry read_entry(sqlite3_stmt* stmt) {
    AuditEntry entry;
    entry.id = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 0));
    entry.ts_ms = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 1));
    entry.category = column_text(stmt, 2);
    entry.decision = column_text(stmt, 3);
    entry.source = column_text(stmt, 4);
    entry.reason = column_text(stmt, 5);
    entry.tool = column_text(stmt, 6);
    entry.target = column_text(stmt, 7);
    entry.session_id = column_text(stmt, 8);
    entry.cwd = column_text(stmt, 9);
    entry.sandbox = column_text(stmt, 10);
    const std::string detail = column_text(stmt, 11);
    auto parsed = nlohmann::json::parse(detail, nullptr, false);
    entry.detail = (parsed.is_discarded() || !parsed.is_object()) ? nlohmann::json::object() : std::move(parsed);
    return entry;
}

constexpr const char* kSelectColumns =
    "id, ts_ms, category, decision, source, reason, tool, target, session_id, cwd, sandbox, detail";

// 筛选条件(不含 before_id)拼成 WHERE 片段;绑定顺序与 bind_filters 一致。
struct FilterClause {
    std::string where;                 // 以 " WHERE " 开头,无条件时为空
    std::vector<std::string> texts;    // 按占位符顺序绑定的文本
    std::vector<std::int64_t> ints;    // since_ms(可选)
};

FilterClause build_filters(const AuditQuery& query, bool with_cursor) {
    FilterClause clause;
    std::vector<std::string> parts;
    if (!query.category.empty()) {
        parts.push_back("category = ?");
        clause.texts.push_back(query.category);
    }
    if (!query.decision.empty()) {
        parts.push_back("decision = ?");
        clause.texts.push_back(query.decision);
    }
    if (!query.text.empty()) {
        parts.push_back("(target LIKE ? ESCAPE '\\' OR reason LIKE ? ESCAPE '\\' OR tool LIKE ? ESCAPE '\\')");
        std::string escaped;
        for (char c : query.text) {
            if (c == '%' || c == '_' || c == '\\') escaped += '\\';
            escaped += c;
        }
        const std::string like = "%" + escaped + "%";
        clause.texts.push_back(like);
        clause.texts.push_back(like);
        clause.texts.push_back(like);
    }
    if (query.since_ms > 0) {
        parts.push_back("ts_ms >= ?");
        clause.ints.push_back(query.since_ms);
    }
    if (with_cursor && query.before_id > 0) {
        parts.push_back("id < ?");
        clause.ints.push_back(query.before_id);
    }
    if (!parts.empty()) {
        clause.where = " WHERE ";
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) clause.where += " AND ";
            clause.where += parts[i];
        }
    }
    return clause;
}

bool bind_filters(sqlite3_stmt* stmt, const FilterClause& clause, int& index) {
    for (const auto& text : clause.texts) {
        if (!bind_text(stmt, index++, text)) return false;
    }
    for (const auto value : clause.ints) {
        if (!bind_i64(stmt, index++, value)) return false;
    }
    return true;
}

std::string csv_escape(const std::string& value) {
    bool needs_quote = false;
    for (char c : value) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { needs_quote = true; break; }
    }
    if (!needs_quote) return value;
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

} // namespace

std::int64_t audit_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

nlohmann::json audit_entry_to_json(const AuditEntry& entry) {
    return nlohmann::json{
        {"id", entry.id},
        {"ts_ms", entry.ts_ms},
        {"category", entry.category},
        {"decision", entry.decision},
        {"source", entry.source},
        {"reason", entry.reason},
        {"tool", entry.tool},
        {"target", entry.target},
        {"session_id", entry.session_id},
        {"cwd", entry.cwd},
        {"sandbox", entry.sandbox},
        {"detail", entry.detail.is_object() ? entry.detail : nlohmann::json::object()},
    };
}

AuditLog::AuditLog() = default;

AuditLog::~AuditLog() {
    std::lock_guard<std::mutex> lock(mu_);
    close_locked();
}

AuditLog& AuditLog::instance() {
    static AuditLog log;
    return log;
}

std::string AuditLog::database_path_for(const std::string& data_dir) {
    return path_to_utf8(path_from_utf8(data_dir) / "security" / "audit.sqlite3");
}

bool AuditLog::configure(const std::string& data_dir, std::string* error) {
    if (data_dir.empty()) {
        set_error(error, "data directory is empty");
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path_from_utf8(data_dir) / "security", ec);
    if (ec) {
        const std::string message = "cannot create security directory: " + ec.message();
        LOG_WARN("[audit] " + message);
        set_error(error, message);
        return false;
    }
    std::string local_error;
    const bool ok = open_file(database_path_for(data_dir), &local_error);
    if (!ok) {
        LOG_WARN("[audit] audit log unavailable: " + local_error);
        set_error(error, local_error);
    }
    return ok;
}

bool AuditLog::open_file(const std::string& db_path, std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    return open_locked(db_path, error);
}

bool AuditLog::open_locked(const std::string& db_path, std::string* error) {
    close_locked();
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(db_path.c_str(), &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        const std::string message = sqlite_error(db, "cannot open audit database");
        if (db) sqlite3_close(db);
        set_error(error, message);
        return false;
    }
    sqlite3_busy_timeout(db, 3000);
    // WAL + NORMAL:审批门在调用线程同步写,别让一次 fsync 卡住工具执行。
    (void)exec_sql(db, "PRAGMA journal_mode=WAL;", nullptr);
    (void)exec_sql(db, "PRAGMA synchronous=NORMAL;", nullptr);
    std::string local_error;
    if (!exec_sql(db, kCreateSql, &local_error)) {
        sqlite3_close(db);
        set_error(error, "cannot create audit schema: " + local_error);
        return false;
    }
    db_ = db;
    path_ = db_path;
    inserts_since_prune_ = 0;
    (void)prune_locked(nullptr);
    return true;
}

void AuditLog::close_locked() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    path_.clear();
}

void AuditLog::close() {
    std::lock_guard<std::mutex> lock(mu_);
    close_locked();
}

bool AuditLog::available() const {
    std::lock_guard<std::mutex> lock(mu_);
    return db_ != nullptr;
}

std::string AuditLog::path() const {
    std::lock_guard<std::mutex> lock(mu_);
    return path_;
}

void AuditLog::set_max_entries(std::size_t value) {
    std::lock_guard<std::mutex> lock(mu_);
    max_entries_ = value == 0 ? 1 : value;
}

std::size_t AuditLog::max_entries() const {
    std::lock_guard<std::mutex> lock(mu_);
    return max_entries_;
}

bool AuditLog::record(AuditEntry& entry, std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) return false;
    if (entry.ts_ms <= 0) entry.ts_ms = audit_now_ms();
    Statement stmt(db_,
        "INSERT INTO audit_entries (ts_ms, category, decision, source, reason, tool, target, "
        "session_id, cwd, sandbox, detail) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        error);
    if (!stmt) return false;
    const std::string detail = entry.detail.is_object() ? entry.detail.dump() : std::string("{}");
    auto* s = stmt.get();
    if (!bind_i64(s, 1, entry.ts_ms) || !bind_text(s, 2, entry.category) ||
        !bind_text(s, 3, entry.decision) || !bind_text(s, 4, entry.source) ||
        !bind_text(s, 5, entry.reason) || !bind_text(s, 6, entry.tool) ||
        !bind_text(s, 7, entry.target) || !bind_text(s, 8, entry.session_id) ||
        !bind_text(s, 9, entry.cwd) || !bind_text(s, 10, entry.sandbox) ||
        !bind_text(s, 11, detail)) {
        set_error(error, sqlite_error(db_, "bind failed"));
        return false;
    }
    if (sqlite3_step(s) != SQLITE_DONE) {
        set_error(error, sqlite_error(db_, "insert failed"));
        return false;
    }
    entry.id = static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_));
    if (++inserts_since_prune_ >= kPruneEvery) {
        inserts_since_prune_ = 0;
        (void)prune_locked(nullptr);
    }
    return true;
}

bool AuditLog::prune_locked(std::string* error) {
    if (!db_) return false;
    // 保留最新 max_entries_ 条:删掉第 max_entries_+1 新那条及更旧的。
    Statement stmt(db_,
        "DELETE FROM audit_entries WHERE id <= COALESCE("
        "(SELECT id FROM audit_entries ORDER BY id DESC LIMIT 1 OFFSET ?), 0);",
        error);
    if (!stmt) return false;
    if (!bind_i64(stmt.get(), 1, static_cast<std::int64_t>(max_entries_))) {
        set_error(error, sqlite_error(db_, "bind failed"));
        return false;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        set_error(error, sqlite_error(db_, "prune failed"));
        return false;
    }
    return true;
}

AuditPage AuditLog::query(const AuditQuery& query, std::string* error) const {
    AuditPage page;
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) {
        set_error(error, "audit log is not configured");
        return page;
    }
    const int limit = std::clamp(query.limit, 1, kMaxQueryLimit);

    {
        const FilterClause clause = build_filters(query, /*with_cursor=*/false);
        Statement count(db_, std::string("SELECT COUNT(*) FROM audit_entries") + clause.where, error);
        if (!count) return page;
        int index = 1;
        if (!bind_filters(count.get(), clause, index)) {
            set_error(error, sqlite_error(db_, "bind failed"));
            return page;
        }
        if (sqlite3_step(count.get()) == SQLITE_ROW) {
            page.total = static_cast<std::int64_t>(sqlite3_column_int64(count.get(), 0));
        }
    }

    const FilterClause clause = build_filters(query, /*with_cursor=*/true);
    Statement select(db_,
        std::string("SELECT ") + kSelectColumns + " FROM audit_entries" + clause.where +
            " ORDER BY id DESC LIMIT ?;",
        error);
    if (!select) return page;
    int index = 1;
    if (!bind_filters(select.get(), clause, index) ||
        !bind_i64(select.get(), index, static_cast<std::int64_t>(limit) + 1)) {
        set_error(error, sqlite_error(db_, "bind failed"));
        return page;
    }
    while (true) {
        const int rc = sqlite3_step(select.get());
        if (rc == SQLITE_ROW) {
            if (static_cast<int>(page.entries.size()) >= limit) {
                page.has_more = true;
                break;
            }
            page.entries.push_back(read_entry(select.get()));
            continue;
        }
        if (rc != SQLITE_DONE) set_error(error, sqlite_error(db_, "query failed"));
        break;
    }
    return page;
}

AuditSummary AuditLog::summary(std::size_t blocked_path_limit, std::string* error) const {
    AuditSummary out;
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) {
        set_error(error, "audit log is not configured");
        return out;
    }
    {
        Statement stmt(db_, "SELECT COUNT(*), COALESCE(MAX(ts_ms), 0) FROM audit_entries;", error);
        if (stmt && sqlite3_step(stmt.get()) == SQLITE_ROW) {
            out.total = static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 0));
            out.last_ts_ms = static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 1));
        }
    }
    {
        Statement stmt(db_, "SELECT decision, COUNT(*) FROM audit_entries GROUP BY decision;", error);
        while (stmt && sqlite3_step(stmt.get()) == SQLITE_ROW) {
            out.by_decision[column_text(stmt.get(), 0)] =
                static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 1));
        }
    }
    {
        Statement stmt(db_, "SELECT category, COUNT(*) FROM audit_entries GROUP BY category;", error);
        while (stmt && sqlite3_step(stmt.get()) == SQLITE_ROW) {
            out.by_category[column_text(stmt.get(), 0)] =
                static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 1));
        }
    }
    if (blocked_path_limit > 0) {
        Statement stmt(db_,
            "SELECT target, COUNT(*), MAX(ts_ms) FROM audit_entries "
            "WHERE category = 'sandbox' AND target <> '' GROUP BY target "
            "ORDER BY MAX(ts_ms) DESC LIMIT ?;",
            error);
        if (stmt && bind_i64(stmt.get(), 1, static_cast<std::int64_t>(blocked_path_limit))) {
            while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
                AuditBlockedPath item;
                item.path = column_text(stmt.get(), 0);
                item.count = static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 1));
                item.last_ts_ms = static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 2));
                out.blocked_paths.push_back(std::move(item));
            }
        }
    }
    return out;
}

std::int64_t AuditLog::count(std::string* error) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) {
        set_error(error, "audit log is not configured");
        return 0;
    }
    Statement stmt(db_, "SELECT COUNT(*) FROM audit_entries;", error);
    if (!stmt) return 0;
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return static_cast<std::int64_t>(sqlite3_column_int64(stmt.get(), 0));
    }
    set_error(error, sqlite_error(db_, "count failed"));
    return 0;
}

bool AuditLog::clear(std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_) {
        set_error(error, "audit log is not configured");
        return false;
    }
    if (!exec_sql(db_, "DELETE FROM audit_entries;", error)) return false;
    (void)exec_sql(db_, "DELETE FROM sqlite_sequence WHERE name = 'audit_entries';", nullptr);
    (void)exec_sql(db_, "VACUUM;", nullptr);
    inserts_since_prune_ = 0;
    return true;
}

std::string render_audit_jsonl(const std::vector<AuditEntry>& entries) {
    std::string out;
    for (const auto& entry : entries) {
        out += audit_entry_to_json(entry).dump();
        out += '\n';
    }
    return out;
}

std::string render_audit_csv(const std::vector<AuditEntry>& entries) {
    std::string out = "id,ts_ms,category,decision,source,reason,tool,target,session_id,cwd,sandbox,detail\r\n";
    for (const auto& entry : entries) {
        out += std::to_string(entry.id) + "," + std::to_string(entry.ts_ms) + "," +
               csv_escape(entry.category) + "," + csv_escape(entry.decision) + "," +
               csv_escape(entry.source) + "," + csv_escape(entry.reason) + "," +
               csv_escape(entry.tool) + "," + csv_escape(entry.target) + "," +
               csv_escape(entry.session_id) + "," + csv_escape(entry.cwd) + "," +
               csv_escape(entry.sandbox) + "," +
               csv_escape(entry.detail.is_object() ? entry.detail.dump() : std::string("{}")) + "\r\n";
    }
    return out;
}

} // namespace acecode::security
