#include "global_session_catalog.hpp"

#include "../desktop/workspace_registry.hpp"
#include "../utils/cwd_hash.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace acecode {

namespace {

constexpr std::size_t kDefaultMaxWorkers = 8;
constexpr std::size_t kMaximumSelectionErrors = 100;

struct ProjectScanResult {
    std::vector<GlobalSessionCatalogEntry> entries;
    std::vector<std::string> archived_identities;
    std::vector<GlobalSessionCatalogError> errors;
    bool cancelled = false;
};

struct ProjectDiscoveryResult {
    std::vector<fs::path> paths;
    std::unordered_map<std::string, std::int64_t> directory_signatures;
    std::vector<GlobalSessionCatalogError> errors;
    bool cancelled = false;
};

std::string effective_updated_at(const GlobalSessionCatalogEntry& entry) {
    if (entry.active && !entry.active->updated_at.empty()) {
        return entry.active->updated_at;
    }
    return entry.meta.updated_at;
}

int content_score(const GlobalSessionCatalogEntry& entry) {
    return entry.content_match ? entry.content_match->score : 0;
}

SessionMeta synthesize_meta(const SessionInfo& active) {
    SessionMeta meta;
    meta.id = active.id;
    meta.cwd = active.cwd;
    meta.created_at = active.created_at;
    meta.updated_at = active.updated_at;
    meta.summary = active.summary;
    meta.provider = active.provider;
    meta.model = active.model;
    meta.model_preset = active.model_name;
    meta.title = active.title;
    meta.title_source = active.title_source;
    meta.message_count = active.message_count;
    meta.turn_count = active.turn_count;
    meta.permission_mode = active.permission_mode;
    meta.last_token_usage = active.last_token_usage;
    meta.session_token_usage = active.session_token_usage;
    meta.parent_session_id = active.parent_session_id;
    meta.expert_id = active.expert_id;
    meta.expert_member_id = active.expert_member_id;
    meta.no_workspace = active.no_workspace;
    meta.worktree.worktree_path = active.worktree_path;
    meta.worktree.worktree_name = active.worktree_name;
    meta.worktree.worktree_branch = active.worktree_branch;
    return meta;
}

GlobalSessionCatalogError make_error(const std::string& project_dir,
                                     const std::string& workspace_hash,
                                     const std::string& stage,
                                     const std::string& message) {
    return GlobalSessionCatalogError{project_dir, workspace_hash, stage, message};
}

ProjectDiscoveryResult discover_project_paths(
    const std::string& projects_dir,
    const std::function<bool()>& should_cancel = {}) {
    ProjectDiscoveryResult result;
    const fs::path root = path_from_utf8(projects_dir);
    std::error_code ec;
    fs::directory_iterator it(root, ec);
    const fs::directory_iterator end;
    if (ec) {
        result.errors.push_back(make_error(
            projects_dir, {}, "projects", ec.message()));
        return result;
    }
    for (; it != end; it.increment(ec)) {
        if (should_cancel && should_cancel()) {
            result.cancelled = true;
            return result;
        }
        if (ec) {
            result.errors.push_back(make_error(
                projects_dir, {}, "projects", ec.message()));
            break;
        }
        std::error_code item_ec;
        if (it->is_directory(item_ec) && !item_ec) {
            result.paths.push_back(it->path());
            std::error_code signature_ec;
            const auto signature = fs::last_write_time(
                it->path(), signature_ec).time_since_epoch().count();
            if (!signature_ec) {
                result.directory_signatures[path_to_utf8(it->path())] =
                    static_cast<std::int64_t>(signature);
            }
        } else if (item_ec) {
            result.errors.push_back(make_error(
                path_to_utf8(it->path()), {}, "projects", item_ec.message()));
        }
    }
    std::sort(result.paths.begin(), result.paths.end(), [&result](const fs::path& a,
                                                                  const fs::path& b) {
        const auto a_path = path_to_utf8(a);
        const auto b_path = path_to_utf8(b);
        const auto a_signature = result.directory_signatures.find(a_path);
        const auto b_signature = result.directory_signatures.find(b_path);
        const std::int64_t a_value = a_signature == result.directory_signatures.end()
            ? 0 : a_signature->second;
        const std::int64_t b_value = b_signature == result.directory_signatures.end()
            ? 0 : b_signature->second;
        if (a_value != b_value) return a_value > b_value;
        return a_path < b_path;
    });
    return result;
}

ProjectScanResult scan_project(const fs::path& project_path,
                               const GlobalSessionCatalogOptions& options) {
    ProjectScanResult result;
    if (options.should_cancel && options.should_cancel()) {
        result.cancelled = true;
        return result;
    }
    const std::string project_dir = path_to_utf8(project_path);
    const std::string workspace_hash = path_to_utf8(project_path.filename());
    const std::string projects_dir = path_to_utf8(project_path.parent_path());
    auto workspace = desktop::load_workspace_metadata(projects_dir, workspace_hash);
    if (workspace &&
        !desktop::workspace_hash_matches_cwd(workspace_hash, workspace->cwd)) {
        workspace.reset();
    }

    std::vector<SessionMeta> metas;
    try {
        metas = SessionStorage::list_session_metadata(
            project_dir, options.should_cancel);
        if (options.should_cancel && options.should_cancel()) {
            result.cancelled = true;
            return result;
        }
    } catch (const std::exception& e) {
        result.errors.push_back(make_error(
            project_dir, workspace_hash, "metadata", e.what()));
        return result;
    } catch (...) {
        result.errors.push_back(make_error(
            project_dir, workspace_hash, "metadata", "unknown project scan failure"));
        return result;
    }

    bool has_searchable_session = false;
    for (const auto& meta : metas) {
        if (!meta.archived && meta.parent_session_id.empty()) {
            has_searchable_session = true;
            break;
        }
    }

    std::unordered_map<std::string, SessionUserMessageSearchResult> matches_by_id;
    const std::string query = options.content_query.value_or(std::string{});
    if (has_searchable_session && !query.empty()) {
        SessionUserMessageIndex index(project_dir);
        std::string error;
        if (!index.ensure_project_indexed(&error)) {
            result.errors.push_back(make_error(
                project_dir,
                workspace_hash,
                "content-index",
                error.empty() ? "failed to refresh user-message index" : error));
        } else {
            const int per_project_limit = std::max(1, options.content_limit_per_project);
            auto matches = index.search(query, per_project_limit, &error);
            if (!error.empty()) {
                result.errors.push_back(make_error(
                    project_dir, workspace_hash, "content-search", error));
            } else {
                for (auto& match : matches) {
                    auto found = matches_by_id.find(match.session_id);
                    if (found == matches_by_id.end() || match.score > found->second.score) {
                        matches_by_id[match.session_id] = std::move(match);
                    }
                }
            }
        }
    }

    for (auto& meta : metas) {
        const std::string effective_hash = meta.no_workspace
            ? std::string{}
            : workspace_hash;
        const std::string identity = global_session_catalog_identity(
            meta.no_workspace, effective_hash, meta.id);
        if (meta.archived) {
            result.archived_identities.push_back(identity);
        }
        if (meta.archived || !meta.parent_session_id.empty()) continue;

        GlobalSessionCatalogEntry entry;
        entry.project_dir = project_dir;
        entry.workspace_hash = effective_hash;
        entry.meta = std::move(meta);
        if (!entry.meta.no_workspace) {
            entry.workspace_cwd = workspace && !workspace->cwd.empty()
                ? workspace->cwd
                : entry.meta.cwd;
            entry.workspace_name = workspace && !workspace->name.empty()
                ? workspace->name
                : desktop::default_workspace_name(entry.workspace_cwd);
            entry.workspace_visible = workspace && workspace->desktop_visible;
        }
        auto match = matches_by_id.find(entry.meta.id);
        if (match != matches_by_id.end()) {
            entry.content_match = std::move(match->second);
        }
        result.entries.push_back(std::move(entry));
    }
    return result;
}

std::size_t effective_worker_count(std::size_t requested, std::size_t project_count) {
    if (project_count == 0) return 0;
    std::size_t cap = requested == 0 ? kDefaultMaxWorkers : requested;
    cap = std::max<std::size_t>(1, cap);
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware > 0) cap = std::min<std::size_t>(cap, hardware);
    return std::min(cap, project_count);
}

