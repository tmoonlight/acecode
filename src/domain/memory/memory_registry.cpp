#include "memory_registry.hpp"

#include "memory_frontmatter.hpp"
#include "memory_index.hpp"
#include "memory_paths.hpp"
#include "memory_state_store.hpp"
#include "secret_redaction.hpp"

#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

std::string read_file_to_string(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return ensure_utf8(oss.str());
}

// Atomic write: dump text to `<target>.tmp-<rand>` then rename() over target.
// Returns empty string on success; rejection reason otherwise.
std::string atomic_write(const fs::path& target, const std::string& content) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) return "failed to create memory dir: " + ec.message();

    std::random_device rd;
    std::mt19937_64 gen(rd());
    auto suffix = std::to_string(gen());
    fs::path tmp = target;
    tmp += ".tmp-" + suffix;

    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return "failed to open temp file: " + path_to_utf8(tmp);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs) return "failed to write temp file: " + path_to_utf8(tmp);
    }

    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code cleanup_ec;
        fs::remove(tmp, cleanup_ec);
        return "failed to rename temp to target: " + ec.message();
    }
    return {};
}

bool is_index_stem(const fs::path& p) {
    std::string stem = path_to_utf8(p.stem());
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return stem == "memory";
}

void sort_entries(std::vector<MemoryEntry>& entries) {
    std::sort(entries.begin(), entries.end(),
              [](const MemoryEntry& a, const MemoryEntry& b) { return a.name < b.name; });
}

} // namespace

MemoryRegistry::MemoryRegistry() : MemoryRegistry(get_memory_dir()) {}

MemoryRegistry::MemoryRegistry(fs::path dir, std::string scope_key, MemoryStateStore* state)
    : dir_(std::move(dir)), scope_key_(std::move(scope_key)), state_(state) {}

void MemoryRegistry::scan() {
    std::lock_guard<std::mutex> lock(mu_);
    scan_locked();
}

void MemoryRegistry::scan_locked() {
    entries_.clear();

    std::error_code ec;
    if (dir_.empty() || !fs::exists(dir_, ec)) {
        // Empty directory is not an error — first launch / first write pending.
        return;
    }
    if (!fs::is_directory(dir_, ec)) {
        LOG_WARN("[memory] " + path_to_utf8(dir_) + " is not a directory; ignoring");
        return;
    }

    // 只看顶层 *.md:inbox/、archive/ 归记忆摘要,其余子目录一律忽略。
    for (auto it = fs::directory_iterator(dir_, ec);
         !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        std::error_code type_ec;
        if (!fs::is_regular_file(p, type_ec)) continue;
        if (p.extension() != ".md") continue;
        // Skip the index itself; entries live in sibling files.
        if (is_index_stem(p)) continue;

        auto parsed = parse_memory_entry_file(p);
        if (!parsed) continue; // parse_memory_entry_file logs the reason.
        entries_.push_back(std::move(*parsed));
    }

    sort_entries(entries_);
}

std::vector<MemoryEntry> MemoryRegistry::list(std::optional<MemoryType> type_filter) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!type_filter.has_value()) return entries_;
    std::vector<MemoryEntry> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) {
        if (e.type == *type_filter) out.push_back(e);
    }
    return out;
}

std::optional<MemoryEntry> MemoryRegistry::find(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& e : entries_) {
        if (e.name == name) return e;
    }
    return std::nullopt;
}

std::string MemoryRegistry::read_index_raw(std::size_t max_bytes) const {
    std::lock_guard<std::mutex> lock(mu_);
    fs::path idx = dir_ / "MEMORY.md";
    std::error_code ec;
    if (!fs::is_regular_file(idx, ec) || ec) return {};

    std::string content = read_file_to_string(idx);
    if (max_bytes > 0 && content.size() > max_bytes) {
        std::size_t dropped = content.size() - max_bytes;
        content.resize(max_bytes);
        content += "\n[... truncated " + std::to_string(dropped) + " bytes]\n";
    }
    return content;
}

