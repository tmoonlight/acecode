#include "memory_state_store.hpp"

#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <sqlite3.h>

#include <chrono>
#include <random>
#include <sstream>

#ifdef _WIN32
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace acecode {

namespace {

constexpr const char* kSchemaSql =
    "CREATE TABLE IF NOT EXISTS extraction_progress ("
    "session_id TEXT PRIMARY KEY,"
    "project_dir TEXT NOT NULL DEFAULT '',"
    "next_message_index INTEGER NOT NULL DEFAULT 0,"
    "attempts INTEGER NOT NULL DEFAULT 0,"
    "last_error TEXT NOT NULL DEFAULT '',"
    "failed_marker TEXT NOT NULL DEFAULT '',"
    "seen_marker TEXT NOT NULL DEFAULT '',"
    "lease_owner TEXT NOT NULL DEFAULT '',"
    "lease_expires_ms INTEGER NOT NULL DEFAULT 0,"
    "updated_ms INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS consolidation_progress ("
    "scope_key TEXT PRIMARY KEY,"
    "attempts INTEGER NOT NULL DEFAULT 0,"
    "last_error TEXT NOT NULL DEFAULT '',"
    "failed_batch TEXT NOT NULL DEFAULT '',"
    "pending_plan TEXT NOT NULL DEFAULT '',"
    "pending_plan_hash TEXT NOT NULL DEFAULT '',"
    "last_plan_hash TEXT NOT NULL DEFAULT '',"
    "lease_owner TEXT NOT NULL DEFAULT '',"
    "lease_expires_ms INTEGER NOT NULL DEFAULT 0,"
    "last_success_ms INTEGER NOT NULL DEFAULT 0,"
    "updated_ms INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS tombstones ("
    "scope_key TEXT NOT NULL,"
    "name TEXT NOT NULL,"
    "title_norm TEXT NOT NULL DEFAULT '',"
    "deleted_ms INTEGER NOT NULL,"
    "PRIMARY KEY(scope_key, name));"
    "CREATE TABLE IF NOT EXISTS status ("
    "key TEXT PRIMARY KEY,"
    "value TEXT NOT NULL DEFAULT '',"
    "updated_ms INTEGER NOT NULL DEFAULT 0);";

class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        if (db && sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            LOG_WARN(std::string("[memory] state prepare failed: ") + sqlite3_errmsg(db));
            stmt_ = nullptr;
        }
    }
    ~Statement() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    explicit operator bool() const { return stmt_ != nullptr; }
    Statement& text(int index, const std::string& value) {
        sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT);
        return *this;
    }
    Statement& integer(int index, std::int64_t value) {
        sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value));
        return *this;
    }
    int step() { return sqlite3_step(stmt_); }
    bool done() { return step() == SQLITE_DONE; }
    std::string column_text(int index) const {
        const auto* raw = sqlite3_column_text(stmt_, index);
        return raw ? std::string(reinterpret_cast<const char*>(raw)) : std::string{};
    }
    std::int64_t column_int(int index) const {
        return static_cast<std::int64_t>(sqlite3_column_int64(stmt_, index));
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

const char* lease_table(MemoryLeaseKind kind) {
    return kind == MemoryLeaseKind::Extraction ? "extraction_progress" : "consolidation_progress";
}

const char* lease_key_column(MemoryLeaseKind kind) {
    return kind == MemoryLeaseKind::Extraction ? "session_id" : "scope_key";
}

} // namespace

std::int64_t memory_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string normalize_memory_title(const std::string& title) {
    std::string out;
    bool pending_space = false;
    for (unsigned char c : title) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) out.push_back(' ');
        pending_space = false;
        out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
    }
    return out;
}

std::string make_memory_lease_owner() {
    static thread_local std::mt19937_64 gen{std::random_device{}()};
#ifdef _WIN32
    const long long pid = static_cast<long long>(_getpid());
#else
    const long long pid = static_cast<long long>(getpid());
#endif
    std::ostringstream oss;
    oss << "pid-" << pid << "-" << std::hex << gen();
    return oss.str();
}

MemoryStateStore::MemoryStateStore(fs::path db_path) : db_path_(std::move(db_path)) {}