bool prefer_candidate(const GlobalSessionCatalogEntry& candidate,
                      const GlobalSessionCatalogEntry& current) {
    const std::string candidate_updated = effective_updated_at(candidate);
    const std::string current_updated = effective_updated_at(current);
    if (candidate_updated != current_updated) return candidate_updated > current_updated;
    return candidate.project_dir < current.project_dir;
}

void retain_best_content_match(GlobalSessionCatalogEntry& target,
                               const GlobalSessionCatalogEntry& other) {
    if (!other.content_match) return;
    if (!target.content_match ||
        other.content_match->score > target.content_match->score) {
        target.content_match = other.content_match;
    }
}

GlobalSessionCatalog merge_project_results(
    const fs::path& root,
    std::vector<ProjectScanResult> project_results,
    const std::vector<SessionInfo>& active_sessions,
    bool prefer_content) {
    GlobalSessionCatalog catalog;
    std::unordered_set<std::string> archived_identities;
    std::unordered_map<std::string, std::size_t> index_by_identity;
    for (auto& project : project_results) {
        for (auto& error : project.errors) {
            catalog.errors.push_back(std::move(error));
        }
        for (auto& identity : project.archived_identities) {
            archived_identities.insert(std::move(identity));
        }
        for (auto& entry : project.entries) {
            const std::string identity = global_session_catalog_identity(
                entry.meta.no_workspace, entry.workspace_hash, entry.meta.id);
            auto found = index_by_identity.find(identity);
            if (found == index_by_identity.end()) {
                index_by_identity.emplace(identity, catalog.entries.size());
                catalog.entries.push_back(std::move(entry));
                continue;
            }
            auto& current = catalog.entries[found->second];
            if (prefer_candidate(entry, current)) {
                retain_best_content_match(entry, current);
                current = std::move(entry);
            } else {
                retain_best_content_match(current, entry);
            }
        }
    }

    for (const auto& active : active_sessions) {
        if (active.id.empty() || !active.parent_session_id.empty()) continue;
        const std::string workspace_hash = active.no_workspace
            ? std::string{}
            : (!active.workspace_hash.empty()
                ? active.workspace_hash
                : compute_cwd_hash(active.cwd));
        const std::string identity = global_session_catalog_identity(
            active.no_workspace, workspace_hash, active.id);
        if (archived_identities.count(identity)) continue;

        auto found = index_by_identity.find(identity);
        if (found != index_by_identity.end()) {
            catalog.entries[found->second].active = active;
            continue;
        }

        GlobalSessionCatalogEntry entry;
        entry.workspace_hash = workspace_hash;
        entry.meta = synthesize_meta(active);
        entry.active = active;
        if (!active.no_workspace) {
            entry.project_dir = path_to_utf8(root / workspace_hash);
            const auto workspace = desktop::load_workspace_metadata(
                path_to_utf8(root), workspace_hash);
            entry.workspace_cwd = workspace && !workspace->cwd.empty()
                ? workspace->cwd
                : active.cwd;
            entry.workspace_name = workspace && !workspace->name.empty()
                ? workspace->name
                : desktop::default_workspace_name(entry.workspace_cwd);
            entry.workspace_visible = workspace && workspace->desktop_visible;
        } else if (!active.cwd.empty()) {
            const std::string hash = compute_cwd_hash(active.cwd);
            entry.project_dir = path_to_utf8(root / hash);
        }
        index_by_identity.emplace(identity, catalog.entries.size());
        catalog.entries.push_back(std::move(entry));
    }

    std::sort(catalog.entries.begin(), catalog.entries.end(), [prefer_content](
        const GlobalSessionCatalogEntry& a,
        const GlobalSessionCatalogEntry& b) {
        if (prefer_content && content_score(a) != content_score(b)) {
            return content_score(a) > content_score(b);
        }
        const std::string au = effective_updated_at(a);
        const std::string bu = effective_updated_at(b);
        if (au != bu) return au > bu;
        if (a.meta.id != b.meta.id) return a.meta.id < b.meta.id;
        if (a.workspace_hash != b.workspace_hash) {
            return a.workspace_hash < b.workspace_hash;
        }
        return a.meta.cwd < b.meta.cwd;
    });
    std::sort(catalog.errors.begin(), catalog.errors.end(), [](
        const GlobalSessionCatalogError& a,
        const GlobalSessionCatalogError& b) {
        if (a.project_dir != b.project_dir) return a.project_dir < b.project_dir;
        if (a.stage != b.stage) return a.stage < b.stage;
        return a.message < b.message;
    });
    return catalog;
}

} // namespace

