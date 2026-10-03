#include <gtest/gtest.h>

#include "tui/commands/slash_command_ranking.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

std::vector<std::string> names_of(
    const std::vector<acecode::SlashCommandCandidate>& candidates) {
    std::vector<std::string> names;
    names.reserve(candidates.size());
    for (const auto& candidate : candidates) names.push_back(candidate.name);
    return names;
}

} // namespace

TEST(SlashCommandRanking, UsageReordersEquallyRelevantMatches) {
    const std::vector<acecode::SlashCommandCandidate> candidates = {
        {"alpha", "Alpha command"},
        {"help", "Show help"},
        {"zoom", "Zoom view"},
    };
    const acecode::SlashCommandUsageCounts usage = {
        {"help", 3},
        {"zoom", 8},
    };

    const auto ranked =
        acecode::rank_slash_command_candidates("", candidates, usage);

    EXPECT_EQ(names_of(ranked),
              (std::vector<std::string>{"zoom", "help", "alpha"}));
}

TEST(SlashCommandRanking, MatchRelevancePrecedesUsage) {
    const std::vector<acecode::SlashCommandCandidate> candidates = {
        {"config", "Change model options"},
        {"model", "Choose a model"},
        {"remote", "Remote control"},
    };
    const acecode::SlashCommandUsageCounts usage = {
        {"config", 1000},
        {"remote", 500},
    };

    const auto ranked =
        acecode::rank_slash_command_candidates("mo", candidates, usage);

    EXPECT_EQ(names_of(ranked),
              (std::vector<std::string>{"model", "remote", "config"}));
}

TEST(SlashCommandRanking, AlphabeticalOrderBreaksExactTie) {
    const std::vector<acecode::SlashCommandCandidate> candidates = {
        {"zeta", "Zeta"},
        {"beta", "Beta"},
        {"alpha", "Alpha"},
    };
    const acecode::SlashCommandUsageCounts usage = {
        {"alpha", 4},
        {"beta", 4},
        {"zeta", 4},
    };

    const auto ranked =
        acecode::rank_slash_command_candidates("", candidates, usage);

    EXPECT_EQ(names_of(ranked),
              (std::vector<std::string>{"alpha", "beta", "zeta"}));
}

TEST(SlashCommandRanking, UnmatchedCandidatesAreFilteredOut) {
    const std::vector<acecode::SlashCommandCandidate> candidates = {
        {"model", "Choose a model"},
        {"help", "Show all commands"},
        {"config", "Runtime settings"},
    };
    const acecode::SlashCommandUsageCounts usage = {{"help", 999}};

    const auto ranked =
        acecode::rank_slash_command_candidates("model", candidates, usage);

    ASSERT_EQ(ranked.size(), 1u);
    EXPECT_EQ(ranked.front().name, "model");
}

TEST(SlashCommandRanking, ExactNameBeatsUsageAndDescriptionMatches) {
    const std::vector<acecode::SlashCommandCandidate> candidates = {
        {"skills", "Open skill management"},
        {"skill-creator", "Create and maintain reusable skills"},
        {"find-skills", "Discover skills"},
    };
    const acecode::SlashCommandUsageCounts usage = {
        {"skill-creator", 1000},
        {"find-skills", 2000},
    };

    const auto ranked =
        acecode::rank_slash_command_candidates("skills", candidates, usage);

    ASSERT_FALSE(ranked.empty());
    EXPECT_EQ(ranked.front().name, "skills");
}

// 场景:三名命令 generate(别名 new、create),用户分别敲 "new" 与 "create"。
// 期望:命令只排出一条 generate,matched_alias 是用户敲中的那个别名,
// 下拉据此显示 "/generate (new)" 或 "/generate (create)"。
TEST(SlashCommandRanking, TypedAliasIsReportedAsMatchedAlias) {
    const std::vector<acecode::SlashCommandCandidate> candidates = {
        {"generate", "Generate a scaffold", {"new", "create"}},
        {"help", "Show help"},
    };

    const auto by_new =
        acecode::rank_slash_command_candidates("new", candidates, {});
    ASSERT_EQ(by_new.size(), 1u);
    EXPECT_EQ(by_new.front().name, "generate");
    EXPECT_EQ(by_new.front().matched_alias, "new");

    const auto by_create =
        acecode::rank_slash_command_candidates("create", candidates, {});
    ASSERT_EQ(by_create.size(), 1u);
    EXPECT_EQ(by_create.front().matched_alias, "create");
}

// 场景:空查询,或查询命中原名不比命中别名差("gen" 只命中原名前缀)。
// 期望:matched_alias 为空 —— 没敲别名就不展示别名。
TEST(SlashCommandRanking, CanonicalOrEmptyQueryLeavesMatchedAliasEmpty) {
    const std::vector<acecode::SlashCommandCandidate> candidates = {
        {"generate", "Generate a scaffold", {"new", "create"}},
    };

    for (const char* query : {"", "gen", "generate"}) {
        const auto ranked =
            acecode::rank_slash_command_candidates(query, candidates, {});
        ASSERT_EQ(ranked.size(), 1u) << query;
        EXPECT_TRUE(ranked.front().matched_alias.empty()) << query;
    }
}

// 场景:完整敲出别名 "rc",另有命令 search 的名字里含 "rc"、且使用次数更高。
// 期望:别名按原名同档位打分(完全匹配 200)排第一,名字子串命中(50)排后面,
// 使用次数只在同档内起作用。
TEST(SlashCommandRanking, ExactAliasRanksLikeExactName) {
    const std::vector<acecode::SlashCommandCandidate> candidates = {
        {"search", "Search sources", {}},
        {"remote-control", "Manage remote control", {"rc"}},
    };
    const acecode::SlashCommandUsageCounts usage = {{"search", 1000}};

    const auto ranked =
        acecode::rank_slash_command_candidates("rc", candidates, usage);

    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked.front().name, "remote-control");
    EXPECT_EQ(ranked.front().matched_alias, "rc");
    EXPECT_TRUE(ranked.back().matched_alias.empty());
}

// 场景:旧版本把别名当独立命令,使用次数按别名单独落在 state.json(如 "new": 9)。
// 期望:排序时原名的使用次数 = 原名 + 全部别名之和,历史用量不因合并而丢失。
TEST(SlashCommandRanking, UsageIncludesLegacyAliasCounts) {
    const std::vector<acecode::SlashCommandCandidate> candidates = {
        {"clear", "Clear conversation", {"new"}},
        {"compact", "Compress conversation"},
    };
    const acecode::SlashCommandUsageCounts usage = {
        {"compact", 5},
        {"new", 9},
    };

    const auto ranked =
        acecode::rank_slash_command_candidates("", candidates, usage);

    EXPECT_EQ(names_of(ranked),
              (std::vector<std::string>{"clear", "compact"}));
}
