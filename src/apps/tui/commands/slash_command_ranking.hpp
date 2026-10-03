#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace acecode {

struct SlashCommandCandidate {
    std::string name;
    std::string description;
    std::vector<std::string> aliases;
    // 仅输出:查询命中某个别名、且别名的匹配档位高于原名时填该别名,
    // 下拉菜单据此显示成 "/原名 (别名)";否则为空。
    std::string matched_alias;
};

using SlashCommandUsageCounts = std::map<std::string, std::uint64_t>;

// Filter and rank slash-command candidates for the TUI picker. Existing
// match relevance remains the primary key; usage replaces alphabetical order
// only among equally relevant matches.
//
// 别名按原名同样的档位(完全 / 前缀 / 子串)参与匹配,一条命令取最好的档位;
// 使用次数取原名与全部别名之和(旧版本按别名单独计数)。
std::vector<SlashCommandCandidate> rank_slash_command_candidates(
    std::string_view query,
    const std::vector<SlashCommandCandidate>& candidates,
    const SlashCommandUsageCounts& usage_counts);

} // namespace acecode
