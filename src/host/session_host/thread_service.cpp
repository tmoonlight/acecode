#include "thread_service.hpp"
#include "session/scoped_subscription.hpp"
#include "thread_wait_state.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "session/compact_checkpoint.hpp"
#include "session/global_session_catalog.hpp"
#include "session/session_manager.hpp"
#include "session/session_pin_store.hpp"
#include "session_registry.hpp"
#include "session/session_storage.hpp"
#include "session/session_tree_purge.hpp"
#include "session/session_user_message_search.hpp"
#include "session/thread_repair.hpp"
#include "llm/message_predicates.hpp"
#include "llm/token_estimate.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace acecode {

namespace {

using nlohmann::json;

constexpr std::size_t kMaxListLimit = 50;
constexpr std::size_t kMaxTurnLimit = 20;
constexpr std::size_t kMaxItemChars = 8000;
constexpr std::size_t kSummaryChars = 240;
constexpr std::size_t kMaxWaitTargets = 8;
constexpr int kMaxWaitMs = 120000;

struct ScopedThread {
    SessionMeta meta;
    std::shared_ptr<SessionEntry> active;
    SessionManager* caller_manager = nullptr;
    std::string project_dir;
    bool is_caller = false;

    SessionManager* manager() const {
        if (caller_manager) return caller_manager;
        return active ? active->sm.get() : nullptr;
    }
};

std::string trim_and_limit(std::string value,
                           std::size_t max_chars,
                           bool* truncated = nullptr) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    const std::string limited = truncate_utf8_prefix(value, max_chars);
    if (truncated) *truncated = limited.size() < value.size();
    return limited;
}

bool valid_title(const std::string& title) {
    if (title.size() > 160) return false;
    return std::none_of(title.begin(), title.end(), [](unsigned char ch) {
        return ch < 0x20 && ch != '\t';
    });
}

std::string effective_thread_id(const ThreadScope& scope,
                                const std::string& thread_id) {
    return thread_id.empty() ? scope.caller_thread_id : thread_id;
}

bool active_matches_scope(const SessionEntry& entry,
                          const ThreadScope& scope) {
    if (scope.cwd.empty()) return false;
    if (entry.cwd == scope.cwd) return true;
    const std::string expected_hash =
        SessionStorage::compute_project_hash(scope.cwd);
    return !expected_hash.empty() && entry.workspace_hash == expected_hash;
}

SessionMeta synthesize_meta(const SessionEntry& entry) {
    SessionMeta meta;
    const std::string now = SessionStorage::now_iso8601();
    meta.id = entry.id;
    meta.cwd = entry.cwd;
    meta.created_at = now;
    meta.updated_at = now;
    const auto model_state = entry.model_binding
        ? entry.model_binding->state_snapshot()
        : SessionModelState{};
    meta.provider = model_state.provider;
    meta.model = model_state.model;
    meta.model_preset = model_state.name;
    meta.parent_session_id = entry.parent_session_id;
    meta.no_workspace = entry.no_workspace;
    if (entry.sm) {
        meta.title = entry.sm->current_title();
        meta.title_source = entry.sm->current_title_source();
        meta.turn_count = entry.sm->current_turn_count();
        meta.permission_mode = entry.sm->current_permission_mode();
    }
    return meta;
}

std::optional<ScopedThread> resolve_thread(
    const ThreadService::Deps& deps,
    const ThreadScope& scope,
    const std::string& requested_id) {
    const std::string thread_id = effective_thread_id(scope, requested_id);
    if (scope.cwd.empty() || thread_id.empty()) return std::nullopt;

    ScopedThread scoped;
    scoped.project_dir = SessionStorage::get_project_dir(scope.cwd);
    scoped.is_caller = thread_id == scope.caller_thread_id;
    if (scoped.is_caller && scope.caller_manager) {
        scoped.caller_manager = scope.caller_manager;
        scoped.meta = scope.caller_manager->load_session_meta(thread_id);
    }

    if (deps.registry) {
        auto active = deps.registry->acquire(thread_id);
        if (active && active_matches_scope(*active, scope)) {
            scoped.active = std::move(active);
        }
    }

    if (scoped.meta.id.empty()) {
        scoped.meta = SessionStorage::read_meta(
            SessionStorage::meta_path(scoped.project_dir, thread_id));
    }
    if (scoped.meta.id.empty() && scoped.active) {
        scoped.meta = synthesize_meta(*scoped.active);
    }
    if (scoped.meta.id.empty() && scoped.caller_manager &&
        scoped.caller_manager->current_session_id() == thread_id) {
        scoped.meta.id = thread_id;
        scoped.meta.cwd = scope.cwd;
        scoped.meta.created_at = SessionStorage::now_iso8601();
        scoped.meta.updated_at = scoped.meta.created_at;
        scoped.meta.title = scoped.caller_manager->current_title();
        scoped.meta.title_source =
            scoped.caller_manager->current_title_source();
        scoped.meta.turn_count =
            scoped.caller_manager->current_turn_count();
    }
    if (scoped.meta.id.empty()) return std::nullopt;
    return scoped;
}

std::string projects_directory() {
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / "projects");
}

std::string storage_workspace_hash(const std::string& project_dir) {
    return path_to_utf8(path_from_utf8(project_dir).filename());
}