MemoryStateStore::~MemoryStateStore() = default;

bool MemoryStateStore::open(std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return open_locked(error);
}

bool MemoryStateStore::exec_locked(const char* sql, std::string* error) {
    char* raw_error = nullptr;
    const int rc = sqlite3_exec(db_.get(), sql, nullptr, nullptr, &raw_error);
    if (rc == SQLITE_OK) return true;
    const std::string message = raw_error ? raw_error : sqlite3_errmsg(db_.get());
    sqlite3_free(raw_error);
    if (error) *error = message;
    return false;
}

bool MemoryStateStore::open_locked(std::string* error) {
    if (db_.get()) return true;
    std::error_code ec;
    fs::create_directories(db_path_.parent_path(), ec);
    if (ec) {
        if (error) *error = "cannot create memory state directory: " + ec.message();
        return false;
    }
    platform::UniqueSqlite db;
    const std::string path = path_to_utf8(db_path_);
    if (sqlite3_open_v2(path.c_str(), db.put(),
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        if (error) {
            *error = std::string("cannot open memory state database: ") +
                     (db.get() ? sqlite3_errmsg(db.get()) : "unknown error");
        }
        return false;
    }
    sqlite3_busy_timeout(db.get(), 5000);
    db_ = std::move(db);
    (void)exec_locked("PRAGMA journal_mode=WAL;");
    (void)exec_locked("PRAGMA synchronous=NORMAL;");
    std::string schema_error;
    if (!exec_locked(kSchemaSql, &schema_error)) {
        db_.reset();
        if (error) *error = "cannot create memory state schema: " + schema_error;
        return false;
    }
    return true;
}

MemoryWriteLock::MemoryWriteLock(MemoryStateStore* store) : store_(store) {
    if (!store_) return;
    lock_ = std::unique_lock<std::recursive_mutex>(store_->mu_);
    if (!store_->open_locked(&error_)) return;
    if (store_->tx_depth_ > 0) {
        nested_ = true;
        acquired_ = true;
        ++store_->tx_depth_;
        return;
    }
    if (!store_->exec_locked("BEGIN IMMEDIATE;", &error_)) return;
    store_->tx_depth_ = 1;
    acquired_ = true;
}

MemoryWriteLock::~MemoryWriteLock() {
    if (!acquired_ || finished_) return;
    finished_ = true;
    if (nested_) {
        --store_->tx_depth_;
        return;
    }
    store_->tx_depth_ = 0;
    (void)store_->exec_locked("ROLLBACK;");
}

bool MemoryWriteLock::commit() {
    if (!acquired_ || finished_) return false;
    finished_ = true;
    if (nested_) {
        --store_->tx_depth_;
        return true;
    }
    store_->tx_depth_ = 0;
    if (store_->exec_locked("COMMIT;", &error_)) return true;
    (void)store_->exec_locked("ROLLBACK;");
    return false;
}

bool MemoryStateStore::try_acquire_lease(MemoryLeaseKind kind, const std::string& key,
                                         const std::string& owner, std::int64_t ttl_ms,
                                         std::int64_t now_ms) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr) || key.empty() || owner.empty()) return false;
    const std::string table = lease_table(kind);
    const std::string column = lease_key_column(kind);
    // 单条 UPSERT 原子完成「空闲 / 已是自己 / 已过期 → 接管」,不给两个进程
    // 「先查后写」之间的窗口。
    const std::string sql =
        "INSERT INTO " + table + "(" + column + ", lease_owner, lease_expires_ms, updated_ms) "
        "VALUES(?1, ?2, ?3, ?4) ON CONFLICT(" + column + ") DO UPDATE SET "
        "lease_owner = excluded.lease_owner, lease_expires_ms = excluded.lease_expires_ms "
        "WHERE " + table + ".lease_owner = '' OR " + table + ".lease_owner = excluded.lease_owner "
        "OR " + table + ".lease_expires_ms <= ?4;";
    Statement stmt(db_.get(), sql.c_str());
    if (!stmt) return false;
    stmt.text(1, key).text(2, owner).integer(3, now_ms + ttl_ms).integer(4, now_ms);
    if (!stmt.done()) return false;
    return sqlite3_changes(db_.get()) > 0;
}

