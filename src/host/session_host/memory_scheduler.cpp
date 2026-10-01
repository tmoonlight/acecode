#include "memory_scheduler.hpp"

#include "memory_consolidation.hpp"
#include "memory_summary.hpp"

#include "agent/compaction/compact.hpp"
#include "memory/memory_frontmatter.hpp"
#include "memory/memory_inbox.hpp"
#include "memory/memory_paths.hpp"
#include "memory/secret_redaction.hpp"
#include "provider/copilot_provider.hpp"
#include "provider/cwd_model_override.hpp"
#include "provider/model_context_resolver.hpp"
#include "provider/model_resolver.hpp"
#include "provider/provider_factory.hpp"
#include "session/session_storage.hpp"
#include "utils/encoding.hpp"
#include "utils/joining_thread.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

constexpr std::int64_t kLeaseTtlMs = 15LL * 60 * 1000;
constexpr int kMaxSessionsPerTick = 2;
constexpr int kMaxAttempts = 3;
constexpr std::size_t kConsolidateCount = 20;
constexpr std::int64_t kConsolidateAgeMs = 24LL * 60 * 60 * 1000;
constexpr std::size_t kMaxBatchObservations = 100;

struct ExtractionCandidate {
    std::string session_id;
    std::string project_dir;
    std::string marker;
    std::int64_t next_index = 0;
    SessionMeta meta;
};

struct ExtractionOutcome {
    bool ok = false;
    std::string error;
    int observations = 0;
    bool touched_global = false;
    bool touched_workspace = false;
    std::string model_name;
};

struct ConsolidationOutcome {
    bool ran = false;
    bool ok = false;
    std::string error;
    int observations = 0;
    int changed = 0;
};

std::string read_text(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return ensure_utf8(oss.str());
}

