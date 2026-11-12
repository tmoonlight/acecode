#include "session_outputs_index.hpp"

#include "jsonl_scanner.hpp"
#include "session_file_reader.hpp"
#include "session_serializer.hpp"
#include "session_storage.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <utility>

#include <sys/stat.h>
#include <sys/types.h>

namespace acecode {

namespace {

// 一个记录文件最多留这么多条候选(留最新的);一次查询最多跟这么多个会话、
// 到磁盘上核对这么多个不同路径。都是防极端会话的上限,正常用不到。
constexpr std::size_t kMaxCandidatesPerSession = 4000;
constexpr std::size_t kMaxSessionsPerQuery = 64;
constexpr std::size_t kMaxResolvedPaths = 400;
constexpr std::size_t kMaxCachedScans = 128;
constexpr std::size_t kPrefixBytes = 4096;

struct FileFacts {
    bool regular = false;
    std::uint64_t size = 0;
    std::int64_t modified_s = 0;
};

FileFacts stat_file(const std::string& utf8_path) {
    FileFacts facts;
#ifdef _WIN32
    struct _stat64 info{};
    if (::_wstat64(path_from_utf8(utf8_path).c_str(), &info) != 0) return facts;
    facts.regular = (info.st_mode & _S_IFREG) != 0;
#else
    struct stat info{};
    if (::stat(utf8_path.c_str(), &info) != 0) return facts;
    facts.regular = S_ISREG(info.st_mode);
#endif
    facts.size = info.st_size > 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
    facts.modified_s = static_cast<std::int64_t>(info.st_mtime);
    return facts;
}

// 比较两侧必须是同一种形态:用户目录是 junction 时,记录里的盘符和工作目录的
// 盘符可能不同,所以会话文件夹和候选路径都过一遍 weakly_canonical。
std::string canonical_path(const std::string& utf8_path) {
    if (utf8_path.empty()) return {};
    std::error_code ec;
    const std::filesystem::path raw = path_from_utf8(utf8_path);
    std::filesystem::path resolved = std::filesystem::weakly_canonical(raw, ec);
    if (ec || resolved.empty()) {
        ec.clear();
        resolved = std::filesystem::absolute(raw, ec);
        if (ec || resolved.empty()) return utf8_path;
        resolved = resolved.lexically_normal();
    }
    return path_to_utf8(resolved);
}

std::string absolute_candidate(const std::string& path, const std::string& cwd) {
    const std::filesystem::path raw = path_from_utf8(path);
    if (raw.is_absolute()) return path;
    if (cwd.empty()) return {};
    return path_to_utf8(path_from_utf8(cwd) / raw);
}

std::vector<std::string> system_temp_roots() {
    std::vector<std::string> roots;
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec);
    if (!ec && !temp.empty()) roots.push_back(canonical_path(path_to_utf8(temp)));
#ifndef _WIN32
    for (const char* fixed : {"/tmp", "/var/tmp"}) roots.push_back(canonical_path(fixed));
#endif
    return roots;
}

// 会话 id 进文件名之前挡掉带路径成分的值(根 id 来自请求)。
bool is_plain_session_id(const std::string& id) {
    return !id.empty() && id.find_first_of("/\\:") == std::string::npos &&
           id.find("..") == std::string::npos;
}

} // namespace

void SessionOutputsIndex::advance(Scan& scan, const std::string& jsonl_path) {
    SessionFileReader reader(jsonl_path);
    if (!reader.valid()) {
        scan = Scan{};
        return;
    }
    bool resume = !scan.identity.empty() && scan.identity == reader.identity() &&
                  reader.size() >= scan.offset;
    if (resume && !scan.prefix.empty()) {
        resume = reader.read(0, scan.prefix.size()) == scan.prefix;
    }
    if (!resume) {
        scan = Scan{};
        scan.identity = reader.identity();
    }
    if (scan.prefix.size() < kPrefixBytes) scan.prefix = reader.prefix();

    JsonlScanner scanner(reader, false, scan.offset);
    while (auto record = scanner.next()) {
        // 写入方还没写完这一行:留到下次。
        if (!record->terminated) break;
        scan.offset = record->end;
        if (!jsonl_line_may_carry_outputs(record->text)) continue;
        const auto parsed = nlohmann::json::parse(record->text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) continue;
        const ChatMessage message = deserialize_message_json(parsed);
        for (auto& candidate : session_output_candidates(message)) {
            scan.candidates.push_back(std::move(candidate));
        }
        std::string child = session_output_child_session_id(message);
        if (!child.empty() &&
            std::find(scan.children.begin(), scan.children.end(), child) == scan.children.end()) {
            scan.children.push_back(std::move(child));
        }
    }
    if (scan.candidates.size() > kMaxCandidatesPerSession) {
        scan.candidates.erase(scan.candidates.begin(),
                              scan.candidates.end() - static_cast<std::ptrdiff_t>(kMaxCandidatesPerSession));
    }
}