std::string global_session_catalog_identity(bool no_workspace,
    const std::string& workspace_hash,
    const std::string& session_id) {
    if (session_id.empty()) return {};
    if (no_workspace) return "no:" + session_id;
    return "ws:" + workspace_hash + ':' + session_id;
}

GlobalSessionCatalog build_global_session_catalog(
    const std::string& projects_dir,
    const std::vector<SessionInfo>& active_sessions,
    const GlobalSessionCatalogOptions& options) {
    const fs::path root = path_from_utf8(projects_dir);
    auto discovery = discover_project_paths(projects_dir);

    std::vector<ProjectScanResult> project_results(discovery.paths.size());
    const std::size_t worker_count = effective_worker_count(
        options.max_workers, discovery.paths.size());
    std::atomic<std::size_t> next_project{0};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            while (true) {
                if (options.should_cancel && options.should_cancel()) return;
                const std::size_t index = next_project.fetch_add(1);
                if (index >= discovery.paths.size()) return;
                project_results[index] = scan_project(discovery.paths[index], options);
                if (project_results[index].cancelled) return;
            }
        });
    }
    for (auto& worker : workers) worker.join();

    const bool prefer_content = options.content_query.has_value() &&
        !options.content_query->empty();
    auto catalog = merge_project_results(
        root, std::move(project_results), active_sessions, prefer_content);
    for (auto& error : discovery.errors) {
        catalog.errors.push_back(std::move(error));
    }
    return catalog;
}