std::string today(std::int64_t now_ms) {
    const std::time_t t = static_cast<std::time_t>(now_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

// 日志里只留模型输出的开头一段(按 UTF-8 字符边界截断),便于排查格式问题。
std::string output_sample(const std::string& output) {
    std::size_t cut = std::min<std::size_t>(output.size(), 400);
    while (cut > 0 && cut < output.size() && (static_cast<unsigned char>(output[cut]) & 0xC0) == 0x80) --cut;
    return output.substr(0, cut);
}

std::string activity_marker(const SessionMeta& meta) {
    return std::to_string(meta.message_count) + "@" + meta.updated_at;
}

std::optional<ModelProfile> find_saved_model(const AppConfig& cfg, const std::string& name) {
    if (name.empty()) return std::nullopt;
    const auto it = std::find_if(cfg.saved_models.begin(), cfg.saved_models.end(),
                                 [&name](const ModelProfile& p) { return p.name == name; });
    if (it == cfg.saved_models.end()) return std::nullopt;
    return *it;
}

// 摘要模型:memory.summary.model_name → 会话最后使用的模型 → 默认模型(D12)。
std::optional<ModelProfile> resolve_summary_profile(const AppConfig& cfg,
                                                    const std::optional<SessionMeta>& meta) {
    if (auto explicit_profile = find_saved_model(cfg, cfg.memory.summary.model_name)) {
        return explicit_profile;
    }
    std::optional<std::string> cwd_override;
    if (meta && !meta->cwd.empty()) cwd_override = load_cwd_model_override(meta->cwd);
    ModelProfile profile = resolve_effective_model(cfg, cwd_override, meta);
    if (profile.name.empty() && profile.model.empty()) return std::nullopt;
    return profile;
}

} // namespace

struct MemorySummaryScheduler::State {
    std::shared_ptr<MemoryService> memory;
    MemorySchedulerHost host;
    std::string owner = make_memory_lease_owner();
    std::mutex mu;
    std::condition_variable cv;
    bool stopping = false;
    std::deque<std::pair<std::string, std::string>> flush_queue;
    std::atomic<bool> abort{false};
    std::optional<fs::file_time_type> config_stamp;
    std::map<std::string, int> budget_divisor;  // PA 上下文超限后按会话把预算减半
    std::mutex work_mu;                          // 同一时刻最多一个提炼 / 整合
    JoiningThread thread;

    std::int64_t now() const {
        return host.now_ms ? host.now_ms() : memory_now_ms();
    }

    bool summary_enabled() const {
        const MemoryConfig cfg = memory->config();
        return cfg.enabled && cfg.summary.enabled;
    }

    void record_error(const std::string& error) {
        memory->state().set_status("last_error", error, now());
    }
};

namespace {

using State = MemorySummaryScheduler::State;

// config.json 的修改时间变了就重读 memory 段(D10),让别的进程改的设置一分钟内生效。
void reload_config_if_changed(State& state) {
    if (state.host.config_path.empty()) return;
    const fs::path path = path_from_utf8(state.host.config_path);
    std::error_code ec;
    const auto stamp_time = fs::last_write_time(path, ec);
    if (ec) return;
    if (state.config_stamp && stamp_time == *state.config_stamp) return;
    state.config_stamp = stamp_time;
    try {
        const auto j = nlohmann::json::parse(read_text(path));
        MemoryConfig fresh;
        if (j.contains("memory")) load_memory_config_json(j["memory"], fresh);
        state.memory->update_config(fresh);
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[memory] cannot reload memory settings: ") + e.what());
    }
}

AppConfig app_config(State& state) {
    AppConfig cfg = state.host.app_config ? state.host.app_config() : AppConfig{};
    cfg.memory = state.memory->config();
    return cfg;
}

// 无工具、非流式语义的一次模型调用;用 chat_stream 只为能在停止时中断。
std::string call_model(State& state, const std::optional<SessionMeta>& meta,
                       const std::string& system, const std::string& user,
                       std::string& model_name, std::string& error, bool& overflow) {
    const AppConfig cfg = app_config(state);
    if (state.host.complete) {
        model_name = cfg.memory.summary.model_name.empty()
            ? (meta ? meta->model_preset : std::string{}) : cfg.memory.summary.model_name;
        return state.host.complete(model_name, system, user, error, overflow);
    }
    const auto profile = resolve_summary_profile(cfg, meta);
    if (!profile) {
        error = "no model is configured for memory summarization";
        return {};
    }
    model_name = profile->name;
    auto provider = create_provider_from_entry(*profile, &cfg);
    if (!provider) {
        error = "cannot create the memory summarization provider";
        return {};
    }
    if (auto copilot = std::dynamic_pointer_cast<CopilotProvider>(provider)) copilot->try_silent_auth();
    ChatMessage system_msg;
    system_msg.role = "system";
    system_msg.content = system;
    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = user;
    std::string text;
    ProviderErrorInfo provider_error;
    std::string stream_error;
    provider->chat_stream({system_msg, user_msg}, {},
        [&text, &provider_error, &stream_error](const StreamEvent& event) {
            if (event.type == StreamEventType::Delta) text += event.content;
            if (event.type == StreamEventType::Error) {
                stream_error = event.error.empty() ? "provider error" : event.error;
                provider_error = event.provider_error;
            }
        },
        &state.abort);
    if (!stream_error.empty()) {
        error = stream_error;
        overflow = is_context_overflow_error(provider_error) || is_context_overflow_error(stream_error);
        return {};
    }
    return text;
}

int context_window_for(State& state, const std::optional<SessionMeta>& meta) {
    const AppConfig cfg = app_config(state);
    if (const auto profile = resolve_summary_profile(cfg, meta)) {
        return resolve_model_profile_context_window_nonblocking(cfg, *profile, cfg.context_window);
    }
    return cfg.context_window > 0 ? cfg.context_window : 128000;
}

std::vector<ExtractionCandidate> select_candidates(State& state, std::int64_t now) {
    std::vector<ExtractionCandidate> out;
    if (!state.host.project_dirs) return out;
    const MemoryConfig cfg = state.memory->config();
    const std::int64_t idle_ms = static_cast<std::int64_t>(cfg.summary.idle_minutes) * 60 * 1000;
    const std::int64_t max_age_ms =
        static_cast<std::int64_t>(cfg.summary.max_session_age_days) * 24 * 60 * 60 * 1000;
    std::set<std::string> seen_dirs;
    for (const auto& project_dir : state.host.project_dirs()) {
        if (project_dir.empty() || !seen_dirs.insert(project_dir).second) continue;
        const auto page = SessionStorage::list_session_metadata_page(project_dir, 40,
            [](const SessionMeta& meta) {
                return !meta.no_workspace && meta.parent_session_id.empty() &&
                       meta.surface != "headless" && meta.memory_mode != "off";
            });
        for (const auto& meta : page.sessions) {
            const auto updated = parse_memory_iso8601(meta.updated_at);
            if (!updated) continue;
            const std::int64_t updated_ms = *updated * 1000;
            if (now - updated_ms > max_age_ms) break;  // 降序:后面的更旧
            if (now - updated_ms < idle_ms) continue;
            if (state.host.session_busy && state.host.session_busy(meta.id)) continue;
            const auto progress = state.memory->state().extraction(meta.id);
            const std::string marker = activity_marker(meta);
            if (progress && progress->seen_marker == marker) continue;
            if (progress && progress->attempts >= kMaxAttempts && progress->failed_marker == marker) continue;
            out.push_back({meta.id, project_dir, marker,
                           progress ? progress->next_message_index : 0, meta});
            if (static_cast<int>(out.size()) >= kMaxSessionsPerTick) return out;
        }
    }
    return out;
}

ExtractionOutcome run_extraction(State& state, const ExtractionCandidate& candidate) {
    ExtractionOutcome outcome;
    MemoryStateStore& store = state.memory->state();
    const std::int64_t now = state.now();
    if (!store.try_acquire_lease(MemoryLeaseKind::Extraction, candidate.session_id, state.owner,
                                 kLeaseTtlMs, now)) {
        outcome.error = "session is being processed by another ACECode process";
        return outcome;
    }
    const auto messages = SessionStorage::load_messages(
        SessionStorage::session_path(candidate.project_dir, candidate.session_id));
    const std::int64_t to = static_cast<std::int64_t>(messages.size());
    const std::int64_t from = std::min(std::max<std::int64_t>(candidate.next_index, 0), to);
    const auto items = collect_memory_transcript(messages, static_cast<std::size_t>(from));
    if (items.empty()) {
        store.commit_extraction(candidate.session_id, candidate.project_dir, to, candidate.marker, now);
        store.release_lease(MemoryLeaseKind::Extraction, candidate.session_id, state.owner);
        outcome.ok = true;
        return outcome;
    }

    const std::optional<SessionMeta> meta = candidate.meta;
    const int divisor = std::max(1, state.budget_divisor[candidate.session_id]);
    const std::size_t window = static_cast<std::size_t>(std::max(context_window_for(state, meta), 8000));
    // 预算 = 摘要模型窗口的 60%;按约 3 字节 / token 换算(中文偏多字节)。
    const std::size_t budget = std::max<std::size_t>(window * 60 / 100 * 3 / divisor, 4000);
    const std::string transcript = render_memory_transcript(items, budget);

    std::string error;
    bool overflow = false;
    const std::string output = call_model(
        state, meta, memory_extraction_system_prompt(),
        memory_extraction_user_prompt(candidate.session_id, from, to, transcript),
        outcome.model_name, error, overflow);
    MemoryExtractionParse parsed;
    if (error.empty()) {
        parsed = parse_memory_extraction_output(output);
        if (!parsed.ok) {
            error = "invalid extraction output: " + parsed.error;
            LOG_WARN("[memory] " + error + "; output starts with: " + output_sample(output));
        }
    }
    if (!error.empty()) {
        if (overflow) state.budget_divisor[candidate.session_id] = divisor * 2;
        store.record_extraction_failure(candidate.session_id, candidate.project_dir, error,
                                        candidate.marker, state.now());
        state.record_error("extraction " + candidate.session_id + ": " + error);
        store.release_lease(MemoryLeaseKind::Extraction, candidate.session_id, state.owner);
        outcome.error = error;
        return outcome;
    }
    // 关闭了记忆摘要 / 使用记忆:进行中的结果丢弃,不落盘(8.7)。
    if (!state.summary_enabled()) {
        store.release_lease(MemoryLeaseKind::Extraction, candidate.session_id, state.owner);
        outcome.error = "memory summarization was turned off";
        return outcome;
    }

    const MemoryScope scopes[] = {MemoryScope::Global, MemoryScope::Workspace};
    for (MemoryScope scope : scopes) {
        auto registry = state.memory->scope(scope, candidate.project_dir);
        if (!registry) continue;
        MemoryObservationFile file;
        file.session_id = candidate.session_id;
        file.from = from;
        file.to = to;
        file.model = outcome.model_name;
        for (const auto& obs : parsed.observations) {
            if (obs.scope != scope) continue;
            MemoryObservation item;
            item.type = obs.type;
            item.title = redact_secrets(obs.title).text;
            item.statement = redact_secrets(obs.statement).text;
            file.observations.push_back(std::move(item));
        }
        if (file.observations.empty()) {
            // 重放时清掉上次可能写过的同范围文件,保证只留这一次的结果。
            remove_memory_observation_file(registry->dir(), candidate.session_id, from, to);
            continue;
        }
        std::string write_error;
        if (!write_memory_observation_file(registry->dir(), file, &write_error)) {
            store.record_extraction_failure(candidate.session_id, candidate.project_dir, write_error,
                                            candidate.marker, state.now());
            store.release_lease(MemoryLeaseKind::Extraction, candidate.session_id, state.owner);
            outcome.error = write_error;
            return outcome;
        }
        outcome.observations += static_cast<int>(file.observations.size());
        (scope == MemoryScope::Global ? outcome.touched_global : outcome.touched_workspace) = true;
    }
    // 观察都写成功后才推进提炼位置;崩溃发生在这之前,重放同一范围会覆盖同名文件。
    store.commit_extraction(candidate.session_id, candidate.project_dir, to, candidate.marker, state.now());
    store.set_status("last_extraction_ms", std::to_string(state.now()), state.now());
    store.release_lease(MemoryLeaseKind::Extraction, candidate.session_id, state.owner);
    state.budget_divisor.erase(candidate.session_id);
    outcome.ok = true;
    return outcome;
}

std::string batch_signature(const std::vector<MemoryObservationFile>& batch) {
    std::string joined;
    for (const auto& file : batch) joined += path_to_utf8(file.path.filename()) + "\n";
    return memory_plan_hash(joined);
}

ConsolidationOutcome run_consolidation(State& state, MemoryScope scope,
                                       const std::string& project_dir, bool force,
                                       const std::optional<SessionMeta>& model_hint) {
    ConsolidationOutcome outcome;
    auto registry = state.memory->scope(scope, project_dir);
    if (!registry) return outcome;
    const auto inbox = list_memory_inbox(registry->dir());
    if (inbox.empty()) return outcome;
    std::size_t total = 0;
    for (const auto& file : inbox) total += file.observations.size();
    const auto oldest = parse_memory_iso8601(inbox.front().created_at);
    const std::int64_t now = state.now();
    const bool due = total >= kConsolidateCount ||
                     (oldest && now - *oldest * 1000 >= kConsolidateAgeMs);
    if (!force && !due) return outcome;

    MemoryStateStore& store = state.memory->state();
    const std::string scope_key = registry->scope_key();
    if (!store.try_acquire_lease(MemoryLeaseKind::Consolidation, scope_key, state.owner, kLeaseTtlMs, now)) {
        return outcome;  // 另一个进程正在整合这个作用域
    }
    outcome.ran = true;
    const auto release = [&state, &store, &scope_key] {
        store.release_lease(MemoryLeaseKind::Consolidation, scope_key, state.owner);
    };
    const auto progress = store.consolidation(scope_key);

    // 崩溃后重放上次已校验、未应用完的计划,不再调用模型。
    if (progress && !progress->pending_plan.empty()) {
        try {
            const auto pending = nlohmann::json::parse(progress->pending_plan);
            const auto plan = parse_memory_consolidation_plan(pending.value("plan", std::string{}));
            std::set<std::string> files;
            for (const auto& name : pending.value("files", nlohmann::json::array())) {
                if (name.is_string()) files.insert(name.get<std::string>());
            }
            std::vector<MemoryObservationFile> batch;
            for (const auto& file : inbox) {
                if (files.count(path_to_utf8(file.path.filename()))) batch.push_back(file);
            }
            if (plan.ok) {
                const auto applied = apply_memory_plan(*state.memory, *registry, plan.operations, batch,
                                                       progress->pending_plan_hash, today(now), now, true);
                if (applied.ok) {
                    store.set_status("last_consolidation_ms", std::to_string(now), now);
                    for (const auto& file : batch) outcome.observations += static_cast<int>(file.observations.size());
                    outcome.changed = applied.changed_entries;
                    outcome.ok = true;
                    release();
                    return outcome;
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[memory] dropping unreadable pending plan: ") + e.what());
        }
        store.clear_consolidation(scope_key);
    }

    std::vector<MemoryObservationFile> batch;
    std::size_t batch_count = 0;
    for (const auto& file : inbox) {
        if (!batch.empty() && batch_count + file.observations.size() > kMaxBatchObservations) break;
        batch_count += file.observations.size();
        batch.push_back(file);
    }
    const std::string signature = batch_signature(batch);
    if (!force && progress && progress->attempts >= kMaxAttempts && progress->failed_batch == signature) {
        release();
        outcome.ran = false;
        return outcome;
    }

    registry->reload();
    const auto entries = registry->list();
    const auto tombstones = store.tombstones(scope_key, now);
    std::string error;
    bool overflow = false;
    std::string model_name;
    const std::string output = call_model(
        state, model_hint, memory_consolidation_system_prompt(),
        memory_consolidation_user_prompt(scope, entries, batch, tombstones), model_name, error, overflow);
    MemoryPlanParse plan;
    if (error.empty()) {
        plan = parse_memory_consolidation_plan(output);
        if (!plan.ok) {
            error = "invalid consolidation plan: " + plan.error;
            LOG_WARN("[memory] " + error + "; output starts with: " + output_sample(output));
        }
    }
    if (error.empty()) {
        MemoryPlanContext context;
        context.entries = entries;
        context.tombstones = tombstones;
        for (const auto& file : batch) {
            for (const auto& obs : file.observations) context.observation_ids.insert(obs.id);
        }
        const std::string invalid = validate_memory_plan(plan.operations, context);
        if (!invalid.empty()) error = "rejected consolidation plan: " + invalid;
    }
    if (error.empty() && !state.summary_enabled()) error = "memory summarization was turned off";
    if (!error.empty()) {
        store.record_consolidation_failure(scope_key, error, signature, state.now());
        state.record_error("consolidation " + memory_scope_to_string(scope) + ": " + error);
        outcome.error = error;
        release();
        return outcome;
    }

    nlohmann::json files = nlohmann::json::array();
    for (const auto& file : batch) files.push_back(path_to_utf8(file.path.filename()));
    const std::string plan_hash = memory_plan_hash(plan.canonical);
    store.set_pending_plan(scope_key, nlohmann::json{{"plan", plan.canonical}, {"files", files}}.dump(),
                           plan_hash, state.now());
    const auto applied = apply_memory_plan(*state.memory, *registry, plan.operations, batch,
                                           plan_hash, today(state.now()), state.now());
    if (!applied.ok) {
        store.record_consolidation_failure(scope_key, applied.error, signature, state.now());
        state.record_error("consolidation " + memory_scope_to_string(scope) + ": " + applied.error);
        outcome.error = applied.error;
        release();
        return outcome;
    }
    store.set_status("last_consolidation_ms", std::to_string(state.now()), state.now());
    outcome.ok = true;
    outcome.observations = static_cast<int>(batch_count);
    outcome.changed = applied.changed_entries;
    release();
    return outcome;
}

void housekeeping(State& state, std::int64_t now) {
    state.memory->state().purge_expired_tombstones(now);
    cleanup_memory_archive(state.memory->global().dir(), now / 1000);
    if (!state.host.project_dirs) return;
    for (const auto& project_dir : state.host.project_dirs()) {
        if (auto registry = state.memory->workspace(project_dir)) {
            cleanup_memory_archive(registry->dir(), now / 1000);
        }
    }
}

void tick_locked(State& state) {
    reload_config_if_changed(state);
    if (!state.summary_enabled()) return;
    const std::int64_t now = state.now();
    housekeeping(state, now);
    for (const auto& candidate : select_candidates(state, now)) {
        const auto outcome = run_extraction(state, candidate);
        if (!outcome.ok) continue;
        const std::optional<SessionMeta> hint = candidate.meta;
        if (outcome.touched_global) run_consolidation(state, MemoryScope::Global, candidate.project_dir, false, hint);
        if (outcome.touched_workspace) {
            run_consolidation(state, MemoryScope::Workspace, candidate.project_dir, false, hint);
        }
    }
    // 时间触发:最早一条观察超过 24 小时的作用域,即使这一轮没有新提炼也要整合。
    run_consolidation(state, MemoryScope::Global, {}, false, std::nullopt);
    if (state.host.project_dirs) {
        for (const auto& project_dir : state.host.project_dirs()) {
            run_consolidation(state, MemoryScope::Workspace, project_dir, false, std::nullopt);
        }
    }
}

std::string flush_locked(State& state, const std::string& session_id, const std::string& project_dir) {
    reload_config_if_changed(state);
    if (!state.summary_enabled()) return "Memory summarization is off; nothing was flushed.";
    const SessionMeta meta = SessionStorage::read_meta(SessionStorage::meta_path(project_dir, session_id));
    ExtractionCandidate candidate;
    candidate.session_id = session_id;
    candidate.project_dir = project_dir;
    candidate.meta = meta;
    candidate.marker = activity_marker(meta);
    if (const auto progress = state.memory->state().extraction(session_id)) {
        candidate.next_index = progress->next_message_index;
    }
    const auto extraction = run_extraction(state, candidate);
    const std::optional<SessionMeta> hint = meta;
    const auto global = run_consolidation(state, MemoryScope::Global, project_dir, true, hint);
    const auto workspace = run_consolidation(state, MemoryScope::Workspace, project_dir, true, hint);
    std::ostringstream out;
    out << "Memory flush finished: " << extraction.observations << " new observation(s), "
        << (global.observations + workspace.observations) << " observation(s) consolidated, "
        << (global.changed + workspace.changed) << " memory entr"
        << (global.changed + workspace.changed == 1 ? "y" : "ies") << " changed.";
    for (const std::string* error : {&extraction.error, &global.error, &workspace.error}) {
        if (!error->empty()) out << "\nProblem: " << *error;
    }
    return out.str();
}

void scheduler_loop(const std::shared_ptr<State>& state, StopToken token) {
    // 调度每 interval(默认 2 分钟)一轮,但 config.json 至少每分钟检查一次:
    // 规格要求别的进程改的设置一分钟内生效。
    const auto config_period = std::min<std::chrono::steady_clock::duration>(
        state->host.interval, std::chrono::seconds(60));
    auto next_tick = std::chrono::steady_clock::now() + state->host.interval;
    auto next_config = std::chrono::steady_clock::now() + config_period;
    while (!token.stop_requested()) {
        std::pair<std::string, std::string> flush;
        bool have_flush = false;
        {
            std::unique_lock<std::mutex> lock(state->mu);
            state->cv.wait_until(lock, std::min(next_tick, next_config), [&state] {
                return state->stopping || !state->flush_queue.empty();
            });
            if (state->stopping) return;
            if (!state->flush_queue.empty()) {
                flush = state->flush_queue.front();
                state->flush_queue.pop_front();
                have_flush = true;
            }
        }
        if (have_flush) {
            std::string text;
            {
                std::lock_guard<std::mutex> work(state->work_mu);
                text = flush_locked(*state, flush.first, flush.second);
            }
            LOG_INFO("[memory] flush for " + flush.first + ": " + text);
            if (state->host.notify) state->host.notify(flush.first, text);
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_config) {
            next_config = now + config_period;
            std::lock_guard<std::mutex> work(state->work_mu);
            reload_config_if_changed(*state);
        }
        if (now < next_tick) continue;
        next_tick = now + state->host.interval;
        try {
            std::lock_guard<std::mutex> work(state->work_mu);
            tick_locked(*state);
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[memory] summary tick failed: ") + e.what());
        }
    }
}

} // namespace

MemorySummaryStatus read_memory_summary_status(MemoryService& memory, const std::string& project_dir) {
    MemorySummaryStatus status;
    const MemoryConfig cfg = memory.config();
    status.enabled = cfg.enabled && cfg.summary.enabled;
    status.global_inbox = count_memory_inbox_observations(memory.global().dir());
    if (auto registry = memory.workspace(project_dir)) {
        status.workspace_inbox = count_memory_inbox_observations(registry->dir());
    }
    auto read_ms = [&memory](const char* key) -> std::int64_t {
        const auto value = memory.state().status(key);
        if (!value) return 0;
        try {
            return std::stoll(value->first);
        } catch (...) {
            return value->second;
        }
    };
    status.last_extraction_ms = read_ms("last_extraction_ms");
    status.last_consolidation_ms = read_ms("last_consolidation_ms");
    if (const auto error = memory.state().status("last_error")) {
        status.last_error = error->first;
        status.last_error_ms = error->second;
    }
    return status;
}

MemorySummaryScheduler::MemorySummaryScheduler(std::shared_ptr<MemoryService> memory,
                                               MemorySchedulerHost host)
    : state_(std::make_shared<State>()) {
    state_->memory = std::move(memory);
    state_->host = std::move(host);
}

MemorySummaryScheduler::~MemorySummaryScheduler() { stop(); }

void MemorySummaryScheduler::start() {
    if (state_->thread.joinable()) return;
    state_->thread = JoiningThread([state = state_](StopToken token) { scheduler_loop(state, token); });
}

void MemorySummaryScheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        state_->stopping = true;
    }
    state_->abort.store(true);
    state_->cv.notify_all();
    state_->thread.request_stop();
    state_->thread.join();
}

void MemorySummaryScheduler::tick() {
    std::lock_guard<std::mutex> work(state_->work_mu);
    tick_locked(*state_);
}

MemoryFlushReport MemorySummaryScheduler::request_flush(const std::string& session_id,
                                                        const std::string& project_dir) {
    MemoryFlushReport report;
    if (!state_->summary_enabled()) {
        report.message = "Memory summarization is off; nothing was flushed.";
        return report;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mu);
        state_->flush_queue.emplace_back(session_id, project_dir);
    }
    state_->cv.notify_all();
    report.started = true;
    report.message = "Started organizing memory: extracting this session and consolidating the "
                     "global and workspace memory. A notice will follow when it finishes.";
    return report;
}

std::string MemorySummaryScheduler::run_flush(const std::string& session_id,
                                              const std::string& project_dir) {
    std::lock_guard<std::mutex> work(state_->work_mu);
    return flush_locked(*state_, session_id, project_dir);
}

} // namespace acecode