std::vector<MemoryEntry>::iterator MemoryRegistry::find_locked(const std::string& name) {
    return std::find_if(entries_.begin(), entries_.end(),
                        [&](const MemoryEntry& e) { return e.name == name; });
}

void MemoryRegistry::rewrite_index_locked() {
    fs::path idx = dir_ / "MEMORY.md";
    std::string existing;
    std::error_code ec;
    if (fs::is_regular_file(idx, ec) && !ec) {
        existing = read_file_to_string(idx);
    }
    std::string rendered = render_memory_index(entries_, existing);
    if (entries_.empty() && !fs::exists(idx, ec)) return;  // 不为空作用域凭空建索引
    std::string err = atomic_write(idx, rendered);
    if (!err.empty()) {
        LOG_ERROR("[memory] failed to rewrite MEMORY.md: " + err);
    }
}

std::optional<MemoryEntry> MemoryRegistry::upsert(const std::string& name,
                                                  MemoryType type,
                                                  const std::string& description,
                                                  const std::string& body,
                                                  MemoryWriteMode mode,
                                                  std::string& error_out) {
    MemoryWriteRequest request;
    request.name = name;
    request.type = type;
    request.description = description;
    request.body = body;
    request.mode = mode;
    return upsert(request, error_out);
}

std::optional<MemoryEntry> MemoryRegistry::upsert(const MemoryWriteRequest& request,
                                                  std::string& error_out,
                                                  MemoryWriteOutcome* outcome) {
    // 锁序固定为「跨进程写锁 → 进程内 mu_」,与整合应用保持一致,避免互等。
    MemoryWriteLock write_lock(state_);
    std::lock_guard<std::mutex> lock(mu_);
    auto written = upsert_locked(request, error_out, outcome);
    if (written) write_lock.commit();
    return written;
}

std::optional<MemoryEntry> MemoryRegistry::upsert_locked(const MemoryWriteRequest& request,
                                                         std::string& error_out,
                                                         MemoryWriteOutcome* outcome) {
    error_out.clear();
    std::string name_err = validate_memory_name(request.name);
    if (!name_err.empty()) {
        error_out = name_err;
        return std::nullopt;
    }

    const SecretRedactionResult description = redact_secrets(request.description);
    const SecretRedactionResult body = redact_secrets(request.body);
    if (description.text.empty()) {
        error_out = "description must not be empty";
        return std::nullopt;
    }

    fs::path target = resolve_memory_entry_path_in(dir_, request.name);
    if (target.empty() || !is_within_directory(target, dir_)) {
        error_out = "resolved path escapes the memory directory: " + request.name;
        return std::nullopt;
    }

    // 写前按磁盘重扫:TUI 与每个工作区的 daemon 各持一份缓存,拿启动时的旧缓存
    // 重写 MEMORY.md 会把别的进程后来写的条目当成「已删除」从索引里丢掉。
    scan_locked();

    auto existing = find_locked(request.name);
    bool file_exists = existing != entries_.end();
    if (request.mode == MemoryWriteMode::Create && file_exists) {
        error_out = "memory entry already exists: " + request.name;
        return std::nullopt;
    }
    if (request.mode == MemoryWriteMode::Update && !file_exists) {
        error_out = "memory entry does not exist: " + request.name;
        return std::nullopt;
    }

    const std::string now = request.now_iso.empty() ? memory_now_iso8601() : request.now_iso;
    MemoryEntry entry;
    entry.name = request.name;
    entry.description = description.text;
    entry.type = request.type;
    entry.path = target;
    entry.body = body.text;
    entry.updated_at = now;
    entry.source = request.source.empty() ? kMemorySourceManual : request.source;
    if (file_exists) {
        // 旧条目没有 created_at 时取文件原有的修改时间(必须在覆盖之前读)。
        entry.created_at = existing->created_at.empty()
            ? memory_file_mtime_iso8601(target)
            : existing->created_at;
        entry.extra_frontmatter = existing->extra_frontmatter;
        if (!request.replace_source_sessions) entry.source_sessions = existing->source_sessions;
    }
    if (entry.created_at.empty()) entry.created_at = now;
    if (request.replace_source_sessions) entry.source_sessions.clear();
    for (const auto& session : request.source_sessions) {
        if (session.empty()) continue;
        if (std::find(entry.source_sessions.begin(), entry.source_sessions.end(), session) ==
            entry.source_sessions.end()) {
            entry.source_sessions.push_back(session);
        }
    }

    std::string rendered = render_memory_entry(entry);
    std::string write_err = atomic_write(target, rendered);
    if (!write_err.empty()) {
        error_out = write_err;
        return std::nullopt;
    }

    if (file_exists) {
        *existing = entry;
    } else {
        entries_.push_back(entry);
        sort_entries(entries_);
    }
    if (outcome) {
        outcome->created = !file_exists;
        outcome->redactions = description.replacements + body.replacements;
    }

    rewrite_index_locked();
    return entry;
}

