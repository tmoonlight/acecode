#include "task_suggestion_store.hpp"

#include "compact_checkpoint.hpp"
#include "session_storage.hpp"
#include "utils/uuid.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <set>
#include <utility>

namespace acecode {
namespace {

using json = nlohmann::json;
constexpr std::size_t kMaxRecordBytes = 96 * 1024;
constexpr std::size_t kMaxPendingSideTasks = 3;

void set_error(std::string* error, const std::string& value) {
    if (error) *error = value;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string trim(std::string value) {
    const auto nonspace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), nonspace));
    value.erase(std::find_if(value.rbegin(), value.rend(), nonspace).base(), value.end());
    return value;
}

bool valid_identifier(const std::string& id) {
    return !id.empty() && id.size() <= 160 &&
        std::all_of(id.begin(), id.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '-' || ch == '_';
        });
}

bool valid_scope(const std::string& source, std::string* error) {
    if (valid_identifier(source)) return true;
    set_error(error, "invalid source session id");
    return false;
}

bool valid_status(const std::string& status) {
    return status == "pending" || status == "queued" || status == "starting" ||
        status == "started" || status == "failed" || status == "dismissed";
}

bool bounded_string(json& record, const char* key, std::size_t maximum,
                    bool required, std::string* error) {
    if (!record.contains(key)) {
        if (!required) return true;
        set_error(error, std::string(key) + " is required");
        return false;
    }
    if (!record[key].is_string()) {
        set_error(error, std::string(key) + " must be a string");
        return false;
    }
    const auto value = trim(record[key].get<std::string>());
    if ((required && value.empty()) || value.size() > maximum ||
        value.find('\0') != std::string::npos) {
        set_error(error, std::string(key) + " is empty or exceeds its size limit");
        return false;
    }
    record[key] = value;
    return true;
}

bool validate_record_contents(json& record, std::string* error) {
    if (!record.is_object() ||
        !bounded_string(record, "title", 240, true, error) ||
        !bounded_string(record, "description", 2400, true, error) ||
        !bounded_string(record, "prompt", 24000, true, error) ||
        !bounded_string(record, "dedupe_key", 512, false, error)) return false;
    if (!record.contains("kind") || !record["kind"].is_string() ||
        (record["kind"] != "side_task" && record["kind"] != "context_handoff")) {
        set_error(error, "kind must be side_task or context_handoff");
        return false;
    }
    if (record.contains("status") &&
        (!record["status"].is_string() ||
         !valid_status(record["status"].get<std::string>()))) {
        set_error(error, "invalid suggestion status");
        return false;
    }
    if (record.contains("evidence") &&
        ((!record["evidence"].is_array() && !record["evidence"].is_string() &&
          !record["evidence"].is_object()) || record["evidence"].dump().size() > 12000)) {
        set_error(error, "evidence must be bounded structured data or text");
        return false;
    }
    if (record.dump().size() <= kMaxRecordBytes) return true;
    set_error(error, "suggestion exceeds its size limit");
    return false;
}

bool validate_record(json& record, std::string* error) {
    try {
        return validate_record_contents(record, error);
    } catch (const json::exception&) {
        set_error(error, "suggestion contains invalid structured data or UTF-8");
        return false;
    }
}

class Database {
public:
    Database(const std::filesystem::path& path, bool writable, std::string* error)
        : error_(error) {
        std::error_code ec;
        if (!writable && !std::filesystem::exists(path, ec)) {
            if (ec) set_error(error_, "cannot inspect suggestion database: " + ec.message());
            return;
        }
        if (writable) {
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) {
                set_error(error_, "cannot create suggestion directory: " + ec.message());
                return;
            }
        }
        const int flags = writable ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                                   : SQLITE_OPEN_READONLY;
        if (sqlite3_open_v2(path.u8string().c_str(), &db_, flags, nullptr) != SQLITE_OK) {
            fail("cannot open suggestion database");
            sqlite3_close(db_);
            db_ = nullptr;
            return;
        }
        sqlite3_busy_timeout(db_, 5000);
        if (writable && !exec(
            "CREATE TABLE IF NOT EXISTS task_suggestions ("
            "source_session_id TEXT NOT NULL, id TEXT NOT NULL, "
            "kind TEXT NOT NULL, dedupe_key TEXT NOT NULL, "
            "record TEXT NOT NULL, PRIMARY KEY(source_session_id,id), "
            "UNIQUE(source_session_id,kind,dedupe_key));")) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }
    ~Database() {
        if (db_) {
            if (transaction_) sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(db_);
        }
    }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    sqlite3* get() const { return db_; }
    explicit operator bool() const { return db_ != nullptr; }
    bool exec(const char* sql) {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK) return true;
        fail("suggestion transaction failed");
        return false;
    }
    bool begin() {
        transaction_ = exec("BEGIN IMMEDIATE;");
        return transaction_;
    }
    bool commit() {
        if (!exec("COMMIT;")) return false;
        transaction_ = false;
        return true;
    }
    void fail(const char* prefix) const {
        set_error(error_, std::string(prefix) + ": " + (db_ ? sqlite3_errmsg(db_) : "unavailable"));
    }