bool valid_storage_token(const std::string& token) {
    return !token.empty() && std::all_of(token.begin(), token.end(),
        [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        });
}

// Read-only lookup. Exact metadata paths avoid loading every session's history
// just to resolve one id. workspaceHash also disambiguates imported ids.
std::optional<ScopedThread> resolve_discovered_thread(
    const ThreadService::Deps& deps,
    const ThreadScope& scope,
    const std::string& requested_id,
    const std::string& workspace_hash,
    std::string& error) {
    const std::string id = effective_thread_id(scope, requested_id);
    if (!valid_storage_token(id) ||
        (!workspace_hash.empty() && !valid_storage_token(workspace_hash))) {
        error = "invalid threadId or workspaceHash";
        return std::nullopt;
    }

    std::optional<ScopedThread> result;
    auto retain = [&](ScopedThread candidate) {
        if (!workspace_hash.empty() &&
            storage_workspace_hash(candidate.project_dir) != workspace_hash) {
            return true;
        }
        if (result && storage_workspace_hash(result->project_dir) !=
                          storage_workspace_hash(candidate.project_dir)) {
            error = "threadId matches multiple ACECode workspaces; pass "
                    "workspaceHash from list_threads";
            return false;
        }
        if (!result || candidate.active || candidate.caller_manager) {
            result = std::move(candidate);
        }
        return true;
    };
    auto read_project = [&](const std::string& project_dir) {
        ScopedThread candidate;
        candidate.project_dir = project_dir;
        candidate.meta = SessionStorage::read_meta(
            SessionStorage::meta_path(project_dir, id));
        return candidate.meta.id != id || retain(std::move(candidate));
    };

    const auto root = path_from_utf8(projects_directory());
    if (!workspace_hash.empty()) {
        if (!read_project(path_to_utf8(root / workspace_hash))) return std::nullopt;
    } else {
        std::error_code ec;
        std::filesystem::directory_iterator it(root, ec), end;
        if (ec == std::errc::no_such_file_or_directory) ec.clear();
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code item_ec;
            if (it->is_directory(item_ec) &&
                !read_project(path_to_utf8(it->path()))) return std::nullopt;
            if (item_ec) { ec = item_ec; break; }
        }
        if (ec) {
            error = "failed to discover ACECode thread: " + ec.message();
            return std::nullopt;
        }
    }

    if (deps.registry) {
        if (auto active = deps.registry->acquire(id)) {
            ScopedThread candidate;
            candidate.project_dir = active->sm
                ? active->sm->current_project_dir() : std::string{};
            if (candidate.project_dir.empty()) {
                candidate.project_dir = path_to_utf8(root /
                    (active->workspace_hash.empty()
                        ? SessionStorage::compute_project_hash(active->cwd)
                        : active->workspace_hash));
            }
            candidate.meta = active->sm
                ? active->sm->load_session_meta(id) : SessionMeta{};
            if (candidate.meta.id.empty()) candidate.meta = synthesize_meta(*active);
            candidate.active = std::move(active);
            if (!retain(std::move(candidate))) return std::nullopt;
        }
    }
    if (id == scope.caller_thread_id && scope.caller_manager) {
        ScopedThread candidate;
        candidate.project_dir = scope.caller_manager->current_project_dir();
        if (candidate.project_dir.empty() && !scope.cwd.empty()) {
            candidate.project_dir = SessionStorage::get_project_dir(scope.cwd);
        }
        if (!candidate.project_dir.empty()) {
            candidate.meta = scope.caller_manager->load_session_meta(id);
            if (candidate.meta.id.empty()) {
                candidate.meta.id = id;
                candidate.meta.cwd = scope.cwd;
                candidate.meta.title = scope.caller_manager->current_title();
            }
            candidate.caller_manager = scope.caller_manager;
            candidate.is_caller = true;
            if (result && storage_workspace_hash(result->project_dir) ==
                              storage_workspace_hash(candidate.project_dir)) {
                candidate.active = result->active;
            }
            if (!retain(std::move(candidate))) return std::nullopt;
        }
    }
    if (!result) {
        error = "thread not found in ACECode sessions";
    } else if (id == scope.caller_thread_id && !scope.cwd.empty() &&
               storage_workspace_hash(result->project_dir) ==
                   SessionStorage::compute_project_hash(scope.cwd)) {
        result->is_caller = true;
    }
    return result;
}

std::vector<ChatMessage> load_thread_messages(const ScopedThread& scoped) {
    if (auto* manager = scoped.manager()) {
        auto messages = manager->load_active_messages();
        if (!messages.empty()) return messages;
    }
    const auto files = SessionStorage::find_session_files(
        scoped.project_dir, scoped.meta.id);
    if (files.empty()) return {};
    return SessionStorage::load_messages(files.front().jsonl_path);
}

std::string message_display_content(const ChatMessage& message) {
    if (message.metadata.is_object() &&
        message.metadata.contains("display_text") &&
        message.metadata["display_text"].is_string()) {
        return message.metadata["display_text"].get<std::string>();
    }
    return message.content;
}

bool hidden_message(const ChatMessage& message) {
    if (message.is_meta || is_compact_checkpoint_message(message)) return true;
    return message.metadata.is_object() &&
           message.metadata.value("hidden_goal_context", false);
}