bool MemoryStateStore::renew_lease(MemoryLeaseKind kind, const std::string& key,
                                   const std::string& owner, std::int64_t ttl_ms,
                                   std::int64_t now_ms) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return false;
    const std::string sql = std::string("UPDATE ") + lease_table(kind) +
        " SET lease_expires_ms = ?1 WHERE " + lease_key_column(kind) +
        " = ?2 AND lease_owner = ?3;";
    Statement stmt(db_.get(), sql.c_str());
    if (!stmt) return false;
    stmt.integer(1, now_ms + ttl_ms).text(2, key).text(3, owner);
    return stmt.done() && sqlite3_changes(db_.get()) > 0;
}

void MemoryStateStore::release_lease(MemoryLeaseKind kind, const std::string& key,
                                     const std::string& owner) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return;
    const std::string sql = std::string("UPDATE ") + lease_table(kind) +
        " SET lease_owner = '', lease_expires_ms = 0 WHERE " + lease_key_column(kind) +
        " = ?1 AND lease_owner = ?2;";
    Statement stmt(db_.get(), sql.c_str());
    if (!stmt) return;
    stmt.text(1, key).text(2, owner);
    (void)stmt.done();
}

std::optional<MemoryExtractionProgress> MemoryStateStore::extraction(const std::string& session_id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return std::nullopt;
    Statement stmt(db_.get(),
        "SELECT project_dir, next_message_index, attempts, last_error, failed_marker, "
        "lease_owner, lease_expires_ms, updated_ms, seen_marker FROM extraction_progress "
        "WHERE session_id = ?1;");
    if (!stmt) return std::nullopt;
    stmt.text(1, session_id);
    if (stmt.step() != SQLITE_ROW) return std::nullopt;
    MemoryExtractionProgress out;
    out.session_id = session_id;
    out.project_dir = stmt.column_text(0);
    out.next_message_index = stmt.column_int(1);
    out.attempts = static_cast<int>(stmt.column_int(2));
    out.last_error = stmt.column_text(3);
    out.failed_marker = stmt.column_text(4);
    out.lease_owner = stmt.column_text(5);
    out.lease_expires_ms = stmt.column_int(6);
    out.updated_ms = stmt.column_int(7);
    out.seen_marker = stmt.column_text(8);
    return out;
}

bool MemoryStateStore::commit_extraction(const std::string& session_id,
                                         const std::string& project_dir,
                                         std::int64_t next_message_index,
                                         const std::string& seen_marker,
                                         std::int64_t now_ms) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return false;
    Statement stmt(db_.get(),
        "INSERT INTO extraction_progress(session_id, project_dir, next_message_index, attempts, "
        "last_error, failed_marker, seen_marker, updated_ms) VALUES(?1, ?2, ?3, 0, '', '', ?4, ?5) "
        "ON CONFLICT(session_id) DO UPDATE SET project_dir = excluded.project_dir, "
        "next_message_index = MAX(extraction_progress.next_message_index, excluded.next_message_index), "
        "attempts = 0, last_error = '', failed_marker = '', seen_marker = excluded.seen_marker, "
        "updated_ms = excluded.updated_ms;");
    if (!stmt) return false;
    stmt.text(1, session_id).text(2, project_dir).integer(3, next_message_index)
        .text(4, seen_marker).integer(5, now_ms);
    return stmt.done();
}

bool MemoryStateStore::record_extraction_failure(const std::string& session_id,
                                                 const std::string& project_dir,
                                                 const std::string& error,
                                                 const std::string& marker,
                                                 std::int64_t now_ms) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return false;
    // 会话活动标记变了(有了新消息)就从 1 重新计数,否则累加。
    Statement stmt(db_.get(),
        "INSERT INTO extraction_progress(session_id, project_dir, attempts, last_error, "
        "failed_marker, updated_ms) VALUES(?1, ?2, 1, ?3, ?4, ?5) "
        "ON CONFLICT(session_id) DO UPDATE SET project_dir = excluded.project_dir, "
        "attempts = CASE WHEN extraction_progress.failed_marker = excluded.failed_marker "
        "THEN extraction_progress.attempts + 1 ELSE 1 END, "
        "last_error = excluded.last_error, failed_marker = excluded.failed_marker, "
        "updated_ms = excluded.updated_ms;");
    if (!stmt) return false;
    stmt.text(1, session_id).text(2, project_dir).text(3, error).text(4, marker).integer(5, now_ms);
    return stmt.done();
}