private:
    sqlite3* db_ = nullptr;
    std::string* error_ = nullptr;
    bool transaction_ = false;
};

class Statement {
public:
    Statement(Database& db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db.get(), sql, -1, &statement_, nullptr) != SQLITE_OK) {
            db_.fail("cannot prepare suggestion query");
        }
    }
    ~Statement() { if (statement_) sqlite3_finalize(statement_); }
    explicit operator bool() const { return statement_ != nullptr; }
    sqlite3_stmt* get() const { return statement_; }
    bool bind(int index, const std::string& value) {
        if (sqlite3_bind_text(statement_, index, value.data(),
                              static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK) return true;
        db_.fail("cannot bind suggestion query");
        return false;
    }
private:
    Database& db_;
    sqlite3_stmt* statement_ = nullptr;
};

std::optional<json> read_row(sqlite3_stmt* stmt, std::string* error) {
    const auto* value = sqlite3_column_text(stmt, 0);
    if (!value) {
        set_error(error, "suggestion record is missing");
        return std::nullopt;
    }
    auto record = json::parse(reinterpret_cast<const char*>(value), nullptr, false);
    if (record.is_discarded() || !record.is_object() ||
        !record.contains("id") || !record["id"].is_string() ||
        !record.contains("source_session_id") || !record["source_session_id"].is_string() ||
        !record.contains("dedupe_key") || !record["dedupe_key"].is_string() ||
        !record.contains("status") || !record["status"].is_string() ||
        !validate_record(record, error)) {
        set_error(error, "suggestion record is corrupt");
        return std::nullopt;
    }
    return record;
}

std::optional<json> select_one(Database& db, const std::string& source,
                               const std::string& id, std::string* error) {
    Statement query(db,
        "SELECT record FROM task_suggestions WHERE source_session_id=? AND id=?;");
    if (!query || !query.bind(1, source) || !query.bind(2, id)) return std::nullopt;
    const int rc = sqlite3_step(query.get());
    if (rc == SQLITE_ROW) return read_row(query.get(), error);
    if (rc != SQLITE_DONE) db.fail("cannot read suggestion");
    return std::nullopt;
}

std::vector<json> select_all(Database& db, const std::string* source,
                             std::string* error) {
    Statement query(db, source
        ? "SELECT record FROM task_suggestions WHERE source_session_id=? ORDER BY rowid;"
        : "SELECT record FROM task_suggestions ORDER BY rowid;");
    if (!query || (source && !query.bind(1, *source))) return {};
    std::vector<json> records;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(query.get())) == SQLITE_ROW) {
        auto record = read_row(query.get(), error);
        if (!record) return {};
        records.push_back(std::move(*record));
    }
    if (rc != SQLITE_DONE) {
        db.fail("cannot list suggestions");
        return {};
    }
    return records;
}

bool write_record(Database& db, const json& record, bool insert) {
    Statement query(db, insert
        ? "INSERT INTO task_suggestions(record,source_session_id,id,kind,dedupe_key) VALUES(?,?,?,?,?);"
        : "UPDATE task_suggestions SET record=? WHERE source_session_id=? AND id=?;");
    if (!query || !query.bind(1, record.dump()) ||
        !query.bind(2, record.at("source_session_id").get<std::string>()) ||
        !query.bind(3, record.at("id").get<std::string>())) return false;
    if (insert && (!query.bind(4, record.at("kind").get<std::string>()) ||
                   !query.bind(5, record.at("dedupe_key").get<std::string>()))) return false;
    if (sqlite3_step(query.get()) == SQLITE_DONE) return true;
    db.fail("cannot save suggestion");
    return false;
}

std::string dedupe_key(const json& record) {
    if (record["kind"] == "context_handoff") return "context_handoff";
    auto key = record.value("dedupe_key", std::string{});
    if (!key.empty()) return key;
    key = record.at("title").get<std::string>();
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return key;
}

} // namespace