std::optional<std::size_t> parse_cursor_offset(const std::string& cursor) {
    if (cursor.empty()) return std::size_t{0};
    if (!std::all_of(cursor.begin(), cursor.end(), [](unsigned char ch) {
            return ch >= '0' && ch <= '9';
        })) return std::nullopt;
    try {
        std::size_t consumed = 0;
        const auto value = std::stoull(cursor, &consumed, 10);
        if (consumed != cursor.size() ||
            value > std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(value);
    } catch (...) {
        return std::nullopt;
    }
}

json thread_summary(const SessionMeta& meta,
                    const SessionInfo* active,
                    bool pinned,
                    int pinned_index = 0) {
    const bool is_active = active != nullptr;
    const bool busy = active && active->busy;
    json out{
        {"threadId", meta.id},
        {"cwd", meta.cwd},
        {"noWorkspace", meta.no_workspace},
        {"title", active && !active->title.empty() ? active->title : meta.title},
        {"summary", trim_and_limit(
            active && !active->summary.empty() ? active->summary : meta.summary,
            kSummaryChars)},
        {"createdAt", active && !active->created_at.empty()
                          ? active->created_at : meta.created_at},
        {"updatedAt", active && !active->updated_at.empty()
                          ? active->updated_at : meta.updated_at},
        {"active", is_active},
        {"busy", busy},
        {"status", busy ? "running" : "idle"},
        {"archived", meta.archived},
        {"pinned", pinned},
        {"parentThreadId", meta.parent_session_id.empty()
                               ? json(nullptr) : json(meta.parent_session_id)},
        {"model", active && !active->model_name.empty()
                      ? active->model_name : meta.model_preset},
    };
    if (pinned_index > 0) out["pinnedIndex"] = pinned_index;
    return out;
}

std::string event_kind_name(SessionEventKind kind) {
    return to_string(kind);
}


json compact_wait_event(const SessionEvent& event) {
    json out{
        {"cursor", std::to_string(event.seq)},
        {"type", event_kind_name(event.kind)},
    };
    if (!event.payload.is_object()) return out;
    for (const char* key : {
             "busy", "outcome", "reason", "request_id", "title"}) {
        if (event.payload.contains(key)) out[key] = event.payload[key];
    }
    if (event.kind == SessionEventKind::Message) {
        bool truncated = false;
        out["role"] = event.payload.value("role", std::string{});
        out["content"] = trim_and_limit(
            event.payload.value("content", std::string{}),
            1200, &truncated);
        out["truncated"] = truncated;
    }
    return out;
}

std::vector<ChatMessage> completed_history_for_fork(
    std::vector<ChatMessage> messages,
    bool source_busy) {
    if (!source_busy) return messages;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (!is_real_user_message(*it)) continue;
        messages.erase(std::prev(it.base()), messages.end());
        break;
    }
    return messages;
}

} // namespace

ThreadServiceResult ThreadServiceResult::ok(json value) {
    ThreadServiceResult result;
    result.success = true;
    result.value = std::move(value);
    return result;
}

ThreadServiceResult ThreadServiceResult::fail(std::string error) {
    ThreadServiceResult result;
    result.error = std::move(error);
    return result;
}

ThreadService::ThreadService(Deps deps) : deps_(deps) {}

ThreadServiceResult ThreadService::list(const ThreadScope& scope,
                                        std::size_t limit,
                                        const std::string& cursor,
                                        bool include_archived) const {
    const auto offset = parse_cursor_offset(cursor);
    if (!offset) return ThreadServiceResult::fail("invalid thread cursor");
    limit = (std::max)(std::size_t{1},
                       (std::min)(limit, kMaxListLimit));
    GlobalSessionCatalogOptions options;
    options.include_archived = include_archived;
    options.include_subagents = true;
    const auto catalog = build_global_session_catalog(
        projects_directory(), deps_.registry
            ? deps_.registry->list_active() : std::vector<SessionInfo>{}, options);

    auto summary = [&](const GlobalSessionCatalogEntry& entry, bool pinned,
                       int pinned_index = 0) {
        auto out = thread_summary(entry.meta,
            entry.active ? &*entry.active : nullptr, pinned, pinned_index);
        out["workspaceHash"] = storage_workspace_hash(entry.project_dir);
        out["workspaceName"] = entry.workspace_name;
        out["workspaceVisible"] = entry.workspace_visible;
        if (scope.caller_manager && entry.meta.id == scope.caller_thread_id &&
            entry.project_dir == scope.caller_manager->current_project_dir()) {
            out["active"] = true;
        }
        return out;
    };

    // The project directory is part of identity: imported ids may coincide
    // across workspaces, and each project owns its own pin order.
    std::map<std::string,
             std::unordered_map<std::string, const GlobalSessionCatalogEntry*>>
        projects;
    for (const auto& entry : catalog.entries) {
        projects[entry.project_dir].emplace(entry.meta.id, &entry);
    }
    std::unordered_set<const GlobalSessionCatalogEntry*> pinned_entries;
    json pinned_threads = json::array();
    int pinned_index = 0;
    for (const auto& [project_dir, entries] : projects) {
        const auto pins = session_pins::read_pinned_sessions_state(
            path_from_utf8(project_dir) / "pinned_sessions.json");
        for (const auto& id : pins.session_ids) {
            const auto found = entries.find(id);
            if (found == entries.end() || found->second->meta.archived ||
                !pinned_entries.insert(found->second).second) continue;
            pinned_threads.push_back(summary(*found->second, true, ++pinned_index));
        }
    }

    std::vector<const GlobalSessionCatalogEntry*> regular;
    for (const auto& entry : catalog.entries) {
        if (!pinned_entries.count(&entry)) regular.push_back(&entry);
    }
    const std::size_t start = (std::min)(*offset, regular.size());
    const std::size_t count = (std::min)(limit, regular.size() - start);
    const std::size_t next = start + count;
    const bool has_more = next < regular.size();
    json threads = json::array();
    for (std::size_t i = start; i < next; ++i) {
        threads.push_back(summary(*regular[i], false));
    }
    json errors = json::array();
    for (const auto& error : catalog.errors) {
        errors.push_back(json{
            {"workspaceHash", error.workspace_hash},
            {"stage", error.stage}, {"message", error.message},
        });
    }
    return ThreadServiceResult::ok(json{
        {"pinnedThreads", std::move(pinned_threads)},
        {"threads", std::move(threads)},
        {"hasMore", has_more},
        {"nextCursor", has_more ? json(std::to_string(next)) : json(nullptr)},
        {"errors", std::move(errors)},
    });
}

