#include "thread_service.hpp"

#include "compact_checkpoint.hpp"
#include "session_manager.hpp"
#include "session_pin_store.hpp"
#include "session_registry.hpp"
#include "session_storage.hpp"
#include "session_user_message_search.hpp"
#include "thread_repair.hpp"
#include "../commands/compact.hpp"
#include "../utils/encoding.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <functional>
#include <iterator>
#include <limits>
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
    meta.provider = entry.provider;
    meta.model = entry.model;
    meta.model_preset = entry.model_state.name;
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

bool wakes_wait(SessionEventKind kind, const json& payload) {
    if (kind == SessionEventKind::Done || kind == SessionEventKind::Error ||
        kind == SessionEventKind::PermissionRequest ||
        kind == SessionEventKind::QuestionRequest) {
        return true;
    }
    return kind == SessionEventKind::BusyChanged &&
           !payload.value("busy", true);
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
                                        std::size_t limit) const {
    if (scope.cwd.empty()) {
        return ThreadServiceResult::fail("thread workspace is unavailable");
    }
    limit = (std::max)(std::size_t{1},
                       (std::min)(limit, kMaxListLimit));
    const std::string project_dir =
        SessionStorage::get_project_dir(scope.cwd);

    std::unordered_map<std::string, SessionMeta> metas;
    for (auto meta : SessionStorage::list_sessions(project_dir)) {
        if (!meta.archived) metas.emplace(meta.id, std::move(meta));
    }

    std::unordered_map<std::string, SessionInfo> active_infos;
    if (deps_.registry) {
        for (auto info : deps_.registry->list_active()) {
            const bool matches = info.cwd == scope.cwd ||
                info.workspace_hash ==
                    SessionStorage::compute_project_hash(scope.cwd);
            if (!matches) continue;
            auto found = metas.find(info.id);
            if (found == metas.end()) {
                SessionMeta meta = SessionStorage::read_meta(
                    SessionStorage::meta_path(project_dir, info.id));
                if (!meta.id.empty() && meta.archived) continue;
                if (meta.id.empty()) {
                    meta.id = info.id;
                    meta.cwd = info.cwd;
                    meta.created_at = info.created_at;
                    meta.updated_at = info.updated_at;
                    meta.summary = info.summary;
                    meta.title = info.title;
                    meta.parent_session_id = info.parent_session_id;
                }
                metas.emplace(meta.id, std::move(meta));
            }
            active_infos.emplace(info.id, std::move(info));
        }
    }

    struct Row {
        SessionMeta meta;
        const SessionInfo* active = nullptr;
    };
    std::unordered_map<std::string, Row> rows_by_id;
    for (auto& [id, meta] : metas) {
        auto active = active_infos.find(id);
        rows_by_id.emplace(id, Row{
            meta, active == active_infos.end() ? nullptr : &active->second});
    }

    const auto pin_path = path_from_utf8(project_dir) /
        "pinned_sessions.json";
    const auto pins = session_pins::read_pinned_sessions_state(pin_path);
    std::unordered_set<std::string> pinned_ids;
    json pinned_threads = json::array();
    int pinned_index = 0;
    for (const auto& id : pins.session_ids) {
        auto found = rows_by_id.find(id);
        if (found == rows_by_id.end()) continue;
        pinned_ids.insert(id);
        pinned_threads.push_back(thread_summary(
            found->second.meta, found->second.active,
            true, ++pinned_index));
    }

    std::vector<const Row*> regular;
    regular.reserve(rows_by_id.size());
    for (const auto& [id, row] : rows_by_id) {
        if (!pinned_ids.count(id)) regular.push_back(&row);
    }
    std::sort(regular.begin(), regular.end(), [](const Row* lhs, const Row* rhs) {
        const std::string lhs_updated = lhs->active &&
            !lhs->active->updated_at.empty()
            ? lhs->active->updated_at : lhs->meta.updated_at;
        const std::string rhs_updated = rhs->active &&
            !rhs->active->updated_at.empty()
            ? rhs->active->updated_at : rhs->meta.updated_at;
        if (lhs_updated != rhs_updated) return lhs_updated > rhs_updated;
        return lhs->meta.id < rhs->meta.id;
    });

    json threads = json::array();
    for (std::size_t i = 0; i < regular.size() && i < limit; ++i) {
        threads.push_back(thread_summary(
            regular[i]->meta, regular[i]->active, false));
    }
    return ThreadServiceResult::ok(json{
        {"pinnedThreads", std::move(pinned_threads)},
        {"threads", std::move(threads)},
        {"hasMore", regular.size() > limit},
    });
}

