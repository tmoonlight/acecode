#include "slash_command_usage.hpp"

#include "utils/state_file.hpp"

#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <utility>

namespace acecode {
namespace {

constexpr const char* kTuiSlashCommandUsageKey = "tui_slash_command_usage";
bool valid_slash_command_name(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return false;
    }
    return true;
}

std::optional<std::uint64_t> parse_positive_count(
    const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        const auto count = value.get<std::uint64_t>();
        if (count > 0) return count;
        return std::nullopt;
    }
    if (value.is_number_integer()) {
        const auto count = value.get<std::int64_t>();
        if (count > 0) return static_cast<std::uint64_t>(count);
    }
    return std::nullopt;
}

std::map<std::string, std::uint64_t> parse_tui_slash_command_usage(
    const nlohmann::json& state) {
    std::map<std::string, std::uint64_t> counts;
    if (!state.contains(kTuiSlashCommandUsageKey) ||
        !state[kTuiSlashCommandUsageKey].is_object()) {
        return counts;
    }

    for (auto it = state[kTuiSlashCommandUsageKey].begin();
         it != state[kTuiSlashCommandUsageKey].end(); ++it) {
        if (!valid_slash_command_name(it.key())) continue;
        auto count = parse_positive_count(it.value());
        if (count) counts.emplace(it.key(), *count);
    }
    return counts;
}

} // namespace

std::map<std::string, std::uint64_t> read_tui_slash_command_usage() {
    return parse_tui_slash_command_usage(read_state_json());
}

SlashCommandUsageWriteResult record_tui_slash_command_use(
    const std::string& command_name) {
    if (!valid_slash_command_name(command_name)) return {};

    SlashCommandUsageWriteResult result;
    // 同步事务:拿不到锁时不计算次数;写盘失败仍返回本次算出的内存次数。
    result.persisted = update_state_json([&result, command_name](nlohmann::json& state) {
        auto counts = parse_tui_slash_command_usage(state);
        auto& count = counts[command_name];
        if (count < (std::numeric_limits<std::uint64_t>::max)()) ++count;
        result.count = count;

        nlohmann::json usage = nlohmann::json::object();
        for (const auto& [name, value] : counts) usage[name] = value;
        state[kTuiSlashCommandUsageKey] = std::move(usage);
        return true;
    });
    return result;
}

} // namespace acecode