ThreadServiceResult ThreadService::read(
    const ThreadScope& scope,
    const std::string& requested_id,
    const std::string& cursor,
    std::size_t turn_limit,
    bool include_outputs,
    std::size_t max_chars_per_item,
    const std::string& workspace_hash) const {
    std::string error;
    auto scoped = resolve_discovered_thread(
        deps_, scope, requested_id, workspace_hash, error);
    if (!scoped) return ThreadServiceResult::fail(std::move(error));
    const auto offset = parse_cursor_offset(cursor);
    if (!offset) return ThreadServiceResult::fail("invalid thread cursor");
    turn_limit = (std::max)(std::size_t{1},
                            (std::min)(turn_limit, kMaxTurnLimit));
    max_chars_per_item = (std::max)(std::size_t{256},
        (std::min)(max_chars_per_item, kMaxItemChars));

    struct Turn {
        std::string id;
        std::vector<ChatMessage> messages;
    };
    std::vector<Turn> turns;
    for (const auto& message : load_thread_messages(*scoped)) {
        if (hidden_message(message)) continue;
        if (is_real_user_message(message)) {
            turns.push_back({message.uuid, {message}});
        } else if (!turns.empty()) {
            turns.back().messages.push_back(message);
        }
    }

    json output_turns = json::array();
    const std::size_t available =
        *offset < turns.size() ? turns.size() - *offset : 0;
    const std::size_t count = (std::min)(turn_limit, available);
    for (std::size_t page_index = 0; page_index < count; ++page_index) {
        const auto& turn = turns[turns.size() - 1 - *offset - page_index];
        json items = json::array();
        for (const auto& message : turn.messages) {
            const bool output = message.role == "tool";
            if (output && !include_outputs) continue;
            if (message.content.empty() && output) continue;
            if (message.content.empty() && message.role != "user") continue;
            bool truncated = false;
            json item{
                {"role", message.role},
                {"content", trim_and_limit(
                    message_display_content(message),
                    max_chars_per_item, &truncated)},
                {"truncated", truncated},
            };
            if (!message.uuid.empty()) item["id"] = message.uuid;
            if (!message.timestamp.empty()) item["timestamp"] = message.timestamp;
            if (output && !message.tool_call_id.empty()) {
                item["toolCallId"] = message.tool_call_id;
            }
            items.push_back(std::move(item));
        }
        output_turns.push_back(json{
            {"turnId", turn.id},
            {"items", std::move(items)},
        });
    }

    const std::size_t next_offset = *offset + count;
    const bool has_more = next_offset < turns.size();
    bool busy = false;
    std::uint64_t event_cursor = 0;
    if (scoped->active && scoped->active->loop) {
        busy = scoped->active->loop->is_busy();
        event_cursor = scoped->active->loop->events().current_seq();
    }
    return ThreadServiceResult::ok(json{
        {"threadId", scoped->meta.id},
        {"workspaceHash", storage_workspace_hash(scoped->project_dir)},
        {"cwd", scoped->meta.cwd},
        {"noWorkspace", scoped->meta.no_workspace},
        {"title", scoped->manager()
                      ? scoped->manager()->current_title()
                      : scoped->meta.title},
        {"status", busy ? "running" : "idle"},
        {"active", scoped->active != nullptr || scoped->caller_manager != nullptr},
        {"busy", busy},
        {"cursor", std::to_string(event_cursor)},
        {"turns", std::move(output_turns)},
        {"nextCursor", has_more
                           ? json(std::to_string(next_offset)) : json(nullptr)},
    });
}

