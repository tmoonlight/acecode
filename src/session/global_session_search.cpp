#include "global_session_search.hpp"

#include "session_user_message_search.hpp"
#include "../desktop/workspace_registry.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace acecode {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kDefaultLimit = 50;
constexpr std::size_t kMaximumLimit = 100;
constexpr std::size_t kMaximumEmptyQueryLimit = 50;
constexpr std::size_t kProjectsPerContentBatch = 32;
constexpr auto kContentBatchBudget = std::chrono::milliseconds(40);
constexpr auto kContentJobTtl = std::chrono::minutes(2);

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim_copy(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return value.substr(first, last - first);
}

std::string entry_updated_at(const GlobalSessionCatalogEntry& entry) {
    if (entry.active && !entry.active->updated_at.empty()) {
        return entry.active->updated_at;
    }
    return entry.meta.updated_at.empty()
        ? entry.meta.created_at
        : entry.meta.updated_at;
}

std::string entry_title(const GlobalSessionCatalogEntry& entry) {
    if (entry.active && !entry.active->title.empty()) return entry.active->title;
    if (!entry.meta.title.empty()) return entry.meta.title;
    if (!entry.meta.summary.empty()) return entry.meta.summary;
    return entry.meta.id;
}

bool fuzzy_hit(const std::string& target, const std::string& query) {
    if (query.empty()) return true;
    std::size_t query_index = 0;
    for (unsigned char value : target) {
        if (value != static_cast<unsigned char>(query[query_index])) continue;
        ++query_index;
        if (query_index == query.size()) return true;
    }
    return false;
}

int metadata_score(const GlobalSessionCatalogEntry& entry,
                   const std::string& query_norm) {
    if (query_norm.empty()) return 1;
    const auto title = ascii_lower(entry_title(entry));
    const auto summary = ascii_lower(entry.meta.summary);
    const auto workspace = ascii_lower(
        entry.workspace_name + "\n" + entry.workspace_cwd);
    const auto id = ascii_lower(entry.meta.id);
    if (title.rfind(query_norm, 0) == 0) return 1000;
    if (title.find(query_norm) != std::string::npos) return 850;
    if (summary.find(query_norm) != std::string::npos) return 650;
    if (workspace.find(query_norm) != std::string::npos) return 450;
    if (id.find(query_norm) != std::string::npos) return 350;
    if (fuzzy_hit(title, query_norm)) return 200;
    return 0;
}

bool parse_cursor(const std::string& cursor,
                  std::uint64_t& generation,
                  std::size_t& offset) {
    const auto separator = cursor.find(':');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= cursor.size()) {
        return false;
    }
    try {
        std::size_t generation_end = 0;
        std::size_t offset_end = 0;
        generation = std::stoull(cursor.substr(0, separator), &generation_end);
        offset = static_cast<std::size_t>(
            std::stoull(cursor.substr(separator + 1), &offset_end));
        return generation_end == separator &&
            offset_end == cursor.size() - separator - 1;
    } catch (...) {
        return false;
    }
}

std::string make_cursor(std::uint64_t generation, std::size_t offset) {
    return std::to_string(generation) + ':' + std::to_string(offset);
}

std::int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

GlobalSessionCatalogEntry fallback_catalog_entry(
    const std::string& project_dir,
    const SessionMeta& meta) {
    GlobalSessionCatalogEntry entry;
    entry.project_dir = project_dir;
    entry.meta = meta;
    entry.workspace_hash = path_to_utf8(
        path_from_utf8(project_dir).filename());
    if (meta.no_workspace) {
        entry.workspace_name = "\u65e0\u5de5\u4f5c\u533a";
        return entry;
    }
    const auto projects_dir = path_to_utf8(
        path_from_utf8(project_dir).parent_path());
    if (auto workspace = desktop::load_workspace_metadata(
            projects_dir, entry.workspace_hash)) {
        entry.workspace_name = workspace->name.empty()
            ? desktop::default_workspace_name(workspace->cwd)
            : workspace->name;
        entry.workspace_cwd = workspace->cwd;
        entry.workspace_visible = workspace->desktop_visible;
    } else {
        entry.workspace_name = desktop::default_workspace_name(meta.cwd);
        entry.workspace_cwd = meta.cwd;
    }
    return entry;
}

} // namespace

struct GlobalSessionSearchService::Impl {
    struct RawContentMatch {
        GlobalSessionCatalogEntry entry;
    };

    struct ContentJob {
        std::string request_id;
        std::string query;
        std::size_t limit = kDefaultLimit;
        std::vector<std::string> project_dirs;
        std::unordered_set<std::string> project_set;
        std::uint64_t project_generation = 0;
        std::size_t next_project = 0;
        std::vector<RawContentMatch> matches;
        std::vector<GlobalSessionCatalogError> errors;
        std::atomic<bool> cancelled{false};
        std::atomic<std::int64_t> last_used_ms{0};
        std::mutex run_mu;
    };

