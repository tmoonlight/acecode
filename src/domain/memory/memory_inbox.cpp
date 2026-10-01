#include "memory_inbox.hpp"

#include "memory_frontmatter.hpp"
#include "memory_paths.hpp"

#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

std::string read_text(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return ensure_utf8(oss.str());
}

bool write_atomic(const fs::path& target, const std::string& content, std::string* error) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        if (error) *error = "cannot create " + path_to_utf8(target.parent_path()) + ": " + ec.message();
        return false;
    }
    std::random_device rd;
    fs::path tmp = target;
    tmp += ".tmp-" + std::to_string(std::mt19937_64(rd())());
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs) {
            if (error) *error = "cannot write " + path_to_utf8(tmp);
            return false;
        }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (error) *error = "cannot rename observation file: " + ec.message();
        return false;
    }
    return true;
}

std::optional<MemoryObservationFile> read_observation_file(const fs::path& path) {
    try {
        const auto j = nlohmann::json::parse(read_text(path));
        MemoryObservationFile file;
        file.path = path;
        file.session_id = j.value("session_id", std::string{});
        file.from = j.value("from", static_cast<std::int64_t>(0));
        file.to = j.value("to", static_cast<std::int64_t>(0));
        file.created_at = j.value("created_at", std::string{});
        file.model = j.value("model", std::string{});
        if (j.contains("observations") && j["observations"].is_array()) {
            for (const auto& item : j["observations"]) {
                MemoryObservation obs;
                obs.id = item.value("id", std::string{});
                obs.type = parse_memory_type(item.value("type", std::string{})).value_or(MemoryType::User);
                obs.title = item.value("title", std::string{});
                obs.statement = item.value("statement", std::string{});
                file.observations.push_back(std::move(obs));
            }
        }
        return file;
    } catch (const std::exception& e) {
        LOG_WARN("[memory] unreadable observation file " + path_to_utf8(path) + ": " + e.what());
        return std::nullopt;
    }
}

std::vector<fs::path> json_files_in(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        std::error_code type_ec;
        if (fs::is_regular_file(it->path(), type_ec) && it->path().extension() == ".json") {
            out.push_back(it->path());
        }
    }
    return out;
}

} // namespace

std::string memory_observation_file_stem(const std::string& session_id,
                                         std::int64_t from, std::int64_t to) {
    return session_id + "-" + std::to_string(from) + "-" + std::to_string(to);
}

bool write_memory_observation_file(const fs::path& scope_dir, MemoryObservationFile& file,
                                   std::string* error) {
    const std::string stem = memory_observation_file_stem(file.session_id, file.from, file.to);
    file.path = memory_inbox_dir(scope_dir) / (stem + ".json");
    if (file.created_at.empty()) file.created_at = memory_now_iso8601();
    nlohmann::json observations = nlohmann::json::array();
    for (std::size_t i = 0; i < file.observations.size(); ++i) {
        auto& obs = file.observations[i];
        obs.id = stem + "#" + std::to_string(i);
        observations.push_back({
            {"id", obs.id},
            {"type", memory_type_to_string(obs.type)},
            {"title", obs.title},
            {"statement", obs.statement},
        });
    }
    const nlohmann::json j = {
        {"version", 1},
        {"session_id", file.session_id},
        {"from", file.from},
        {"to", file.to},
        {"created_at", file.created_at},
        {"model", file.model},
        {"observations", std::move(observations)},
    };
    return write_atomic(file.path, j.dump(2), error);
}

void remove_memory_observation_file(const fs::path& scope_dir, const std::string& session_id,
                                    std::int64_t from, std::int64_t to) {
    std::error_code ec;
    fs::remove(memory_inbox_dir(scope_dir) /
               (memory_observation_file_stem(session_id, from, to) + ".json"), ec);
}

std::vector<MemoryObservationFile> list_memory_inbox(const fs::path& scope_dir) {
    std::vector<MemoryObservationFile> out;
    for (const auto& path : json_files_in(memory_inbox_dir(scope_dir))) {
        if (auto file = read_observation_file(path)) out.push_back(std::move(*file));
    }
    std::sort(out.begin(), out.end(), [](const MemoryObservationFile& a, const MemoryObservationFile& b) {
        if (a.created_at != b.created_at) return a.created_at < b.created_at;
        return a.path.filename() < b.path.filename();
    });
    return out;
}

std::size_t count_memory_inbox_observations(const fs::path& scope_dir) {
    std::size_t total = 0;
    for (const auto& file : list_memory_inbox(scope_dir)) total += file.observations.size();
    return total;
}

bool archive_memory_observation_files(const fs::path& scope_dir,
                                      const std::vector<fs::path>& files,
                                      const std::string& date, std::string* error) {
    const fs::path target_dir = memory_archive_dir(scope_dir) / date;
    std::error_code ec;
    fs::create_directories(target_dir, ec);
    if (ec) {
        if (error) *error = "cannot create archive directory: " + ec.message();
        return false;
    }
    for (const auto& file : files) {
        std::error_code exists_ec;
        if (!fs::exists(file, exists_ec)) continue;  // 已归档(重放)
        fs::rename(file, target_dir / file.filename(), ec);
        if (ec) {
            if (error) *error = "cannot archive " + path_to_utf8(file) + ": " + ec.message();
            return false;
        }
    }
    return true;
}

int forget_memory_session_observations(const fs::path& scope_dir, const std::string& session_id) {
    int removed = 0;
    std::vector<fs::path> candidates = json_files_in(memory_inbox_dir(scope_dir));
    std::error_code ec;
    const fs::path archive = memory_archive_dir(scope_dir);
    if (fs::is_directory(archive, ec)) {
        for (auto it = fs::directory_iterator(archive, ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            for (const auto& path : json_files_in(it->path())) candidates.push_back(path);
        }
    }
    for (const auto& path : candidates) {
        const auto file = read_observation_file(path);
        if (!file || file->session_id != session_id) continue;
        std::error_code remove_ec;
        if (fs::remove(path, remove_ec)) removed += static_cast<int>(file->observations.size());
    }
    return removed;
}

int cleanup_memory_archive(const fs::path& scope_dir, std::int64_t now_seconds, int keep_days) {
    int removed = 0;
    std::error_code ec;
    const fs::path archive = memory_archive_dir(scope_dir);
    if (!fs::is_directory(archive, ec)) return 0;
    std::vector<fs::path> expired;
    for (auto it = fs::directory_iterator(archive, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const auto day = parse_memory_iso8601(path_to_utf8(it->path().filename()) + "T00:00:00Z");
        if (!day) continue;
        if (now_seconds - *day > static_cast<std::int64_t>(keep_days) * 86400) {
            expired.push_back(it->path());
        }
    }
    for (const auto& dir : expired) {
        std::error_code remove_ec;
        fs::remove_all(dir, remove_ec);
        if (!remove_ec) ++removed;
    }
    return removed;
}

} // namespace acecode