struct GlobalSessionCatalogIndex::Impl {
    struct ScanJob {
        std::string project_dir;
        bool dirty = false;
    };

    std::string projects_dir;
    ActiveSessionsProvider active_sessions_provider;

    mutable std::mutex mu;
    std::condition_variable cv;
    std::thread worker;
    bool started = false;
    bool stopping = false;
    bool paused = false;
    bool ever_attached = false;
    bool discovery_ready = false;
    bool discovery_requested = true;
    bool scan_in_progress = false;
    std::uint64_t generation = 0;
    std::uint64_t project_generation = 0;
    std::chrono::steady_clock::time_point last_discovery_at{};

    std::unordered_set<std::string> active_requests;
    std::unordered_set<std::string> cancelled_requests;
    std::unordered_set<std::string> known_projects;
    std::unordered_set<std::string> scanned_projects;
    std::unordered_set<std::string> dirty_projects;
    std::unordered_map<std::string, std::int64_t> project_signatures;
    std::deque<std::string> pending_projects;
    std::deque<std::string> dirty_queue;
    std::unordered_map<std::string, std::shared_ptr<const ProjectScanResult>> shards;
    std::vector<GlobalSessionCatalogError> discovery_errors;

    explicit Impl(std::string root, ActiveSessionsProvider provider)
        : projects_dir(std::move(root)),
          active_sessions_provider(std::move(provider)) {}

    bool cancelled_or_stopping() const {
        std::lock_guard<std::mutex> lock(mu);
        return stopping || paused;
    }

    bool complete_locked() const {
        return discovery_ready && !discovery_requested &&
            scanned_projects.size() == known_projects.size() &&
            pending_projects.empty() &&
            dirty_queue.empty() &&
            !scan_in_progress;
    }

    static void retain_known(
        std::deque<std::string>& queue,
        const std::unordered_set<std::string>& known) {
        std::deque<std::string> retained;
        while (!queue.empty()) {
            auto item = std::move(queue.front());
            queue.pop_front();
            if (known.count(item)) retained.push_back(std::move(item));
        }
        queue = std::move(retained);
    }