bool MemoryRegistry::remove(const std::string& name, std::string& error_out) {
    MemoryWriteLock write_lock(state_);
    std::lock_guard<std::mutex> lock(mu_);
    const bool removed = remove_locked(name, error_out);
    if (removed) write_lock.commit();
    return removed;
}

bool MemoryRegistry::remove_locked(const std::string& name, std::string& error_out) {
    error_out.clear();
    std::string name_err = validate_memory_name(name);
    if (!name_err.empty()) {
        error_out = name_err;
        return false;
    }

    fs::path target = resolve_memory_entry_path_in(dir_, name);
    if (target.empty() || !is_within_directory(target, dir_)) {
        error_out = "resolved path escapes the memory directory: " + name;
        return false;
    }

    scan_locked();

    auto it = find_locked(name);
    if (it == entries_.end()) {
        error_out = "memory entry does not exist: " + name;
        return false;
    }

    std::error_code ec;
    fs::remove(target, ec);
    if (ec) {
        error_out = "failed to remove entry file: " + ec.message();
        return false;
    }

    entries_.erase(it);

    // Update MEMORY.md: drop the matching line and keep everything else.
    fs::path idx = dir_ / "MEMORY.md";
    if (fs::is_regular_file(idx, ec) && !ec) {
        std::string existing = read_file_to_string(idx);
        std::string updated = remove_memory_index_line(existing, name);
        if (updated != existing) {
            std::string werr = atomic_write(idx, updated);
            if (!werr.empty()) {
                LOG_ERROR("[memory] failed to update MEMORY.md after remove: " + werr);
            }
        }
    }
    return true;
}

bool MemoryRegistry::reset(std::string& error_out) {
    error_out.clear();
    MemoryWriteLock write_lock(state_);
    std::lock_guard<std::mutex> lock(mu_);
    std::error_code ec;
    if (!dir_.empty() && fs::is_directory(dir_, ec)) {
        std::vector<fs::path> victims;
        for (auto it = fs::directory_iterator(dir_, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code type_ec;
            if (fs::is_regular_file(it->path(), type_ec) && it->path().extension() == ".md") {
                victims.push_back(it->path());
            }
        }
        victims.push_back(memory_inbox_dir(dir_));
        victims.push_back(memory_archive_dir(dir_));
        for (const auto& victim : victims) {
            std::error_code remove_ec;
            fs::remove_all(victim, remove_ec);
            if (remove_ec) {
                error_out = "failed to remove " + path_to_utf8(victim) + ": " + remove_ec.message();
            }
        }
    }
    entries_.clear();
    if (error_out.empty()) write_lock.commit();
    return error_out.empty();
}

void MemoryRegistry::rebuild_index() {
    MemoryWriteLock write_lock(state_);
    std::lock_guard<std::mutex> lock(mu_);
    scan_locked();
    rewrite_index_locked();
    write_lock.commit();
}

std::size_t MemoryRegistry::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

} // namespace acecode