ThreadServiceResult ThreadService::wait(
    const ThreadScope& scope,
    const std::vector<ThreadWaitTarget>& targets,
    int timeout_ms,
    const std::atomic<bool>* abort_flag) const {
    if (targets.empty() || targets.size() > kMaxWaitTargets) {
        return ThreadServiceResult::fail(
            "wait_threads requires one to eight targets");
    }
    if (!deps_.client) {
        return ThreadServiceResult::fail("thread event client is unavailable");
    }
    timeout_ms = (std::max)(0, (std::min)(timeout_ms, kMaxWaitMs));

    using State = thread_detail::WaitTargetState;
    using WaitState = thread_detail::WaitState;
    // The caller owns the wait; listeners hold only a weak reference.
    auto waiting = std::make_shared<WaitState>();
    auto& states = waiting->states;
    states.reserve(targets.size());
    json errors = json::array();
    std::unordered_set<std::string> seen;
    for (const auto& target : targets) {
        std::string error;
        auto scoped = resolve_discovered_thread(
            deps_, scope, target.thread_id, target.workspace_hash, error);
        if (target.thread_id.empty() || !scoped) {
            errors.push_back(json{
                {"threadId", target.thread_id},
                {"workspaceHash", target.workspace_hash},
                {"error", target.thread_id.empty() ? "threadId is required" : error},
            });
            continue;
        }
        if (scoped->is_caller) {
            errors.push_back(json{
                {"threadId", target.thread_id},
                {"error", "the calling thread cannot wait for itself"},
            });
            continue;
        }
        const auto hash = storage_workspace_hash(scoped->project_dir);
        if (!seen.insert(hash + ':' + target.thread_id).second) {
            errors.push_back(json{
                {"threadId", target.thread_id},
                {"error", "duplicate thread target"},
            });
            continue;
        }
        State state;
        state.target = target;
        state.target.workspace_hash = hash;
        state.cursor = target.after_cursor;
        state.active = scoped->active;
        state.terminal = !state.active || !state.active->loop ||
                         !state.active->loop->is_busy();
        if (state.active && state.active->loop) {
            state.cursor = (std::max)(
                state.cursor, state.active->loop->events().current_seq());
        }
        states.push_back(std::move(state));
    }
    if (states.empty()) {
        return ThreadServiceResult::fail(
            errors.empty() ? "no valid wait target" : errors.dump());
    }

    auto& mu = waiting->mu;
    auto& cv = waiting->cv;
    auto& ready = waiting->ready;
    ready = std::any_of(states.begin(), states.end(),
                             [](const State& state) {
                                 return state.terminal;
                             });
    std::vector<ScopedSubscription> subscriptions;
    subscriptions.reserve(states.size());
    const std::weak_ptr<WaitState> weak = waiting;
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (!states[i].active || !states[i].active->loop) continue;
        const auto subscription = deps_.client->subscribe(
            states[i].target.thread_id,
            [weak, i](const SessionEvent& event) {
                auto active = weak.lock();
                if (!active) return;
                std::lock_guard<std::mutex> lock(active->mu);
                auto& states = active->states;
                states[i].cursor = (std::max)(states[i].cursor, event.seq);
                if (event.seq > states[i].target.after_cursor) {
                    states[i].event = compact_wait_event(event);
                }
                if (thread_detail::wakes_wait(event.kind, event.payload)) {
                    states[i].terminal = true;
                    active->ready = true;
                    active->cv.notify_all();
                }
            },
            states[i].target.after_cursor);
        subscriptions.emplace_back(*deps_.client, states[i].target.thread_id, subscription);
        if (subscription == 0) {
            std::lock_guard<std::mutex> lock(mu);
            states[i].terminal = true;
            states[i].event = json{{"type", "unavailable"}};
            ready = true;
        }
    }

    bool timed_out = false;
    bool aborted = false;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    {
        std::unique_lock<std::mutex> lock(mu);
        while (!ready) {
            if (abort_flag && abort_flag->load()) {
                aborted = true;
                break;
            }
            if (timeout_ms == 0 ||
                std::chrono::steady_clock::now() >= deadline) {
                timed_out = true;
                break;
            }
            cv.wait_for(lock, std::chrono::milliseconds(100));
        }
    }

    // Wait for admitted listeners without holding their state mutex.
    subscriptions.clear();

    json snapshots = json::array();
    std::string winner;
    {
        std::lock_guard<std::mutex> lock(mu);
        for (auto& state : states) {
            bool busy = false;
            bool active = state.active && state.active->loop;
            if (active) {
                busy = state.active->loop->is_busy();
                state.cursor = (std::max)(
                    state.cursor,
                    state.active->loop->events().current_seq());
            }
            if (winner.empty() && state.terminal) {
                winner = state.target.thread_id;
            }
            json snapshot{
                {"threadId", state.target.thread_id},
                {"workspaceHash", state.target.workspace_hash},
                {"cursor", std::to_string(state.cursor)},
                {"active", active},
                {"busy", busy},
                {"status", busy ? "running" : "idle"},
            };
            if (!state.event.is_null() && !state.event.empty()) {
                snapshot["event"] = state.event;
            }
            snapshots.push_back(std::move(snapshot));
        }
    }
    return ThreadServiceResult::ok(json{
        {"threads", std::move(snapshots)},
        {"winnerThreadId", winner.empty() ? json(nullptr) : json(winner)},
        {"timedOut", timed_out},
        {"aborted", aborted},
        {"errors", std::move(errors)},
    });
}