    bool refresh_discovery() {
        auto discovery = discover_project_paths(
            projects_dir, [this] { return cancelled_or_stopping(); });
        if (discovery.cancelled) return false;

        std::vector<std::string> discovered_paths;
        discovered_paths.reserve(discovery.paths.size());
        for (const auto& path : discovery.paths) {
            discovered_paths.push_back(path_to_utf8(path));
        }
        std::unordered_set<std::string> discovered(
            discovered_paths.begin(), discovered_paths.end());

        std::lock_guard<std::mutex> lock(mu);
        if (stopping || paused) return false;
        bool changed = !discovery_ready;

        for (auto it = known_projects.begin(); it != known_projects.end();) {
            if (discovered.count(*it)) {
                ++it;
                continue;
            }
            const std::string removed = *it;
            it = known_projects.erase(it);
            scanned_projects.erase(removed);
            dirty_projects.erase(removed);
            project_signatures.erase(removed);
            shards.erase(removed);
            changed = true;
        }
        retain_known(pending_projects, known_projects);
        retain_known(dirty_queue, known_projects);

        for (auto& path : discovered_paths) {
            const auto signature_it = discovery.directory_signatures.find(path);
            const std::int64_t signature =
                signature_it == discovery.directory_signatures.end()
                ? 0
                : signature_it->second;
            if (known_projects.insert(path).second) {
                project_signatures[path] = signature;
                pending_projects.push_back(std::move(path));
                changed = true;
                continue;
            }
            const auto previous = project_signatures.find(path);
            const bool signature_changed = previous != project_signatures.end() &&
                previous->second != 0 && signature != 0 &&
                previous->second != signature;
            project_signatures[path] = signature;
            if (signature_changed && shards.count(path) &&
                dirty_projects.insert(path).second) {
                dirty_queue.push_back(path);
            }
        }

        discovery_errors = std::move(discovery.errors);
        discovery_ready = true;
        discovery_requested = false;
        last_discovery_at = std::chrono::steady_clock::now();
        if (changed) {
            ++generation;
            ++project_generation;
        }
        cv.notify_all();
        return true;
    }

    std::optional<ScanJob> take_job_locked() {
        if (!dirty_queue.empty()) {
            ScanJob job{std::move(dirty_queue.front()), true};
            dirty_queue.pop_front();
            dirty_projects.erase(job.project_dir);
            return job;
        }
        if (!pending_projects.empty()) {
            ScanJob job{std::move(pending_projects.front()), false};
            pending_projects.pop_front();
            return job;
        }
        return std::nullopt;
    }

    void restore_cancelled_job_locked(ScanJob job) {
        if (!known_projects.count(job.project_dir)) return;
        if (job.dirty) {
            if (dirty_projects.insert(job.project_dir).second) {
                dirty_queue.push_front(std::move(job.project_dir));
            }
        } else if (!scanned_projects.count(job.project_dir)) {
            pending_projects.push_front(std::move(job.project_dir));
        }
    }

    void run() {
        refresh_discovery();
        constexpr auto kDiscoveryInterval = std::chrono::seconds(5);

        while (true) {
            std::optional<ScanJob> job;
            bool discover = false;
            {
                std::unique_lock<std::mutex> lock(mu);
                if (stopping) return;
                if (paused) {
                    cv.wait(lock, [this] { return stopping || !paused; });
                    if (stopping) return;
                }

                if (discovery_requested || !discovery_ready) {
                    discover = true;
                } else {
                    job = take_job_locked();
                    if (job) {
                        scan_in_progress = true;
                    } else {
                        const bool signalled = cv.wait_for(
                            lock, kDiscoveryInterval, [this] {
                                return stopping || paused || discovery_requested ||
                                    !dirty_queue.empty() || !pending_projects.empty();
                            });
                        if (stopping) return;
                        if (paused) continue;
                        if (signalled) continue;
                        discover = true;
                    }
                }
            }

            if (discover) {
                refresh_discovery();
                continue;
            }
            if (!job) continue;

            GlobalSessionCatalogOptions options;
            options.should_cancel = [this] { return cancelled_or_stopping(); };
            auto result = scan_project(path_from_utf8(job->project_dir), options);

            {
                std::lock_guard<std::mutex> lock(mu);
                scan_in_progress = false;
                if (result.cancelled || stopping || paused) {
                    restore_cancelled_job_locked(std::move(*job));
                    cv.notify_all();
                    continue;
                }
                shards[job->project_dir] =
                    std::make_shared<ProjectScanResult>(std::move(result));
                scanned_projects.insert(job->project_dir);
                ++generation;
                cv.notify_all();
            }

            // Keep startup prewarming cooperative with the rest of the daemon.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

GlobalSessionCatalogIndex::GlobalSessionCatalogIndex(
    std::string projects_dir,
    ActiveSessionsProvider active_sessions_provider)
    : impl_(std::make_unique<Impl>(
          std::move(projects_dir), std::move(active_sessions_provider))) {}

GlobalSessionCatalogIndex::~GlobalSessionCatalogIndex() {
    stop();
}

void GlobalSessionCatalogIndex::start() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->started) return;
    impl_->started = true;
    impl_->stopping = false;
    impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
}

void GlobalSessionCatalogIndex::stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (!impl_->started) return;
        impl_->stopping = true;
        impl_->paused = false;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->started = false;
}