SessionOutputsIndex::Scan SessionOutputsIndex::scan_session(const std::string& jsonl_path) {
    Scan scan;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = scans_.find(jsonl_path);
        if (it != scans_.end()) scan = it->second;
    }
    // 读盘在锁外:同一会话的并发查询各读各的,结果一致,后写回的覆盖先写回的。
    try {
        advance(scan, jsonl_path);
    } catch (const std::exception& error) {
        LOG_WARN(std::string("[outputs] scan failed, will rescan: ") + error.what());
        scan = Scan{};
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (scan.identity.empty()) {
        scans_.erase(jsonl_path);
        return scan;
    }
    scan.used = ++tick_;
    scans_[jsonl_path] = scan;
    if (scans_.size() > kMaxCachedScans) {
        std::vector<std::pair<std::uint64_t, std::string>> by_age;
        by_age.reserve(scans_.size());
        for (const auto& item : scans_) by_age.emplace_back(item.second.used, item.first);
        std::sort(by_age.begin(), by_age.end());
        for (std::size_t i = 0; i + kMaxCachedScans / 2 < by_age.size(); ++i) {
            scans_.erase(by_age[i].second);
        }
    }
    return scan;
}

SessionOutputsResult SessionOutputsIndex::query(const SessionOutputsQuery& query) {
    SessionOutputsResult result;
    if (!is_plain_session_id(query.session_id) || query.project_dir.empty()) return result;

    // 主会话在前,再按派生顺序跟到各个子代理会话(网状模式下子代理还会再派生)。
    std::vector<SessionOutputCandidate> raw;
    std::vector<std::string> sessions{query.session_id};
    std::unordered_set<std::string> visited;
    for (std::size_t i = 0; i < sessions.size() && visited.size() < kMaxSessionsPerQuery; ++i) {
        const std::string id = sessions[i];
        if (!is_plain_session_id(id) || !visited.insert(id).second) continue;
        Scan scan = scan_session(SessionStorage::session_path(query.project_dir, id));
        raw.insert(raw.end(), scan.candidates.begin(), scan.candidates.end());
        sessions.insert(sessions.end(), scan.children.begin(), scan.children.end());
    }
    if (raw.empty()) return result;

    // 比较用归一后的形态,交给界面的路径用调用方给的原始形态拼回去:界面的预览、
    // 文件树都按它自己知道的工作目录写法定位文件,盘符换了就对不上。
    std::vector<std::string> folders = query.folders;
    if (!query.cwd.empty()) folders.push_back(query.cwd);
    SessionOutputRoots roots;
    for (const auto& folder : folders) {
        roots.folders.push_back(folder.empty() ? std::string{} : canonical_path(folder));
    }
    roots.temp_roots = system_temp_roots();

    // 从最新的往回取,只对前 kMaxResolvedPaths 个不同路径做归一(要碰磁盘)。
    std::vector<SessionOutputCandidate> resolved;
    std::unordered_set<std::string> seen;
    for (auto it = raw.rbegin(); it != raw.rend() && resolved.size() < kMaxResolvedPaths; ++it) {
        if (!seen.insert(it->path).second) continue;
        const std::string absolute = absolute_candidate(it->path, query.cwd);
        if (absolute.empty()) continue;
        resolved.push_back({canonical_path(absolute), it->source});
    }
    std::reverse(resolved.begin(), resolved.end());

    for (auto& entry : select_session_outputs(resolved, roots)) {
        const FileFacts facts = stat_file(entry.path);
        if (!facts.regular) continue;   // 已经删掉 / 挪走的不显示
        SessionOutputFile file;
        if (entry.folder_index >= 0 && !entry.relative.empty()) {
            std::filesystem::path shown = path_from_utf8(folders[static_cast<std::size_t>(entry.folder_index)]) /
                                          path_from_utf8(entry.relative);
            file.path = path_to_utf8(shown.make_preferred());
        } else {
            file.path = std::move(entry.path);
        }
        file.relative = std::move(entry.relative);
        file.source = entry.source;
        file.size = facts.size;
        file.modified_ms = facts.modified_s * 1000;
        result.files.push_back(std::move(file));
    }
    // 最近修改的在前;同一秒内保持「记录里更靠后的在前」。
    std::stable_sort(result.files.begin(), result.files.end(),
        [](const SessionOutputFile& a, const SessionOutputFile& b) {
            return a.modified_ms > b.modified_ms;
        });
    const std::size_t limit = query.limit == 0 ? 50 : query.limit;
    if (result.files.size() > limit) {
        result.has_more = true;
        result.files.resize(limit);
    }
    return result;
}

} // namespace acecode
