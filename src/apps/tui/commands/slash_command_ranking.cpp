#include "slash_command_ranking.hpp"

#include <algorithm>
#include <utility>

namespace acecode {

namespace {

int name_match_score(std::string_view query, std::string_view name) {
    if (name == query) return 200;
    if (name.rfind(query, 0) == 0) return 100;
    if (name.find(query) != std::string_view::npos) return 50;
    return 0;
}

int match_score(std::string_view query,
                std::string_view name,
                std::string_view description) {
    if (query.empty()) return 1;
    const int score = name_match_score(query, name);
    if (score > 0) return score;
    if (description.find(query) != std::string_view::npos) return 10;
    return 0;
}

std::uint64_t usage_of(const SlashCommandUsageCounts& usage_counts,
                       const std::string& name) {
    const auto it = usage_counts.find(name);
    return it == usage_counts.end() ? 0 : it->second;
}

struct RankedCandidate {
    int match_score = 0;
    std::uint64_t usage_count = 0;
    SlashCommandCandidate candidate;
};

} // namespace

std::vector<SlashCommandCandidate> rank_slash_command_candidates(
    std::string_view query,
    const std::vector<SlashCommandCandidate>& candidates,
    const SlashCommandUsageCounts& usage_counts) {
    std::vector<RankedCandidate> ranked;
    ranked.reserve(candidates.size());

    for (const auto& candidate : candidates) {
        int score = match_score(
            query, candidate.name, candidate.description);
        std::string matched_alias;
        if (!query.empty()) {
            // 只有别名比原名(含描述)匹配得更好时才标出别名:敲 "/new" 显示
            // "/clear (new)",敲 "/cl" 仍只显示 "/clear"。
            for (const auto& alias : candidate.aliases) {
                const int alias_score = name_match_score(query, alias);
                if (alias_score > score) {
                    score = alias_score;
                    matched_alias = alias;
                }
            }
        }
        if (score == 0) continue;

        std::uint64_t usage = usage_of(usage_counts, candidate.name);
        for (const auto& alias : candidate.aliases) {
            usage += usage_of(usage_counts, alias);
        }
        SlashCommandCandidate ranked_candidate = candidate;
        ranked_candidate.matched_alias = std::move(matched_alias);
        ranked.push_back({score, usage, std::move(ranked_candidate)});
    }

    std::sort(ranked.begin(), ranked.end(),
              [](const RankedCandidate& a, const RankedCandidate& b) {
                  if (a.match_score != b.match_score) {
                      return a.match_score > b.match_score;
                  }
                  if (a.usage_count != b.usage_count) {
                      return a.usage_count > b.usage_count;
                  }
                  return a.candidate.name < b.candidate.name;
              });

    std::vector<SlashCommandCandidate> result;
    result.reserve(ranked.size());
    for (auto& item : ranked) {
        result.push_back(std::move(item.candidate));
    }
    return result;
}

} // namespace acecode
