// 覆盖 src/tool/diff_utils.cpp 的 DiffStats 输出路径 + 新增的
// generate_structured_diff 结构化输出。
// DiffStats 场景:
//   1. 纯新增行(旧空 → 新 3 行)
//   2. 纯删除行(旧 3 行 → 新空)
//   3. 同时增删(替换一行)
//   4. 内容完全相同时增删均为 0(不生成空 hunk 的 additions/deletions)
// Structured diff 场景:
//   A. 空 → 非空 — 期望单 hunk,全是 Added,old_line_no 全空
//   B. 单行替换 — 期望 Removed / Added 各 1,行号与 marker 对应
//   C. 前后都有 context — 期望 context 行在 Removed/Added 两侧,且行号累加正确
//   D. 内容相同 — 期望结构化结果为空向量(无 hunk)
//   E. 两处相隔较远的替换 — 期望拆成两个 hunk,而不是中间整段替换
//   F. 纯 CRLF/LF 差异 — 期望空 hunk / 零统计

#include <gtest/gtest.h>

#include "tool/diff_utils.hpp"

using acecode::DiffLineKind;
using acecode::DiffStats;
using acecode::generate_structured_diff;
using acecode::generate_unified_diff;

// 场景 1: 文件从空变为 3 行,期待 additions=3 / deletions=0。
TEST(DiffStats, PureAdditions) {
    DiffStats stats;
    auto diff = generate_unified_diff("", "a\nb\nc\n", "/tmp/f.txt", stats);
    EXPECT_EQ(stats.additions, 3);
    EXPECT_EQ(stats.deletions, 0);
    EXPECT_NE(diff.find("+a"), std::string::npos);
}

// 场景 2: 文件从 3 行被整体删光,期待 additions=0 / deletions=3。
TEST(DiffStats, PureDeletions) {
    DiffStats stats;
    auto diff = generate_unified_diff("a\nb\nc\n", "", "/tmp/f.txt", stats);
    EXPECT_EQ(stats.additions, 0);
    EXPECT_EQ(stats.deletions, 3);
    EXPECT_NE(diff.find("-a"), std::string::npos);
}

// 场景 3: 中间一行被替换为另外两行(旧:a/b/c,新:a/X/Y/c),
// 期待 additions=2 / deletions=1。
TEST(DiffStats, SimultaneousAddDelete) {
    DiffStats stats;
    auto diff = generate_unified_diff("a\nb\nc\n", "a\nX\nY\nc\n", "/tmp/f.txt", stats);
    EXPECT_EQ(stats.additions, 2);
    EXPECT_EQ(stats.deletions, 1);
}

// 场景 4: 内容一字未改,期待 additions/deletions 都是 0。
// (verify stats are zeroed even when the caller reused a stats object.)
TEST(DiffStats, NoChangeZeroStats) {
    DiffStats stats;
    stats.additions = 99;
    stats.deletions = 99;
    (void)generate_unified_diff("a\nb\nc\n", "a\nb\nc\n", "/tmp/f.txt", stats);
    EXPECT_EQ(stats.additions, 0);
    EXPECT_EQ(stats.deletions, 0);
}

// 场景 A: 空文件 → 3 行新内容。期望 1 个 hunk,全部为 Added,
// 每行的 old_line_no 为空(nullopt),new_line_no 从 1 递增。
TEST(StructuredDiff, EmptyToNonEmpty) {
    auto hunks = generate_structured_diff("", "a\nb\nc\n", "/tmp/f.txt");
    ASSERT_EQ(hunks.size(), 1u);
    const auto& h = hunks[0];
    EXPECT_EQ(h.old_count, 0);
    EXPECT_EQ(h.new_start, 1);
    ASSERT_EQ(h.lines.size(), 3u);
    for (size_t i = 0; i < h.lines.size(); ++i) {
        EXPECT_EQ(h.lines[i].kind, DiffLineKind::Added);
        EXPECT_FALSE(h.lines[i].old_line_no.has_value());
        ASSERT_TRUE(h.lines[i].new_line_no.has_value());
        EXPECT_EQ(*h.lines[i].new_line_no, static_cast<int>(i + 1));
    }
}

// 场景 B: 单行文件被替换成另一行。期望一个 hunk,Removed + Added 成对出现,
// 行号与 marker 都正确。
TEST(StructuredDiff, SingleLineReplacement) {
    auto hunks = generate_structured_diff("alpha\n", "beta\n", "/tmp/f.txt");
    ASSERT_EQ(hunks.size(), 1u);
    const auto& h = hunks[0];
    ASSERT_EQ(h.lines.size(), 2u);
    EXPECT_EQ(h.lines[0].kind, DiffLineKind::Removed);
    EXPECT_EQ(h.lines[0].text, "alpha");
    EXPECT_EQ(h.lines[0].old_line_no.value_or(-1), 1);
    EXPECT_FALSE(h.lines[0].new_line_no.has_value());
    EXPECT_EQ(h.lines[1].kind, DiffLineKind::Added);
    EXPECT_EQ(h.lines[1].text, "beta");
    EXPECT_FALSE(h.lines[1].old_line_no.has_value());
    EXPECT_EQ(h.lines[1].new_line_no.value_or(-1), 1);
}