bool GlobalSessionCatalogIndex::attach_request(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!request_id.empty() &&
        impl_->cancelled_requests.count(request_id)) {
        return false;
    }
    const bool first_attach = !impl_->ever_attached;
    const bool resuming_after_pause = impl_->paused;
    impl_->ever_attached = true;
    // Legacy callers have no lifecycle id to detach later. They still resume
    // prewarming, but are not retained as an active request that could prevent
    // a later explicit cancellation from pausing the worker.
    if (!request_id.empty()) impl_->active_requests.insert(request_id);
    impl_->paused = false;
    constexpr auto kResumeDiscoveryInterval = std::chrono::seconds(1);
    const auto now = std::chrono::steady_clock::now();
    if (first_attach ||
        (resuming_after_pause &&
         (impl_->last_discovery_at.time_since_epoch().count() == 0 ||
          now - impl_->last_discovery_at >= kResumeDiscoveryInterval))) {
        impl_->discovery_requested = true;
    }
    impl_->cv.notify_all();
    return true;
}

bool GlobalSessionCatalogIndex::cancel_request(const std::string& request_id) {
    if (request_id.empty()) return false;
    std::lock_guard<std::mutex> lock(impl_->mu);
    const bool first_cancel = impl_->cancelled_requests.insert(request_id).second;
    impl_->active_requests.erase(request_id);
    if (impl_->ever_attached && impl_->active_requests.empty()) {
        impl_->paused = true;
    }
    impl_->cv.notify_all();
    return first_cancel;
}

bool GlobalSessionCatalogIndex::is_request_cancelled(
    const std::string& request_id) const {
    if (request_id.empty()) return false;
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->cancelled_requests.count(request_id) != 0;
}

void GlobalSessionCatalogIndex::invalidate_project(
    const std::string& workspace_hash) {
    if (workspace_hash.empty()) return;
    const std::string project_dir = path_to_utf8(
        path_from_utf8(impl_->projects_dir) / workspace_hash);
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->known_projects.count(project_dir)) {
        impl_->discovery_requested = true;
    } else if (impl_->shards.count(project_dir) &&
               impl_->dirty_projects.insert(project_dir).second) {
        impl_->dirty_queue.push_back(project_dir);
    }
    impl_->cv.notify_all();
}

void GlobalSessionCatalogIndex::request_discovery() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->discovery_requested = true;
    impl_->cv.notify_all();
}

GlobalSessionCatalogIndexSnapshot GlobalSessionCatalogIndex::snapshot() const {
    std::vector<ProjectScanResult> project_results;
    std::vector<GlobalSessionCatalogError> discovery_errors;
    GlobalSessionCatalogIndexSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        project_results.reserve(impl_->shards.size());
        for (const auto& [_, shard] : impl_->shards) {
            if (shard) project_results.push_back(*shard);
        }
        discovery_errors = impl_->discovery_errors;
        snapshot.progress.scanned_projects = impl_->scanned_projects.size();
        snapshot.progress.total_projects = impl_->known_projects.size();
        snapshot.progress.generation = impl_->generation;
        snapshot.progress.complete = impl_->complete_locked();
        snapshot.progress.paused = impl_->paused;
        snapshot.project_dirs.assign(
            impl_->known_projects.begin(), impl_->known_projects.end());
    }
    std::sort(snapshot.project_dirs.begin(), snapshot.project_dirs.end());

    std::vector<SessionInfo> active_sessions;
    if (impl_->active_sessions_provider) {
        try {
            active_sessions = impl_->active_sessions_provider();
        } catch (...) {
            discovery_errors.push_back(make_error(
                impl_->projects_dir, {}, "active-sessions",
                "failed to read active session snapshot"));
        }
    }
    snapshot.catalog = merge_project_results(
        path_from_utf8(impl_->projects_dir),
        std::move(project_results),
        active_sessions,
        false);
    for (auto& error : discovery_errors) {
        snapshot.catalog.errors.push_back(std::move(error));
    }
    std::sort(snapshot.catalog.errors.begin(), snapshot.catalog.errors.end(), [](
        const GlobalSessionCatalogError& a,
        const GlobalSessionCatalogError& b) {
        if (a.project_dir != b.project_dir) return a.project_dir < b.project_dir;
        if (a.stage != b.stage) return a.stage < b.stage;
        return a.message < b.message;
    });
    return snapshot;
}