    GlobalSessionCatalogIndex index;
    std::mutex jobs_mu;
    std::unordered_map<std::string, std::shared_ptr<ContentJob>> jobs;

    explicit Impl(std::string projects_dir,
                  ActiveSessionsProvider active_sessions_provider)
        : index(std::move(projects_dir),
                std::move(active_sessions_provider)) {}

    static std::string job_key(const std::string& request_id,
                               const std::string& query) {
        return request_id + '\x1f' + query;
    }

    void cleanup_jobs_locked(std::int64_t now_ms) {
        const auto ttl_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            kContentJobTtl).count();
        for (auto it = jobs.begin(); it != jobs.end();) {
            if (now_ms - it->second->last_used_ms.load() > ttl_ms) {
                it->second->cancelled.store(true);
                it = jobs.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::shared_ptr<ContentJob> get_or_create_job(
        const std::string& request_id,
        const std::string& query,
        std::size_t limit) {
        const auto now_ms = steady_now_ms();
        std::lock_guard<std::mutex> lock(jobs_mu);
        cleanup_jobs_locked(now_ms);
        const auto key = job_key(request_id, query);
        auto found = jobs.find(key);
        if (found != jobs.end()) {
            found->second->last_used_ms.store(now_ms);
            return found->second;
        }
        auto snapshot = index.project_snapshot();
        auto job = std::make_shared<ContentJob>();
        job->request_id = request_id;
        job->query = query;
        job->limit = limit;
        job->project_dirs = std::move(snapshot.project_dirs);
        job->project_generation = snapshot.project_generation;
        job->project_set.insert(
            job->project_dirs.begin(), job->project_dirs.end());
        job->last_used_ms.store(now_ms);
        jobs.emplace(key, job);
        return job;
    }
};

GlobalSessionSearchService::GlobalSessionSearchService(
    std::string projects_dir,
    ActiveSessionsProvider active_sessions_provider)
    : impl_(std::make_unique<Impl>(
          std::move(projects_dir), std::move(active_sessions_provider))) {}

GlobalSessionSearchService::~GlobalSessionSearchService() {
    stop();
}

void GlobalSessionSearchService::start() {
    impl_->index.start();
}

void GlobalSessionSearchService::stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->jobs_mu);
        for (auto& [_, job] : impl_->jobs) job->cancelled.store(true);
        impl_->jobs.clear();
    }
    impl_->index.stop();
}

GlobalSessionSearchPage GlobalSessionSearchService::search_sessions(
    const std::string& request_id,
    const std::string& query,
    std::size_t limit,
    const std::string& cursor) {
    GlobalSessionSearchPage page;
    if (!impl_->index.attach_request(request_id)) {
        page.progress.paused = true;
        return page;
    }
    const std::string query_norm = ascii_lower(trim_copy(query));
    limit = limit == 0 ? kDefaultLimit : (std::min)(limit, kMaximumLimit);
    if (query_norm.empty()) {
        limit = (std::min)(limit, kMaximumEmptyQueryLimit);
    }

    std::size_t offset = 0;
    std::uint64_t cursor_generation = 0;
    if (!cursor.empty()) {
        if (!parse_cursor(cursor, cursor_generation, offset)) {
            page.cursor_stale = true;
            return page;
        }
    }
    if (offset > (std::numeric_limits<std::size_t>::max)() - limit - 1) {
        page.cursor_stale = true;
        return page;
    }

    auto selection = impl_->index.select_entries(
        offset + limit + 1,
        [&query_norm](const GlobalSessionCatalogEntry& entry) {
            return metadata_score(entry, query_norm);
        });
    page.progress = selection.progress;
    page.errors = std::move(selection.errors);
    if (!cursor.empty() && cursor_generation != page.progress.generation) {
        page.cursor_stale = true;
        return page;
    }
    if (offset > selection.entries.size()) {
        page.cursor_stale = true;
        return page;
    }

    const auto end = (std::min)(selection.entries.size(), offset + limit);
    page.entries.reserve(end - offset);
    for (std::size_t i = offset; i < end; ++i) {
        page.entries.push_back(std::move(selection.entries[i]));
    }
    if (page.progress.complete &&
        (end < selection.entries.size() || selection.truncated)) {
        page.next_cursor = make_cursor(page.progress.generation, end);
    }
    return page;
}

