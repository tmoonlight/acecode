#include "diff_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <vector>

namespace acecode {

namespace {

constexpr size_t kDiffContextLines = 3;
// 中间段超过这个乘积就退回"整段替换",避免 O(n*m) 在超大文件上爆内存。
constexpr size_t kMaxLcsCells = 1500000;

// 按行切开,丢掉行尾 CR,这样 CRLF / LF / 混用不会把每一行都标成改动。
static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

enum class LineOpKind {
    Keep,
    Del,
    Add,
};

struct LineOp {
    LineOpKind kind = LineOpKind::Keep;
    size_t old_index = 0;
    size_t new_index = 0;
};

struct ChangeSpan {
    size_t old_start = 0;
    size_t old_end = 0;
    size_t new_start = 0;
    size_t new_end = 0;
};

static std::vector<LineOp> lcs_edit_script(
    const std::vector<std::string>& a,
    const std::vector<std::string>& b
) {
    const size_t n = a.size();
    const size_t m = b.size();
    std::vector<LineOp> ops;
    if (n == 0 && m == 0) return ops;

    // 超大中间段:不做 LCS,整段删再整段加(旧行为),保证工具路径可预测。
    if (n > 0 && m > 0 && n > kMaxLcsCells / m) {
        ops.reserve(n + m);
        for (size_t i = 0; i < n; ++i) ops.push_back({LineOpKind::Del, i, 0});
        for (size_t j = 0; j < m; ++j) ops.push_back({LineOpKind::Add, 0, j});
        return ops;
    }

    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            if (a[i - 1] == b[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1] + 1;
            } else {
                dp[i][j] = (dp[i - 1][j] >= dp[i][j - 1]) ? dp[i - 1][j] : dp[i][j - 1];
            }
        }
    }

    std::vector<LineOp> rev;
    size_t i = n;
    size_t j = m;
    while (i > 0 && j > 0) {
        if (a[i - 1] == b[j - 1]) {
            rev.push_back({LineOpKind::Keep, i - 1, j - 1});
            --i;
            --j;
        } else if (dp[i - 1][j] >= dp[i][j - 1]) {
            rev.push_back({LineOpKind::Del, i - 1, j});
            --i;
        } else {
            rev.push_back({LineOpKind::Add, i, j - 1});
            --j;
        }
    }
    while (i > 0) {
        rev.push_back({LineOpKind::Del, i - 1, j});
        --i;
    }
    while (j > 0) {
        rev.push_back({LineOpKind::Add, i, j - 1});
        --j;
    }
    std::reverse(rev.begin(), rev.end());
    return rev;
}

static std::vector<ChangeSpan> collect_change_spans(const std::vector<LineOp>& ops) {
    std::vector<ChangeSpan> spans;
    ChangeSpan current;
    bool open = false;
    size_t old_i = 0;
    size_t new_i = 0;

    auto flush = [&]() {
        if (!open) return;
        spans.push_back(current);
        open = false;
    };

    for (const auto& op : ops) {
        if (op.kind == LineOpKind::Keep) {
            flush();
            ++old_i;
            ++new_i;
            continue;
        }
        if (!open) {
            current = ChangeSpan{old_i, old_i, new_i, new_i};
            open = true;
        }
        if (op.kind == LineOpKind::Del) {
            current.old_end = ++old_i;
        } else {
            current.new_end = ++new_i;
        }
    }
    flush();
    return spans;
}

static std::vector<ChangeSpan> merge_change_spans(std::vector<ChangeSpan> spans) {
    if (spans.empty()) return spans;
    std::vector<ChangeSpan> merged;
    merged.push_back(spans.front());
    for (size_t i = 1; i < spans.size(); ++i) {
        auto& prev = merged.back();
        const auto& next = spans[i];
        const bool close_old = next.old_start <= prev.old_end + 2 * kDiffContextLines;
        const bool close_new = next.new_start <= prev.new_end + 2 * kDiffContextLines;
        if (close_old && close_new) {
            prev.old_end = next.old_end;
            prev.new_end = next.new_end;
        } else {
            merged.push_back(next);
        }
    }
    return merged;
}