std::uint64_t count_successful_compactions(const std::vector<ChatMessage>& raw_messages) {
    std::uint64_t count = 0;
    std::set<std::string> seen;
    for (const auto& message : raw_messages) {
        std::optional<CompactCheckpoint> checkpoint;
        try {
            checkpoint = decode_compact_checkpoint(message);
        } catch (const json::exception&) {
            continue;
        }
        if (!checkpoint) continue;
        if (checkpoint->window_number == 0 && !checkpoint->window_id.empty() &&
            checkpoint->first_window_id == checkpoint->window_id &&
            checkpoint->previous_window_id.empty()) {
            count = 0;
            seen.clear();
            if (!checkpoint->id.empty()) seen.insert(checkpoint->id);
            continue;
        }
        if ((checkpoint->trigger != "auto" && checkpoint->trigger != "manual") ||
            checkpoint->summary.empty()) continue;
        if (!checkpoint->id.empty() && !seen.insert(checkpoint->id).second) continue;
        if (count < std::numeric_limits<std::uint64_t>::max()) ++count;
    }
    return count;
}

TaskSuggestionStore::TaskSuggestionStore(std::filesystem::path project_dir)
    : db_path_(database_path_for_project(project_dir)) {}

std::filesystem::path TaskSuggestionStore::database_path_for_project(
    const std::filesystem::path& project_dir) {
    return project_dir / "task_suggestions.sqlite3";
}

std::vector<json> TaskSuggestionStore::list(const std::string& source,
                                           std::string* error) const {
    if (error) error->clear();
    if (!valid_scope(source, error)) return {};
    Database db(db_path_, false, error);
    return db ? select_all(db, &source, error) : std::vector<json>{};
}

std::optional<json> TaskSuggestionStore::get(const std::string& source,
                                            const std::string& id,
                                            std::string* error) const {
    if (error) error->clear();
    if (!valid_scope(source, error)) return std::nullopt;
    Database db(db_path_, false, error);
    return db ? select_one(db, source, id, error) : std::nullopt;
}

std::optional<json> TaskSuggestionStore::propose(const std::string& source,
                                                json draft,
                                                std::string* error) const {
    std::string local_error;
    if (!error) error = &local_error;
    if (error) error->clear();
    if (!valid_scope(source, error) || !validate_record(draft, error)) return std::nullopt;
    Database db(db_path_, true, error);
    if (!db || !db.begin()) return std::nullopt;
    const auto key = dedupe_key(draft);
    auto records = select_all(db, &source, error);
    if (!error->empty()) return std::nullopt;
    std::size_t pending = 0;
    for (const auto& record : records) {
        if (record.value("kind", "") == draft["kind"] &&
            record.value("dedupe_key", "") == key) {
            return std::optional<json>(std::in_place, record);
        }
        const auto status = record.value("status", "pending");
        if (record.value("kind", "") == "side_task" &&
            status != "started" && status != "dismissed") ++pending;
    }
    if (draft["kind"] == "side_task" && pending >= kMaxPendingSideTasks) {
        set_error(error, "at most three unresolved side-task suggestions are allowed per session");
        return std::nullopt;
    }
    draft["id"] = generate_uuid_v7();
    draft["source_session_id"] = source;
    draft["dedupe_key"] = key;
    draft["status"] = "pending";
    draft["target_session_id"] = "";
    draft["location"] = "";
    draft["error"] = "";
    draft["created_at_ms"] = now_ms();
    draft["updated_at_ms"] = draft["created_at_ms"];
    if (!draft.contains("evidence")) draft["evidence"] = json::array();
    if (!write_record(db, draft, true) || !db.commit()) return std::nullopt;
    return draft;
}

TaskSuggestionClaimResult TaskSuggestionStore::claim(const std::string& source,
                                                    const std::string& id,
                                                    const std::string& location,
                                                    std::string* error) const {
    if (error) error->clear();
    TaskSuggestionClaimResult result;
    if (!valid_scope(source, error)) return result;
    if (location != "worktree" && location != "current_branch") {
        set_error(error, "location must be worktree or current_branch");
        return result;
    }
    Database db(db_path_, true, error);
    if (!db || !db.begin()) return result;
    auto record = select_one(db, source, id, error);
    if (!record) {
        if (!error || error->empty()) set_error(error, "suggestion not found");
        return result;
    }
    const auto previous_location = record->value("location", "");
    if ((!previous_location.empty() && previous_location != location) ||
        (record->value("kind", "") == "context_handoff" && location != "current_branch")) {
        set_error(error, "suggestion execution location cannot be changed");
        return result;
    }
    const auto status = record->value("status", "pending");
    if (status == "dismissed") {
        set_error(error, "suggestion was dismissed");
        return result;
    }
    if (status != "pending" && status != "failed") {
        result.suggestion = std::move(record);
        return result;
    }
    (*record)["location"] = location;
    if (record->value("target_session_id", "").empty()) {
        (*record)["target_session_id"] = SessionStorage::generate_session_id();
    }
    (*record)["status"] = "queued";
    (*record)["error"] = "";
    (*record)["updated_at_ms"] = now_ms();
    if (!write_record(db, *record, false) || !db.commit()) return result;
    result.suggestion = std::move(record);
    result.claimed = true;
    return result;
}