// 场景 C: 前后各有 context 行,中间替换一行。验证 context 行号累加 +
// Removed/Added 行两侧行号的空/非空状态。
TEST(StructuredDiff, WithContextAround) {
    // 旧: a / b / c / d / e
    // 新: a / b / X / d / e
    auto hunks = generate_structured_diff(
        "a\nb\nc\nd\ne\n",
        "a\nb\nX\nd\ne\n",
        "/tmp/f.txt");
    ASSERT_EQ(hunks.size(), 1u);
    const auto& h = hunks[0];

    // 期望顺序: [ctx a, ctx b, Removed c, Added X, ctx d, ctx e]
    ASSERT_EQ(h.lines.size(), 6u);

    EXPECT_EQ(h.lines[0].kind, DiffLineKind::Context);
    EXPECT_EQ(h.lines[0].text, "a");
    EXPECT_EQ(h.lines[0].old_line_no.value_or(-1), 1);
    EXPECT_EQ(h.lines[0].new_line_no.value_or(-1), 1);

    EXPECT_EQ(h.lines[1].kind, DiffLineKind::Context);
    EXPECT_EQ(h.lines[1].old_line_no.value_or(-1), 2);
    EXPECT_EQ(h.lines[1].new_line_no.value_or(-1), 2);

    EXPECT_EQ(h.lines[2].kind, DiffLineKind::Removed);
    EXPECT_EQ(h.lines[2].text, "c");
    EXPECT_EQ(h.lines[2].old_line_no.value_or(-1), 3);
    EXPECT_FALSE(h.lines[2].new_line_no.has_value());

    EXPECT_EQ(h.lines[3].kind, DiffLineKind::Added);
    EXPECT_EQ(h.lines[3].text, "X");
    EXPECT_FALSE(h.lines[3].old_line_no.has_value());
    EXPECT_EQ(h.lines[3].new_line_no.value_or(-1), 3);

    // Context after: d / e —— old/new 行号都应继续递增
    EXPECT_EQ(h.lines[4].kind, DiffLineKind::Context);
    EXPECT_EQ(h.lines[4].old_line_no.value_or(-1), 4);
    EXPECT_EQ(h.lines[4].new_line_no.value_or(-1), 4);
    EXPECT_EQ(h.lines[5].kind, DiffLineKind::Context);
    EXPECT_EQ(h.lines[5].old_line_no.value_or(-1), 5);
    EXPECT_EQ(h.lines[5].new_line_no.value_or(-1), 5);
}

// 场景 D: 内容完全相同时,结构化 diff 应返回空向量(没有任何 hunk)。
TEST(StructuredDiff, NoChangeEmptyHunks) {
    auto hunks = generate_structured_diff("x\ny\n", "x\ny\n", "/tmp/f.txt");
    EXPECT_TRUE(hunks.empty());
}

// 两处相隔较远的单行替换不应被合成一个"中间整段替换"。
// 回归:旧算法只剥公共前后缀,会把 3..133 行全部标成改动。
TEST(StructuredDiff, DistantEditsStaySeparateHunks) {
    std::string old_text;
    std::string new_text;
    for (int i = 1; i <= 140; ++i) {
        const std::string line = "line-" + std::to_string(i) + "\n";
        old_text += line;
        if (i == 3) new_text += "changed-3\n";
        else if (i == 133) new_text += "changed-133\n";
        else new_text += line;
    }

    DiffStats stats;
    (void)generate_unified_diff(old_text, new_text, "/tmp/f.txt", stats);
    EXPECT_EQ(stats.additions, 2);
    EXPECT_EQ(stats.deletions, 2);

    auto hunks = generate_structured_diff(old_text, new_text, "/tmp/f.txt");
    ASSERT_EQ(hunks.size(), 2u);
    EXPECT_EQ(hunks[0].old_start, 1);
    EXPECT_EQ(hunks[0].new_start, 1);
    EXPECT_EQ(hunks[1].old_start, 130);
    EXPECT_EQ(hunks[1].new_start, 130);

    int removed = 0;
    int added = 0;
    for (const auto& hunk : hunks) {
        for (const auto& line : hunk.lines) {
            if (line.kind == DiffLineKind::Removed) ++removed;
            if (line.kind == DiffLineKind::Added) ++added;
        }
    }
    EXPECT_EQ(removed, 2);
    EXPECT_EQ(added, 2);
}

// 纯换行风格差异(LF vs CRLF)不算内容改动。
TEST(StructuredDiff, CrlfOnlyDifferenceIsEmpty) {
    auto hunks = generate_structured_diff("a\nb\nc\n", "a\r\nb\r\nc\r\n", "/tmp/f.txt");
    EXPECT_TRUE(hunks.empty());

    DiffStats stats;
    (void)generate_unified_diff("a\nb\nc\n", "a\r\nb\r\nc\r\n", "/tmp/f.txt", stats);
    EXPECT_EQ(stats.additions, 0);
    EXPECT_EQ(stats.deletions, 0);
}