GlobalSessionContentSearchPage
GlobalSessionSearchService::search_user_messages_batch(
    const std::string& request_id,
    const std::string& query,
    std::size_t limit) {
    GlobalSessionContentSearchPage page;
    const std::string normalized_query = trim_copy(query);
    if (normalized_query.empty()) {
        page.complete = true;
        return page;
    }
    limit = limit == 0 ? kDefaultLimit : (std::min)(limit, kMaximumLimit);
    if (!impl_->index.attach_request(request_id)) {
        page.cancelled = true;
        return page;
    }
    auto job = impl_->get_or_create_job(request_id, normalized_query, limit);
    std::lock_guard<std::mutex> run_lock(job->run_mu);
    job->last_used_ms.store(steady_now_ms());
    auto project_snapshot = impl_->index.project_snapshot(job->project_generation);
    job->project_generation = project_snapshot.project_generation;
    for (const auto& project_dir : project_snapshot.project_dirs) {
        if (job->project_set.insert(project_dir).second) {
            job->project_dirs.push_back(project_dir);
        }
    }
    const auto should_cancel = [this, job] {
        return job->cancelled.load() ||
            impl_->index.is_request_cancelled(job->request_id);
    };
    if (should_cancel()) {
        page.cancelled = true;
        return page;
    }

    const auto deadline = std::chrono::steady_clock::now() + kContentBatchBudget;
    std::size_t processed = 0;
    while (job->next_project < job->project_dirs.size() &&
           processed < kProjectsPerContentBatch &&
           std::chrono::steady_clock::now() < deadline &&
           !should_cancel()) {
        const std::string project_dir = job->project_dirs[job->next_project];
        SessionUserMessageIndex search_index(project_dir);
        std::string error;
        const bool indexed = search_index.ensure_project_indexed(
            &error, should_cancel);
        if (should_cancel() || error == "cancelled") break;
        if (!indexed) {
            job->errors.push_back(GlobalSessionCatalogError{
                project_dir, {}, "content-index", error});
        } else {
            auto matches = search_index.search(
                normalized_query, static_cast<int>(limit), &error, should_cancel);
            if (should_cancel() || error == "cancelled") break;
            if (!error.empty()) {
                job->errors.push_back(GlobalSessionCatalogError{
                    project_dir, {}, "content-search", error});
            }
            std::unordered_map<std::string, SessionMeta> metadata;
            for (auto& meta : SessionStorage::list_session_metadata(
                     project_dir, should_cancel)) {
                if (meta.id.empty() || meta.archived ||
                    !meta.parent_session_id.empty()) {
                    continue;
                }
                metadata.emplace(meta.id, std::move(meta));
            }
            for (auto& match : matches) {
                const auto meta = metadata.find(match.session_id);
                if (meta == metadata.end()) continue;
                auto known = impl_->index.find_project_session(
                    project_dir, match.session_id);
                auto entry = known
                    ? std::move(*known)
                    : fallback_catalog_entry(project_dir, meta->second);
                entry.content_match = std::move(match);
                job->matches.push_back(Impl::RawContentMatch{std::move(entry)});
            }
        }
        ++job->next_project;
        ++processed;
    }

    if (should_cancel()) {
        page.cancelled = true;
        return page;
    }

    std::unordered_map<std::string, GlobalSessionCatalogEntry> best_by_identity;
    for (const auto& raw : job->matches) {
        auto entry = raw.entry;
        const auto identity = global_session_catalog_identity(
            entry.meta.no_workspace, entry.workspace_hash, entry.meta.id);
        auto existing = best_by_identity.find(identity);
        if (existing == best_by_identity.end() ||
            !existing->second.content_match ||
            existing->second.content_match->score < entry.content_match->score) {
            best_by_identity[identity] = std::move(entry);
        }
    }
    page.entries.reserve(best_by_identity.size());
    for (auto& [_, entry] : best_by_identity) {
        page.entries.push_back(std::move(entry));
    }
    std::sort(page.entries.begin(), page.entries.end(), [](const auto& a, const auto& b) {
        const int a_score = a.content_match ? a.content_match->score : 0;
        const int b_score = b.content_match ? b.content_match->score : 0;
        if (a_score != b_score) return a_score > b_score;
        return entry_updated_at(a) > entry_updated_at(b);
    });
    if (page.entries.size() > limit) page.entries.resize(limit);

    page.errors = job->errors;
    page.scanned_projects = job->next_project;
    page.total_projects = job->project_dirs.size();
    page.complete = project_snapshot.progress.complete &&
        job->next_project >= job->project_dirs.size();
    return page;
}

bool GlobalSessionSearchService::cancel(const std::string& request_id) {
    if (request_id.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(impl_->jobs_mu);
        for (auto& [_, job] : impl_->jobs) {
            if (job->request_id == request_id) job->cancelled.store(true);
        }
    }
    return impl_->index.cancel_request(request_id);
}

void GlobalSessionSearchService::invalidate_project(
    const std::string& workspace_hash) {
    impl_->index.invalidate_project(workspace_hash);
}

void GlobalSessionSearchService::request_discovery() {
    impl_->index.request_discovery();
}

} // namespace acecode
