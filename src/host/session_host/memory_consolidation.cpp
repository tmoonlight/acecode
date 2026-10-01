#include "memory_consolidation.hpp"

#include "memory/memory_paths.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

using EntrySnapshot = std::map<std::string, std::optional<std::string>>;

std::optional<std::string> read_if_exists(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return std::nullopt;
    std::ifstream ifs(path, std::ios::binary);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

// 把计划触及的条目文件恢复到应用前的字节(原本不存在的删掉),再按磁盘重建索引。
void restore_entries(MemoryRegistry& registry, const EntrySnapshot& snapshot) {
    for (const auto& [name, content] : snapshot) {
        const fs::path path = registry.dir() / (name + ".md");
        std::error_code ec;
        if (!content) {
            fs::remove(path, ec);
            continue;
        }
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        ofs.write(content->data(), static_cast<std::streamsize>(content->size()));
        if (!ofs) LOG_ERROR("[memory] rollback failed to restore " + path_to_utf8(path));
    }
    registry.rebuild_index();
}

void add_unique(std::vector<std::string>& into, const std::vector<std::string>& values) {
    for (const auto& value : values) {
        if (value.empty()) continue;
        if (std::find(into.begin(), into.end(), value) == into.end()) into.push_back(value);
    }
}

MemoryWriteMode write_mode(const std::string& op, bool replay) {
    if (replay) return MemoryWriteMode::Upsert;
    if (op == "create") return MemoryWriteMode::Create;
    if (op == "update") return MemoryWriteMode::Update;
    return MemoryWriteMode::Upsert;
}

} // namespace

MemoryConsolidationApply apply_memory_plan(MemoryService& memory,
                                           MemoryRegistry& registry,
                                           const std::vector<MemoryPlanOperation>& operations,
                                           const std::vector<MemoryObservationFile>& batch,
                                           const std::string& plan_hash,
                                           const std::string& archive_date,
                                           std::int64_t now_ms,
                                           bool replay) {
    MemoryConsolidationApply result;
    MemoryWriteLock lock(&memory.state());
    registry.reload();

    std::map<std::string, std::string> session_of;
    for (const auto& file : batch) {
        for (const auto& obs : file.observations) session_of[obs.id] = file.session_id;
    }
    EntrySnapshot snapshot;
    for (const auto& op : operations) {
        std::vector<std::string> names = op.sources;
        names.push_back(op.name);
        for (const auto& name : names) {
            if (!snapshot.count(name)) snapshot[name] = read_if_exists(registry.dir() / (name + ".md"));
        }
    }

    for (const auto& op : operations) {
        std::string error;
        bool ok = true;
        const auto existing = registry.find(op.name);
        if (op.op == "delete") {
            if (existing) ok = registry.remove(op.name, error);
            else if (!replay) { ok = false; error = "entry does not exist"; }
        } else {
            std::vector<std::string> sessions;
            if (existing) add_unique(sessions, existing->source_sessions);
            for (const auto& source : op.sources) {
                if (const auto merged = registry.find(source)) add_unique(sessions, merged->source_sessions);
            }
            for (const auto& id : op.evidence) {
                const auto it = session_of.find(id);
                if (it != session_of.end()) add_unique(sessions, {it->second});
            }
            MemoryWriteRequest request;
            request.name = op.name;
            request.type = op.type.value_or(existing ? existing->type : MemoryType::Project);
            request.description = op.description;
            request.body = op.body;
            request.mode = write_mode(op.op, replay);
            request.source = kMemorySourceSummary;
            request.source_sessions = sessions;
            request.replace_source_sessions = true;
            ok = registry.upsert(request, error).has_value();
            if (ok && op.op == "merge") {
                for (const auto& source : op.sources) {
                    if (source == op.name || !registry.find(source)) continue;
                    if (!registry.remove(source, error)) {
                        ok = false;
                        break;
                    }
                }
            }
        }
        if (!ok) {
            restore_entries(registry, snapshot);
            result.error = op.op + " " + op.name + ": " + error;
            return result;
        }
        ++result.changed_entries;
    }

    // 本批观察无论是否被引用都整体归档;失败时把已移走的文件挪回收件箱。
    const fs::path archive_dir = memory_archive_dir(registry.dir()) / archive_date;
    std::error_code ec;
    fs::create_directories(archive_dir, ec);
    std::vector<std::pair<fs::path, fs::path>> moved;
    for (const auto& file : batch) {
        std::error_code exists_ec;
        if (!fs::exists(file.path, exists_ec)) continue;  // 重放:上次已归档
        const fs::path target = archive_dir / file.path.filename();
        fs::rename(file.path, target, ec);
        if (ec) {
            for (const auto& [from, to] : moved) {
                std::error_code back_ec;
                fs::rename(to, from, back_ec);
            }
            restore_entries(registry, snapshot);
            result.error = "failed to archive " + path_to_utf8(file.path) + ": " + ec.message();
            return result;
        }
        moved.emplace_back(file.path, target);
    }
    memory.state().commit_consolidation(registry.scope_key(), plan_hash, now_ms);
    lock.commit();
    result.ok = true;
    return result;
}

} // namespace acecode