bool MemoryStateStore::forget_extraction(const std::string& session_id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return false;
    Statement stmt(db_.get(), "DELETE FROM extraction_progress WHERE session_id = ?1;");
    if (!stmt) return false;
    stmt.text(1, session_id);
    return stmt.done();
}

std::optional<MemoryConsolidationProgress> MemoryStateStore::consolidation(
    const std::string& scope_key) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return std::nullopt;
    Statement stmt(db_.get(),
        "SELECT attempts, last_error, failed_batch, pending_plan, pending_plan_hash, "
        "last_plan_hash, lease_owner, lease_expires_ms, last_success_ms, updated_ms "
        "FROM consolidation_progress WHERE scope_key = ?1;");
    if (!stmt) return std::nullopt;
    stmt.text(1, scope_key);
    if (stmt.step() != SQLITE_ROW) return std::nullopt;
    MemoryConsolidationProgress out;
    out.scope_key = scope_key;
    out.attempts = static_cast<int>(stmt.column_int(0));
    out.last_error = stmt.column_text(1);
    out.failed_batch = stmt.column_text(2);
    out.pending_plan = stmt.column_text(3);
    out.pending_plan_hash = stmt.column_text(4);
    out.last_plan_hash = stmt.column_text(5);
    out.lease_owner = stmt.column_text(6);
    out.lease_expires_ms = stmt.column_int(7);
    out.last_success_ms = stmt.column_int(8);
    out.updated_ms = stmt.column_int(9);
    return out;
}

bool MemoryStateStore::set_pending_plan(const std::string& scope_key, const std::string& plan,
                                        const std::string& plan_hash, std::int64_t now_ms) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return false;
    Statement stmt(db_.get(),
        "INSERT INTO consolidation_progress(scope_key, pending_plan, pending_plan_hash, updated_ms) "
        "VALUES(?1, ?2, ?3, ?4) ON CONFLICT(scope_key) DO UPDATE SET "
        "pending_plan = excluded.pending_plan, pending_plan_hash = excluded.pending_plan_hash, "
        "updated_ms = excluded.updated_ms;");
    if (!stmt) return false;
    stmt.text(1, scope_key).text(2, plan).text(3, plan_hash).integer(4, now_ms);
    return stmt.done();
}

bool MemoryStateStore::commit_consolidation(const std::string& scope_key,
                                            const std::string& plan_hash,
                                            std::int64_t now_ms) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return false;
    Statement stmt(db_.get(),
        "INSERT INTO consolidation_progress(scope_key, last_plan_hash, last_success_ms, updated_ms) "
        "VALUES(?1, ?2, ?3, ?3) ON CONFLICT(scope_key) DO UPDATE SET attempts = 0, "
        "last_error = '', failed_batch = '', pending_plan = '', pending_plan_hash = '', "
        "last_plan_hash = excluded.last_plan_hash, last_success_ms = excluded.last_success_ms, "
        "updated_ms = excluded.updated_ms;");
    if (!stmt) return false;
    stmt.text(1, scope_key).text(2, plan_hash).integer(3, now_ms);
    return stmt.done();
}

bool MemoryStateStore::record_consolidation_failure(const std::string& scope_key,
                                                    const std::string& error,
                                                    const std::string& batch_signature,
                                                    std::int64_t now_ms) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return false;
    Statement stmt(db_.get(),
        "INSERT INTO consolidation_progress(scope_key, attempts, last_error, failed_batch, updated_ms) "
        "VALUES(?1, 1, ?2, ?3, ?4) ON CONFLICT(scope_key) DO UPDATE SET "
        "attempts = CASE WHEN consolidation_progress.failed_batch = excluded.failed_batch "
        "THEN consolidation_progress.attempts + 1 ELSE 1 END, "
        "last_error = excluded.last_error, failed_batch = excluded.failed_batch, "
        "pending_plan = '', pending_plan_hash = '', updated_ms = excluded.updated_ms;");
    if (!stmt) return false;
    stmt.text(1, scope_key).text(2, error).text(3, batch_signature).integer(4, now_ms);
    return stmt.done();
}

