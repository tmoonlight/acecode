#include "session_data_diagnostics.hpp"
#include "session_load_metrics.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <regex>

namespace acecode {
namespace {

constexpr std::array<const char*, 7> categories{
    "user", "assistant", "tool", "compact_checkpoint",
    "file_checkpoint", "turn_net_diff", "other",
};

std::size_t category_for(const std::string& line) {
    try {
        ++session_read_metrics().records;
        const auto record = nlohmann::json::parse(line);
        if (!record.is_object()) return 6;
        if (line.find("compact_checkpoint") != std::string::npos &&
            record.value("is_meta", false) && record.value("subtype", "") == "compact_checkpoint") return 3;
        if (line.find("file_checkpoint") != std::string::npos &&
            record.value("is_meta", false) && record.value("subtype", "") == "file_checkpoint") return 4;
        if (line.find("turn_net_diff") != std::string::npos &&
            record.contains("metadata") && record["metadata"].is_object() &&
            record["metadata"].contains("turn_net_diff")) return 5;
        const auto role = record.value("role", "");
        if (role == "user") return 0;
        if (role == "assistant") return 1;
        if (role == "tool") return 2;
    } catch (...) {}
    return 6;
}

} // namespace

nlohmann::json diagnose_session_data(
    const std::string& projects_dir,
    const std::vector<desktop::WorkspaceMeta>& workspaces,
    const std::function<bool()>& should_cancel) {
    SessionLoadTimer timer("data_diagnostics");
    namespace fs = std::filesystem;
    using json = nlohmann::json;
    json result{{"workspaces", json::array()}, {"cancelled", false}, {"sample_limit", 5}};
    const auto cancelled = [&] {
        if (!should_cancel || !should_cancel()) return false;
        result["cancelled"] = true;
        return true;
    };
    const std::regex canonical("[0-9]{8}-[0-9]{6}-[0-9a-fA-F]{4}\\.jsonl");
    for (const auto& workspace : workspaces) {
        if (cancelled()) break;
        // Hashes, never cwd, select directories beneath the supplied root.
        if (workspace.hash.size() != 16 ||
            workspace.hash.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) continue;
        struct File { fs::path path; std::uintmax_t size; };
        std::vector<File> files;
        std::uintmax_t total = 0;
        std::size_t errors = 0;
        std::error_code ec;
        const auto directory = path_from_utf8(projects_dir) / workspace.hash;
        for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            if (cancelled()) break;
            if (!it->is_regular_file(ec)) { ec.clear(); continue; }
            if (!std::regex_match(path_to_utf8(it->path().filename()), canonical)) continue;
            const auto size = it->file_size(ec);
            if (ec) { ++errors; ec.clear(); continue; }
            files.push_back({it->path(), size});
            total += size;
        }
        if (ec) ++errors;
        std::sort(files.begin(), files.end(), [](const File& a, const File& b) {
            return a.size != b.size ? a.size > b.size : a.path < b.path;
        });
        json report{{"workspace_hash", workspace.hash}, {"workspace_name", workspace.name},
                    {"session_count", files.size()}, {"total_bytes", total},
                    {"largest_bytes", files.empty() ? 0 : files.front().size},
                    {"samples", json::array()}};
        for (std::size_t i = 0; i < (std::min)(files.size(), std::size_t{5}); ++i) {
            if (cancelled()) break;
            std::ifstream input(files[i].path, std::ios::binary);
            if (!input) { ++errors; continue; }
            ++session_read_metrics().files;
            std::array<std::uint64_t, 7> bytes{}, counts{};
            std::string line;
            std::uint64_t sampled = 0;
            while (!cancelled() && std::getline(input, line)) {
                const auto size = line.size() + (input.eof() ? 0 : 1);
                session_read_metrics().bytes += size;
                sampled += size;
                const auto category = category_for(line);
                bytes[category] += size;
                ++counts[category];
            }
            json composition = json::object();
            for (std::size_t j = 0; j < categories.size(); ++j) {
                composition[categories[j]] = {
                    {"bytes", bytes[j]}, {"records", counts[j]},
                    {"fraction", sampled == 0 ? 0.0 : static_cast<double>(bytes[j]) / sampled},
                };
            }
            report["samples"].push_back({
                {"session_id", path_to_utf8(files[i].path.stem())},
                {"file_bytes", files[i].size}, {"sampled_bytes", sampled},
                {"composition", std::move(composition)},
            });
        }
        report["errors"] = errors;
        result["workspaces"].push_back(std::move(report));
    }
    return result;
}

} // namespace acecode