static DiffHunk make_hunk(
    const std::vector<std::string>& old_lines,
    const std::vector<std::string>& new_lines,
    const ChangeSpan& change
) {
    const size_t pre = std::min({kDiffContextLines, change.old_start, change.new_start});
    const size_t post = std::min({
        kDiffContextLines,
        old_lines.size() - change.old_end,
        new_lines.size() - change.new_end
    });

    const size_t hunk_old_begin = change.old_start - pre;
    const size_t hunk_old_end = change.old_end + post;
    const size_t hunk_new_begin = change.new_start - pre;
    const size_t hunk_new_end = change.new_end + post;

    DiffHunk hunk;
    hunk.old_start = static_cast<int>(hunk_old_begin + 1);
    hunk.new_start = static_cast<int>(hunk_new_begin + 1);
    hunk.old_count = static_cast<int>(hunk_old_end - hunk_old_begin);
    hunk.new_count = static_cast<int>(hunk_new_end - hunk_new_begin);

    int next_old_no = hunk.old_start;
    int next_new_no = hunk.new_start;

    for (size_t i = hunk_old_begin; i < change.old_start; ++i) {
        DiffLine line;
        line.kind = DiffLineKind::Context;
        line.text = old_lines[i];
        line.old_line_no = next_old_no++;
        line.new_line_no = next_new_no++;
        hunk.lines.push_back(std::move(line));
    }
    for (size_t i = change.old_start; i < change.old_end; ++i) {
        DiffLine line;
        line.kind = DiffLineKind::Removed;
        line.text = old_lines[i];
        line.old_line_no = next_old_no++;
        hunk.lines.push_back(std::move(line));
    }
    for (size_t i = change.new_start; i < change.new_end; ++i) {
        DiffLine line;
        line.kind = DiffLineKind::Added;
        line.text = new_lines[i];
        line.new_line_no = next_new_no++;
        hunk.lines.push_back(std::move(line));
    }
    for (size_t i = change.old_end; i < hunk_old_end; ++i) {
        DiffLine line;
        line.kind = DiffLineKind::Context;
        line.text = old_lines[i];
        line.old_line_no = next_old_no++;
        line.new_line_no = next_new_no++;
        hunk.lines.push_back(std::move(line));
    }
    return hunk;
}

} // namespace

std::vector<DiffHunk> generate_structured_diff(
    const std::string& old_content,
    const std::string& new_content,
    const std::string& /*file_path*/
) {
    const auto old_lines = split_lines(old_content);
    const auto new_lines = split_lines(new_content);

    size_t prefix = 0;
    while (prefix < old_lines.size() && prefix < new_lines.size() &&
           old_lines[prefix] == new_lines[prefix]) {
        ++prefix;
    }

    size_t suffix = 0;
    while (suffix < (old_lines.size() - prefix) &&
           suffix < (new_lines.size() - prefix) &&
           old_lines[old_lines.size() - 1 - suffix] ==
               new_lines[new_lines.size() - 1 - suffix]) {
        ++suffix;
    }

    if (prefix + suffix == old_lines.size() && prefix + suffix == new_lines.size()) {
        return {};
    }

    const std::vector<std::string> old_mid(
        old_lines.begin() + static_cast<std::ptrdiff_t>(prefix),
        old_lines.end() - static_cast<std::ptrdiff_t>(suffix));
    const std::vector<std::string> new_mid(
        new_lines.begin() + static_cast<std::ptrdiff_t>(prefix),
        new_lines.end() - static_cast<std::ptrdiff_t>(suffix));

    auto ops = lcs_edit_script(old_mid, new_mid);
    for (auto& op : ops) {
        op.old_index += prefix;
        op.new_index += prefix;
    }

    // 公共前缀/后缀是 Keep,这样 hunk 能带上两侧 context。
    std::vector<LineOp> full_ops;
    full_ops.reserve(prefix + ops.size() + suffix);
    for (size_t i = 0; i < prefix; ++i) {
        full_ops.push_back({LineOpKind::Keep, i, i});
    }
    full_ops.insert(full_ops.end(), ops.begin(), ops.end());
    for (size_t i = 0; i < suffix; ++i) {
        const size_t old_i = old_lines.size() - suffix + i;
        const size_t new_i = new_lines.size() - suffix + i;
        full_ops.push_back({LineOpKind::Keep, old_i, new_i});
    }

    const auto spans = merge_change_spans(collect_change_spans(full_ops));
    std::vector<DiffHunk> hunks;
    hunks.reserve(spans.size());
    for (const auto& span : spans) {
        hunks.push_back(make_hunk(old_lines, new_lines, span));
    }
    return hunks;
}

std::string generate_unified_diff(
    const std::string& old_content,
    const std::string& new_content,
    const std::string& file_path,
    DiffStats& stats
) {
    stats.additions = 0;
    stats.deletions = 0;

    auto hunks = generate_structured_diff(old_content, new_content, file_path);

    std::ostringstream out;
    out << "--- " << file_path << "\n";
    out << "+++ " << file_path << "\n";

    if (hunks.empty()) {
        // 保持和旧实现的输出格式兼容:虽然没有 hunk,但标头仍输出。
        return out.str();
    }

    for (const auto& hunk : hunks) {
        out << "@@ -" << hunk.old_start << "," << hunk.old_count
            << " +" << hunk.new_start << "," << hunk.new_count << " @@\n";
        for (const auto& line : hunk.lines) {
            char marker = ' ';
            switch (line.kind) {
                case DiffLineKind::Added:
                    marker = '+';
                    ++stats.additions;
                    break;
                case DiffLineKind::Removed:
                    marker = '-';
                    ++stats.deletions;
                    break;
                case DiffLineKind::Context:
                    marker = ' ';
                    break;
            }
            out << marker << line.text << "\n";
        }
    }

    return out.str();
}

std::string generate_unified_diff(
    const std::string& old_content,
    const std::string& new_content,
    const std::string& file_path
) {
    DiffStats discard;
    return generate_unified_diff(old_content, new_content, file_path, discard);
}

} // namespace acecode