bool MemoryStateStore::clear_consolidation(const std::string& scope_key) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return false;
    Statement stmt(db_.get(),
        "UPDATE consolidation_progress SET attempts = 0, last_error = '', failed_batch = '', "
        "pending_plan = '', pending_plan_hash = '' WHERE scope_key = ?1;");
    if (!stmt) return false;
    stmt.text(1, scope_key);
    return stmt.done();
}

bool MemoryStateStore::add_tombstone(const std::string& scope_key, const std::string& name,
                                     const std::string& title, std::int64_t now_ms) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return false;
    Statement stmt(db_.get(),
        "INSERT INTO tombstones(scope_key, name, title_norm, deleted_ms) VALUES(?1, ?2, ?3, ?4) "
        "ON CONFLICT(scope_key, name) DO UPDATE SET title_norm = excluded.title_norm, "
        "deleted_ms = excluded.deleted_ms;");
    if (!stmt) return false;
    stmt.text(1, scope_key).text(2, name).text(3, normalize_memory_title(title)).integer(4, now_ms);
    return stmt.done();
}

std::vector<MemoryTombstone> MemoryStateStore::tombstones(const std::string& scope_key,
                                                          std::int64_t now_ms) {
    std::vector<MemoryTombstone> out;
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return out;
    Statement stmt(db_.get(),
        "SELECT name, title_norm, deleted_ms FROM tombstones WHERE scope_key = ?1 "
        "AND deleted_ms > ?2 ORDER BY deleted_ms DESC;");
    if (!stmt) return out;
    stmt.text(1, scope_key).integer(2, now_ms - kTombstoneTtlMs);
    while (stmt.step() == SQLITE_ROW) {
        out.push_back({scope_key, stmt.column_text(0), stmt.column_text(1), stmt.column_int(2)});
    }
    return out;
}

bool MemoryStateStore::is_tombstoned(const std::string& scope_key, const std::string& name,
                                     const std::string& title, std::int64_t now_ms) {
    const std::string norm = normalize_memory_title(title);
    for (const auto& tomb : tombstones(scope_key, now_ms)) {
        if (normalize_memory_title(tomb.name) == normalize_memory_title(name)) return true;
        if (!norm.empty() && tomb.title_norm == norm) return true;
    }
    return false;
}

int MemoryStateStore::purge_expired_tombstones(std::int64_t now_ms) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return 0;
    Statement stmt(db_.get(), "DELETE FROM tombstones WHERE deleted_ms <= ?1;");
    if (!stmt) return 0;
    stmt.integer(1, now_ms - kTombstoneTtlMs);
    if (!stmt.done()) return 0;
    return sqlite3_changes(db_.get());
}

bool MemoryStateStore::set_status(const std::string& key, const std::string& value,
                                  std::int64_t now_ms) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return false;
    Statement stmt(db_.get(),
        "INSERT INTO status(key, value, updated_ms) VALUES(?1, ?2, ?3) ON CONFLICT(key) "
        "DO UPDATE SET value = excluded.value, updated_ms = excluded.updated_ms;");
    if (!stmt) return false;
    stmt.text(1, key).text(2, value).integer(3, now_ms);
    return stmt.done();
}

std::optional<std::pair<std::string, std::int64_t>> MemoryStateStore::status(
    const std::string& key) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!open_locked(nullptr)) return std::nullopt;
    Statement stmt(db_.get(), "SELECT value, updated_ms FROM status WHERE key = ?1;");
    if (!stmt) return std::nullopt;
    stmt.text(1, key);
    if (stmt.step() != SQLITE_ROW) return std::nullopt;
    return std::make_pair(stmt.column_text(0), stmt.column_int(1));
}

} // namespace acecode