ThreadServiceResult ThreadService::create(
    const ThreadScope& scope,
    const std::string& prompt,
    const std::string& title,
    const std::string& model_name) const {
    if (!deps_.registry || !deps_.client || scope.cwd.empty()) {
        return ThreadServiceResult::fail("thread creation is unavailable");
    }
    if (prompt.empty()) return ThreadServiceResult::fail("prompt is required");
    if (!valid_title(title)) return ThreadServiceResult::fail("invalid title");

    SessionOptions options;
    options.cwd = scope.cwd;
    options.model_name = model_name;
    std::string id;
    try {
        id = deps_.registry->create(options);
    } catch (const std::exception& error) {
        return ThreadServiceResult::fail(
            std::string("failed to create thread: ") + error.what());
    }
    auto entry = deps_.registry->acquire(id);
    if (!entry || !entry->sm ||
        entry->sm->ensure_active_session_id().empty()) {
        deps_.client->destroy_session(id);
        return ThreadServiceResult::fail("failed to persist new thread");
    }
    if (!title.empty()) entry->sm->set_session_title(title);
    if (!deps_.client->send_input(id, prompt)) {
        deps_.client->destroy_session(id);
        std::string ignored;
        SessionStorage::purge_session_files(
            SessionStorage::get_project_dir(scope.cwd), id, &ignored);
        return ThreadServiceResult::fail("failed to queue initial prompt");
    }
    return ThreadServiceResult::ok(json{
        {"threadId", id},
        {"title", title},
        {"status", "running"},
    });
}

ThreadServiceResult ThreadService::fork(
    const ThreadScope& scope,
    const std::string& requested_id) const {
    const std::string source_id = effective_thread_id(scope, requested_id);
    auto scoped = resolve_thread(deps_, scope, source_id);
    if (!scoped) {
        return ThreadServiceResult::fail(
            "source thread is unavailable in the current workspace");
    }
    if (!scoped->manager()) {
        SessionOptions options;
        options.cwd = scope.cwd;
        if (!deps_.client ||
            !deps_.client->resume_session(source_id, options)) {
            return ThreadServiceResult::fail("failed to resume source thread");
        }
        scoped = resolve_thread(deps_, scope, source_id);
    }
    if (!scoped || !scoped->manager()) {
        return ThreadServiceResult::fail("source thread manager is unavailable");
    }

    const bool busy = scoped->is_caller ||
        (scoped->active && scoped->active->loop &&
         scoped->active->loop->is_busy());
    auto messages = completed_history_for_fork(
        load_thread_messages(*scoped), busy);
    const std::string source_title = scoped->manager()->current_title();
    const std::string title = source_title.empty()
        ? "Fork" : source_title + " (fork)";
    const std::string new_id = scoped->manager()->fork_session_to_new_id(
        messages, title, source_id, {});
    if (new_id.empty()) return ThreadServiceResult::fail("failed to write fork");

    SessionOptions options;
    options.cwd = scope.cwd;
    if (deps_.client && !deps_.client->resume_session(new_id, options)) {
        return ThreadServiceResult::fail(
            "fork was written but could not be activated");
    }
    return ThreadServiceResult::ok(json{
        {"threadId", new_id},
        {"forkedFrom", source_id},
        {"title", title},
        {"status", "idle"},
    });
}

ThreadServiceResult ThreadService::send(
    const ThreadScope& scope,
    const std::string& thread_id,
    const std::string& prompt) const {
    if (thread_id.empty()) return ThreadServiceResult::fail("threadId is required");
    if (thread_id == scope.caller_thread_id) {
        return ThreadServiceResult::fail(
            "the calling thread cannot send a message to itself");
    }
    if (prompt.empty()) return ThreadServiceResult::fail("prompt is required");
    auto scoped = resolve_thread(deps_, scope, thread_id);
    if (!scoped) {
        return ThreadServiceResult::fail(
            "thread is unavailable in the current workspace");
    }
    if (!scoped->active) {
        SessionOptions options;
        options.cwd = scope.cwd;
        if (!deps_.client ||
            !deps_.client->resume_session(thread_id, options)) {
            return ThreadServiceResult::fail("failed to resume target thread");
        }
    }
    if (!deps_.client || !deps_.client->send_input(thread_id, prompt)) {
        return ThreadServiceResult::fail("failed to queue thread prompt");
    }
    return ThreadServiceResult::ok(json{
        {"threadId", thread_id},
        {"delivery", "queued"},
    });
}

ThreadServiceResult ThreadService::set_title(
    const ThreadScope& scope,
    const std::string& requested_id,
    const std::string& title) const {
    if (!valid_title(title)) return ThreadServiceResult::fail("invalid title");
    auto scoped = resolve_thread(deps_, scope, requested_id);
    if (!scoped) {
        return ThreadServiceResult::fail(
            "thread is unavailable in the current workspace");
    }
    if (auto* manager = scoped->manager()) {
        manager->set_session_title(title);
    } else {
        scoped->meta.title = title;
        scoped->meta.title_source = title.empty() ? "user-cleared" : "user";
        if (!SessionStorage::write_meta(
                SessionStorage::meta_path(
                    scoped->project_dir, scoped->meta.id),
                scoped->meta)) {
            return ThreadServiceResult::fail("failed to persist thread title");
        }
    }
    return ThreadServiceResult::ok(json{
        {"threadId", scoped->meta.id}, {"title", title},
    });
}