std::optional<json> TaskSuggestionStore::update(
    const std::string& source, const std::string& id,
    const std::function<bool(json&)>& mutate, std::string* error) const {
    if (error) error->clear();
    if (!valid_scope(source, error) || !mutate) return std::nullopt;
    Database db(db_path_, true, error);
    if (!db || !db.begin()) return std::nullopt;
    auto record = select_one(db, source, id, error);
    if (!record) {
        if (!error || error->empty()) set_error(error, "suggestion not found");
        return std::nullopt;
    }
    const auto previous = *record;
    try {
        if (!mutate(*record)) return std::optional<json>(std::in_place, previous);
        for (const auto* key : {"id", "source_session_id", "kind", "dedupe_key", "created_at_ms"}) {
            if (!record->contains(key) || (*record)[key] != previous[key]) {
                set_error(error, "suggestion identity cannot be changed");
                return std::nullopt;
            }
        }
        for (const auto* key : {"target_session_id", "location"}) {
            const auto fixed = previous.value(key, std::string{});
            if (!fixed.empty() && (!record->contains(key) || (*record)[key] != fixed)) {
                set_error(error, "accepted suggestion target and location cannot be changed");
                return std::nullopt;
            }
        }
        if (!validate_record(*record, error)) return std::nullopt;
        (*record)["updated_at_ms"] = now_ms();
        if (!write_record(db, *record, false) || !db.commit()) return std::nullopt;
    } catch (const std::exception& exception) {
        set_error(error, std::string("suggestion update failed: ") + exception.what());
        return std::nullopt;
    }
    return record;
}

std::optional<json> TaskSuggestionStore::dismiss(const std::string& source,
                                                 const std::string& id,
                                                 std::string* error) const {
    bool refused = false;
    auto record = update(source, id, [&](json& value) {
        const auto status = value.value("status", "pending");
        if (status == "dismissed") return false;
        if (status == "starting") {
            refused = true;
            return false;
        }
        value["status"] = "dismissed";
        return true;
    }, error);
    if (refused) {
        set_error(error, "a task that is starting cannot be dismissed yet");
        return std::nullopt;
    }
    return record;
}

std::vector<json> TaskSuggestionStore::recoverable(std::string* error) const {
    if (error) error->clear();
    Database db(db_path_, false, error);
    if (!db) return {};
    auto records = select_all(db, nullptr, error);
    records.erase(std::remove_if(records.begin(), records.end(), [](const json& value) {
        const auto status = value.value("status", "pending");
        return status != "queued" && status != "starting";
    }), records.end());
    return records;
}

bool TaskSuggestionStore::erase_source(const std::string& source,
                                        std::string* error) const {
    if (error) error->clear();
    if (!valid_scope(source, error)) return false;
    std::error_code exists_error;
    if (!std::filesystem::exists(db_path_, exists_error)) {
        if (!exists_error) return true;
        set_error(error, "cannot inspect suggestion database: " + exists_error.message());
        return false;
    }
    Database db(db_path_, true, error);
    if (!db || !db.exec("PRAGMA secure_delete=ON;") || !db.begin()) return false;
    Statement query(db, "DELETE FROM task_suggestions WHERE source_session_id=?;");
    if (!query || !query.bind(1, source)) return false;
    if (sqlite3_step(query.get()) != SQLITE_DONE) {
        db.fail("cannot delete source suggestions");
        return false;
    }
    return db.commit();
}

std::optional<json> TaskSuggestionStore::propose_continuation(
    const std::string& source, const std::vector<ChatMessage>& raw_messages,
    std::uint64_t threshold, json launch_context, std::string* error) const {
    if (error) error->clear();
    if (threshold == 0) return std::nullopt;
    const auto count = count_successful_compactions(raw_messages);
    if (count < threshold) return std::nullopt;
    json draft = {
        {"kind", "context_handoff"},
        {"title", "Continue in a new session"},
        {"description", "This conversation has been compacted " + std::to_string(count) +
            " times. Continue with a fresh context and a handoff of the current work."},
        {"prompt", "Continue the current task from the referenced source conversation. "
            "Preserve its user requirements, decisions, completed work and remaining steps. "
            "Verify the live working directory before acting and read source history on demand."},
        {"successful_compactions", count},
        {"launch_context", std::move(launch_context)},
    };
    return propose(source, std::move(draft), error);
}

} // namespace acecode