GlobalSessionCatalogSelection GlobalSessionCatalogIndex::select_entries(
    std::size_t max_entries,
    const std::function<int(const GlobalSessionCatalogEntry&)>& score) const {
    std::vector<std::shared_ptr<const ProjectScanResult>> shard_views;
    GlobalSessionCatalogSelection selection;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        shard_views.reserve(impl_->shards.size());
        for (const auto& [_, shard] : impl_->shards) {
            if (shard) shard_views.push_back(shard);
        }
        selection.errors = impl_->discovery_errors;
        if (selection.errors.size() > kMaximumSelectionErrors) {
            selection.errors.resize(kMaximumSelectionErrors);
        }
        selection.progress.scanned_projects = impl_->scanned_projects.size();
        selection.progress.total_projects = impl_->known_projects.size();
        selection.progress.generation = impl_->generation;
        selection.progress.complete = impl_->complete_locked();
        selection.progress.paused = impl_->paused;
    }

    std::vector<SessionInfo> active_sessions;
    if (impl_->active_sessions_provider) {
        try {
            active_sessions = impl_->active_sessions_provider();
        } catch (...) {
            selection.errors.push_back(make_error(
                impl_->projects_dir, {}, "active-sessions",
                "failed to read active session snapshot"));
        }
    }

    std::unordered_set<std::string> archived_identities;
    std::unordered_map<std::string, const GlobalSessionCatalogEntry*> best_by_identity;
    for (const auto& shard : shard_views) {
        for (const auto& error : shard->errors) {
            if (selection.errors.size() >= kMaximumSelectionErrors) break;
            selection.errors.push_back(error);
        }
        for (const auto& identity : shard->archived_identities) {
            archived_identities.insert(identity);
        }
        for (const auto& entry : shard->entries) {
            const std::string identity = global_session_catalog_identity(
                entry.meta.no_workspace, entry.workspace_hash, entry.meta.id);
            auto found = best_by_identity.find(identity);
            if (found == best_by_identity.end() ||
                prefer_candidate(entry, *found->second)) {
                best_by_identity[identity] = &entry;
            }
        }
    }

    std::unordered_map<std::string, const SessionInfo*> active_by_identity;
    for (const auto& active : active_sessions) {
        if (active.id.empty() || !active.parent_session_id.empty()) continue;
        const std::string workspace_hash = active.no_workspace
            ? std::string{}
            : (!active.workspace_hash.empty()
                ? active.workspace_hash
                : compute_cwd_hash(active.cwd));
        const std::string identity = global_session_catalog_identity(
            active.no_workspace, workspace_hash, active.id);
        if (!archived_identities.count(identity)) {
            active_by_identity[identity] = &active;
        }
    }

    struct RankedEntry {
        int score = 0;
        const GlobalSessionCatalogEntry* entry = nullptr;
    };
    std::deque<GlobalSessionCatalogEntry> overlays;
    std::vector<RankedEntry> ranked;
    ranked.reserve(best_by_identity.size() + active_by_identity.size());

    for (const auto& [identity, disk_entry] : best_by_identity) {
        if (archived_identities.count(identity)) continue;
        const GlobalSessionCatalogEntry* candidate = disk_entry;
        if (const auto active = active_by_identity.find(identity);
            active != active_by_identity.end()) {
            overlays.push_back(*disk_entry);
            overlays.back().active = *active->second;
            candidate = &overlays.back();
            active_by_identity.erase(active);
        }
        const int value = score ? score(*candidate) : 1;
        if (value > 0) ranked.push_back({value, candidate});
    }

    const fs::path root = path_from_utf8(impl_->projects_dir);
    for (const auto& [identity, active] : active_by_identity) {
        if (!active || archived_identities.count(identity)) continue;
        overlays.emplace_back();
        auto& entry = overlays.back();
        entry.workspace_hash = active->no_workspace
            ? std::string{}
            : (!active->workspace_hash.empty()
                ? active->workspace_hash
                : compute_cwd_hash(active->cwd));
        entry.meta = synthesize_meta(*active);
        entry.active = *active;
        if (!active->no_workspace) {
            entry.project_dir = path_to_utf8(root / entry.workspace_hash);
            const auto workspace = desktop::load_workspace_metadata(
                impl_->projects_dir, entry.workspace_hash);
            entry.workspace_cwd = workspace && !workspace->cwd.empty()
                ? workspace->cwd
                : active->cwd;
            entry.workspace_name = workspace && !workspace->name.empty()
                ? workspace->name
                : desktop::default_workspace_name(entry.workspace_cwd);
            entry.workspace_visible = workspace && workspace->desktop_visible;
        } else if (!active->cwd.empty()) {
            entry.project_dir = path_to_utf8(root / compute_cwd_hash(active->cwd));
        }
        const int value = score ? score(entry) : 1;
        if (value > 0) ranked.push_back({value, &entry});
    }

    const auto ranked_before = [](const auto& a, const auto& b) {
        if (a.score != b.score) return a.score > b.score;
        const std::string a_updated = effective_updated_at(*a.entry).empty()
            ? a.entry->meta.created_at
            : effective_updated_at(*a.entry);
        const std::string b_updated = effective_updated_at(*b.entry).empty()
            ? b.entry->meta.created_at
            : effective_updated_at(*b.entry);
        if (a_updated != b_updated) return a_updated > b_updated;
        if (a.entry->workspace_hash != b.entry->workspace_hash) {
            return a.entry->workspace_hash < b.entry->workspace_hash;
        }
        return a.entry->meta.id < b.entry->meta.id;
    };
    selection.truncated = ranked.size() > max_entries;
    const std::size_t count = (std::min)(ranked.size(), max_entries);
    if (count < ranked.size()) {
        std::partial_sort(
            ranked.begin(), ranked.begin() + count, ranked.end(), ranked_before);
    } else {
        std::sort(ranked.begin(), ranked.end(), ranked_before);
    }
    selection.entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        selection.entries.push_back(*ranked[i].entry);
    }
    std::sort(selection.errors.begin(), selection.errors.end(), [](
        const GlobalSessionCatalogError& a,
        const GlobalSessionCatalogError& b) {
        if (a.project_dir != b.project_dir) return a.project_dir < b.project_dir;
        if (a.stage != b.stage) return a.stage < b.stage;
        return a.message < b.message;
    });
    return selection;
}