ThreadServiceResult ThreadService::read(
    const ThreadScope& scope,
    const std::string& requested_id,
    const std::string& cursor,
    std::size_t turn_limit,
    bool include_outputs,
    std::size_t max_chars_per_item) const {
    auto scoped = resolve_thread(deps_, scope, requested_id);
    if (!scoped) {
        return ThreadServiceResult::fail(
            "thread is unavailable in the current workspace");
    }
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

    struct State {
        ThreadWaitTarget target;
        std::shared_ptr<SessionEntry> active;
        SessionClient::SubscriptionId subscription = 0;
        std::uint64_t cursor = 0;
        bool terminal = false;
        json event;
    };
    std::vector<State> states;
    states.reserve(targets.size());
    json errors = json::array();
    std::unordered_set<std::string> seen;
    for (const auto& target : targets) {
        if (target.thread_id.empty() || !seen.insert(target.thread_id).second) {
            errors.push_back(json{
                {"threadId", target.thread_id},
                {"error", "empty or duplicate threadId"},
            });
            continue;
        }
        if (target.thread_id == scope.caller_thread_id) {
            errors.push_back(json{
                {"threadId", target.thread_id},
                {"error", "the calling thread cannot wait for itself"},
            });
            continue;
        }
        auto scoped = resolve_thread(deps_, scope, target.thread_id);
        if (!scoped) {
            errors.push_back(json{
                {"threadId", target.thread_id},
                {"error", "thread not found"},
            });
            continue;
        }
        State state;
        state.target = target;
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

    std::mutex mu;
    std::condition_variable cv;
    bool ready = std::any_of(states.begin(), states.end(),
                             [](const State& state) {
                                 return state.terminal;
                             });
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (!states[i].active || !states[i].active->loop) continue;
        states[i].subscription = deps_.client->subscribe(
            states[i].target.thread_id,
            [&, i](const SessionEvent& event) {
                std::lock_guard<std::mutex> lock(mu);
                states[i].cursor = (std::max)(states[i].cursor, event.seq);
                if (event.seq > states[i].target.after_cursor) {
                    states[i].event = compact_wait_event(event);
                }
                if (wakes_wait(event.kind, event.payload)) {
                    states[i].terminal = true;
                    ready = true;
                    cv.notify_all();
                }
            },
            states[i].target.after_cursor);
        if (states[i].subscription == 0) {
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

    for (auto& state : states) {
        if (state.subscription != 0) {
            deps_.client->unsubscribe(
                state.target.thread_id, state.subscription);
        }
    }

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

std::vector<std::string> child_first_delete_order(
    const std::string& project_dir,
    const std::string& root_thread_id) {
    const auto metas = SessionStorage::list_sessions(project_dir);
    std::unordered_multimap<std::string, std::string> children;
    for (const auto& meta : metas) {
        if (!meta.parent_session_id.empty()) {
            children.emplace(meta.parent_session_id, meta.id);
        }
    }

    std::vector<std::string> delete_order;
    std::unordered_set<std::string> visited;
    std::function<void(const std::string&)> collect =
        [&](const std::string& id) {
            if (!visited.insert(id).second) return;
            const auto range = children.equal_range(id);
            for (auto it = range.first; it != range.second; ++it) {
                collect(it->second);
            }
            delete_order.push_back(id);
        };
    collect(root_thread_id);
    return delete_order;
}

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

    const std::string project_dir = target->project_dir;
    const auto delete_order = child_first_delete_order(project_dir, thread_id);
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
        auto receipt = entry->loop->enqueue_control(
            [entry, result, options]() mutable {
                auto& history = entry->loop->messages_mut();
                options.target_tokens =
                    estimate_message_tokens(history) * 3 / 4;
                *result = apply_thread_repair(
                    entry->sm.get(), history, options);
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