ThreadServiceResult ThreadService::set_pinned(
    const ThreadScope& scope,
    const std::string& thread_id,
    bool pinned) const {
    auto scoped = resolve_thread(deps_, scope, thread_id);
    if (!scoped || scoped->meta.archived) {
        return ThreadServiceResult::fail(
            "thread is unavailable for pinning");
    }
    const auto path = path_from_utf8(scoped->project_dir) /
        "pinned_sessions.json";
    auto state = session_pins::read_pinned_sessions_state(path);
    state.session_ids = pinned
        ? session_pins::pin_session_id(state.session_ids, scoped->meta.id)
        : session_pins::unpin_session_id(state.session_ids, scoped->meta.id);
    std::string error;
    if (!session_pins::write_pinned_sessions_state(path, state, &error)) {
        return ThreadServiceResult::fail(
            "failed to persist pin state: " + error);
    }
    return ThreadServiceResult::ok(json{
        {"threadId", scoped->meta.id}, {"pinned", pinned},
    });
}

ThreadServiceResult ThreadService::set_archived(
    const ThreadScope& scope,
    const std::string& requested_id,
    bool archived) const {
    auto scoped = resolve_thread(deps_, scope, requested_id);
    if (!scoped) {
        return ThreadServiceResult::fail(
            "thread is unavailable in the current workspace");
    }
    if (archived && !scoped->meta.parent_session_id.empty()) {
        // 子会话跟随主会话,没有单独归档(见 session_tree_purge.hpp)。
        return ThreadServiceResult::fail(
            "subagent threads follow their main thread and cannot be archived separately");
    }
    if (auto* manager = scoped->manager()) {
        manager->set_session_archived(archived);
    } else {
        scoped->meta.archived = archived;
        if (!SessionStorage::write_meta(
                SessionStorage::meta_path(
                    scoped->project_dir, scoped->meta.id),
                scoped->meta)) {
            return ThreadServiceResult::fail(
                "failed to persist archive state");
        }
    }

    if (archived) {
        const auto path = path_from_utf8(scoped->project_dir) /
            "pinned_sessions.json";
        auto state = session_pins::read_pinned_sessions_state(path);
        state.session_ids = session_pins::unpin_session_id(
            state.session_ids, scoped->meta.id);
        std::string error;
        if (!session_pins::write_pinned_sessions_state(
                path, state, &error)) {
            return ThreadServiceResult::fail(
                "archive persisted but pin cleanup failed: " + error);
        }
    }
    return ThreadServiceResult::ok(json{
        {"threadId", scoped->meta.id}, {"archived", archived},
    });
}

namespace {

json deleted_ids_json(const std::vector<std::string>& delete_order) {
    json deleted = json::array();
    for (const auto& id : delete_order) deleted.push_back(id);
    return deleted;
}

void destroy_active_threads(const ThreadService::Deps& deps,
                            const std::vector<std::string>& delete_order) {
    if (!deps.registry) return;
    for (const auto& id : delete_order) {
        if (!deps.registry->acquire(id)) continue;
        // Lifecycle tasks are owned and joined by the registry. Calling it
        // directly avoids retaining a SessionClient pointer whose stack
        // lifetime may end before SessionRegistry teardown joins the task.
        deps.registry->destroy(id);
    }
}

ThreadServiceResult purge_thread_delete_plan(
    const ThreadService::Deps& deps,
    const std::string& project_dir,
    const std::vector<std::string>& delete_order) {
    destroy_active_threads(deps, delete_order);

    SessionUserMessageIndex search_index(project_dir);
    for (const auto& id : delete_order) {
        std::string error;
        if (!SessionStorage::purge_session_files(project_dir, id, &error)) {
            return ThreadServiceResult::fail(
                "failed to delete thread " + id + ": " + error);
        }
        if (!search_index.remove_session(id, &error)) {
            return ThreadServiceResult::fail(
                "thread files deleted but search index cleanup failed for " +
                id + ": " + error);
        }
    }

    const auto pin_path = path_from_utf8(project_dir) /
        "pinned_sessions.json";
    auto pins = session_pins::read_pinned_sessions_state(pin_path);
    for (const auto& id : delete_order) {
        pins.session_ids = session_pins::unpin_session_id(
            pins.session_ids, id);
    }
    std::string pin_error;
    if (!session_pins::write_pinned_sessions_state(
            pin_path, pins, &pin_error)) {
        return ThreadServiceResult::fail(
            "threads deleted but pin cleanup failed: " + pin_error);
    }

    return ThreadServiceResult::ok(json{
        {"deletedThreadIds", deleted_ids_json(delete_order)},
    });
}

} // namespace