GlobalSessionCatalogProjectSnapshot
GlobalSessionCatalogIndex::project_snapshot(
    std::optional<std::uint64_t> known_generation) const {
    GlobalSessionCatalogProjectSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        snapshot.progress.scanned_projects = impl_->scanned_projects.size();
        snapshot.progress.total_projects = impl_->known_projects.size();
        snapshot.progress.generation = impl_->generation;
        snapshot.progress.complete = impl_->complete_locked();
        snapshot.progress.paused = impl_->paused;
        snapshot.project_generation = impl_->project_generation;
        snapshot.paths_changed = !known_generation ||
            *known_generation != impl_->project_generation;
        if (snapshot.paths_changed) {
            snapshot.project_dirs.assign(
                impl_->known_projects.begin(), impl_->known_projects.end());
        }
    }
    if (snapshot.paths_changed) {
        std::sort(snapshot.project_dirs.begin(), snapshot.project_dirs.end());
    }
    return snapshot;
}

std::optional<GlobalSessionCatalogEntry>
GlobalSessionCatalogIndex::find_project_session(
    const std::string& project_dir,
    const std::string& session_id) const {
    std::shared_ptr<const ProjectScanResult> shard;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        const auto found = impl_->shards.find(project_dir);
        if (found == impl_->shards.end()) return std::nullopt;
        shard = found->second;
    }
    if (!shard) return std::nullopt;
    for (const auto& entry : shard->entries) {
        if (entry.meta.id == session_id) return entry;
    }
    return std::nullopt;
}

} // namespace acecode