ThreadServiceResult ThreadService::delete_thread(
    const ThreadScope& scope,
    const std::string& thread_id) const {
    if (thread_id.empty()) return ThreadServiceResult::fail("threadId is required");
    auto target = resolve_thread(deps_, scope, thread_id);
    if (!target) {
        return ThreadServiceResult::fail(
            "thread is unavailable in the current workspace");
    }
    if (!target->meta.parent_session_id.empty()) {
        return ThreadServiceResult::fail(
            "subagent threads are deleted together with their main thread; "
            "delete the main thread instead");
    }

    const std::string project_dir = target->project_dir;
    const auto delete_order = session_tree_delete_order(project_dir, thread_id);
    const bool contains_caller =
        !scope.caller_thread_id.empty() &&
        std::find(delete_order.begin(), delete_order.end(),
                  scope.caller_thread_id) != delete_order.end();
    if (!contains_caller) {
        return purge_thread_delete_plan(deps_, project_dir, delete_order);
    }
    if (!scope.caller_manager) {
        return ThreadServiceResult::fail(
            "calling thread manager is unavailable for deferred deletion");
    }

    const bool caller_in_registry =
        deps_.registry &&
        static_cast<bool>(deps_.registry->acquire(scope.caller_thread_id));
    const auto deps = deps_;
    const std::string caller_id = scope.caller_thread_id;
    SessionManager* const caller_manager = scope.caller_manager;

    ThreadServiceResult scheduled = ThreadServiceResult::ok(json{
        {"deletedThreadIds", deleted_ids_json(delete_order)},
        {"scheduled", true},
    });
    scheduled.terminate_caller_after_turn = true;
    scheduled.post_turn_action =
        [deps, project_dir, delete_order, caller_id, caller_manager,
         caller_in_registry]() {
            auto delete_after_boundary =
                [deps, project_dir, delete_order, caller_id, caller_manager,
                 caller_in_registry]() {
                    if (!caller_in_registry && caller_manager) {
                        // The TUI root loop is not owned by its subagent
                        // registry. Its worker is already at the post-turn
                        // boundary, so releasing the writer here is safe.
                        caller_manager->end_current_session();
                    }
                    auto result = purge_thread_delete_plan(
                        deps, project_dir, delete_order);
                    if (!result.success) {
                        LOG_ERROR("[thread/delete] deferred deletion failed for " +
                                  caller_id + ": " + result.error);
                    } else {
                        LOG_INFO("[thread/delete] deferred deletion completed for " +
                                 caller_id);
                    }
                };

            if (caller_in_registry && deps.registry) {
                if (!deps.registry->enqueue_lifecycle_task(
                        std::move(delete_after_boundary))) {
                    LOG_ERROR("[thread/delete] failed to queue deferred deletion for " +
                              caller_id);
                }
                return;
            }
            delete_after_boundary();
        };
    return scheduled;
}

ThreadServiceResult ThreadService::repair(
    const ThreadScope& scope,
    const std::string& thread_id) const {
    if (thread_id.empty()) return ThreadServiceResult::fail("threadId is required");
    if (thread_id == scope.caller_thread_id) {
        return ThreadServiceResult::fail(
            "repair_thread cannot rewrite its own active tool turn; automatic "
            "overflow recovery handles the calling thread, or use another thread");
    }
    auto scoped = resolve_thread(deps_, scope, thread_id);
    if (!scoped) {
        return ThreadServiceResult::fail(
            "thread is unavailable in the current workspace");
    }

    ThreadRepairOptions options;
    options.trigger = "repair-manual";
    options.force_prune_one_group = true;

    if (scoped->active && scoped->active->loop && scoped->active->sm) {
        auto entry = scoped->active;
        if (entry->loop->is_busy() && deps_.client) {
            deps_.client->abort(thread_id);
        }
        auto result = std::make_shared<ThreadRepairResult>();
        auto receipt = deps_.registry->enqueue_entry_control(entry,
            [result, options](SessionRegistry&, SessionEntry& active) mutable {
                active.loop->history_on_worker([&](agent::ConversationHistory& history) {
                    options.target_tokens = estimate_message_tokens(history.view()) * 3 / 4;
                    *result = history.repair(active.sm.get(), options);
                });
                return result->status != ThreadRepairStatus::Failed;
            });
        if (!receipt.accepted ||
            !receipt.wait_for_completion(std::chrono::seconds(15))) {
            return ThreadServiceResult::fail(
                "target thread did not reach a repair boundary");
        }
        if (!receipt.succeeded() ||
            result->status == ThreadRepairStatus::Failed) {
            return ThreadServiceResult::fail(
                result->reason.empty()
                    ? "active thread repair failed" : result->reason);
        }
        return ThreadServiceResult::ok(
            thread_repair_result_to_json(*result, thread_id));
    }

    const auto files = SessionStorage::find_session_files(
        scoped->project_dir, thread_id);
    if (files.empty()) {
        return ThreadServiceResult::fail("thread JSONL file is unavailable");
    }
    auto loaded = SessionStorage::load_messages_with_diagnostics(
        files.front().jsonl_path);
    const int current_tokens = estimate_message_tokens(
        reconstruct_effective_model_history(loaded.messages));
    options.target_tokens = current_tokens * 3 / 4;
    auto result = plan_thread_repair(
        loaded.messages, options, loaded.diagnostics);
    if (result.repaired() && !SessionStorage::append_message(
            files.front().jsonl_path,
            encode_compact_checkpoint(result.checkpoint))) {
        return ThreadServiceResult::fail(
            "failed to append repair checkpoint");
    }
    return ThreadServiceResult::ok(
        thread_repair_result_to_json(result, thread_id));
}

} // namespace acecode
