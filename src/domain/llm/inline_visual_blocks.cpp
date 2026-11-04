#include "inline_visual_blocks.hpp"

#include <algorithm>
#include <cctype>

namespace acecode {
namespace {

struct LineView {
    std::size_t begin = 0;   // 行首偏移
    std::size_t end = 0;     // 行尾偏移(不含 \r\n)
    std::string_view text;   // 不含 \r\n
};

std::vector<LineView> split_lines(std::string_view text) {
    std::vector<LineView> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t stop = newline == std::string_view::npos ? text.size() : newline;
        std::size_t content_end = stop;
        if (content_end > start && text[content_end - 1] == '\r') --content_end;
        lines.push_back({start, content_end, text.substr(start, content_end - start)});
        if (newline == std::string_view::npos) break;
        start = newline + 1;
    }
    return lines;
}

bool is_blank(std::string_view line) {
    return std::all_of(line.begin(), line.end(), [](char c) { return c == ' ' || c == '\t'; });
}

// 行首缩进的列数(Tab 按 4 列)与第一个非空白字符的位置。
int indent_columns(std::string_view line, std::size_t& first) {
    int columns = 0;
    first = 0;
    while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) {
        columns += line[first] == '\t' ? 4 - (columns % 4) : 1;
        ++first;
    }
    return columns;
}

struct FenceOpen {
    char marker = 0;
    std::size_t length = 0;
    std::string_view info;
};

// 从 first 起解析开围栏:至少 3 个 ` 或 ~;反引号围栏的信息串不能再含反引号。
bool parse_fence_open(std::string_view line, std::size_t first, FenceOpen& out) {
    if (first >= line.size() || (line[first] != '`' && line[first] != '~')) return false;
    const char marker = line[first];
    std::size_t run = first;
    while (run < line.size() && line[run] == marker) ++run;
    if (run - first < 3) return false;
    std::string_view info = line.substr(run);
    if (marker == '`' && info.find('`') != std::string_view::npos) return false;
    out.marker = marker;
    out.length = run - first;
    out.info = info;
    return true;
}

bool is_fence_close(std::string_view line, std::size_t first, const FenceOpen& open) {
    if (first >= line.size() || line[first] != open.marker) return false;
    std::size_t run = first;
    while (run < line.size() && line[run] == open.marker) ++run;
    if (run - first < open.length) return false;
    return is_blank(line.substr(run));
}

// 列表项标记(- + * 或 1. / 1));返回内容缩进列数,不是标记时返回 -1。
int list_content_indent(std::string_view line, std::size_t first, int indent) {
    if (indent > 3 || first >= line.size()) return -1;
    std::size_t pos = first;
    if (line[pos] == '-' || line[pos] == '+' || line[pos] == '*') {
        ++pos;
    } else {
        const std::size_t digits = pos;
        while (pos < line.size() && pos - digits < 9 &&
               std::isdigit(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        if (pos == digits || pos >= line.size() || (line[pos] != '.' && line[pos] != ')')) {
            return -1;
        }
        ++pos;
    }
    const int marker_width = static_cast<int>(pos - first);
    if (pos == line.size()) return indent + marker_width + 1;  // 空列表项
    if (line[pos] != ' ' && line[pos] != '\t') return -1;
    std::size_t spaces = 0;
    while (pos + spaces < line.size() && line[pos + spaces] == ' ') ++spaces;
    if (spaces == 0 || spaces > 4 || pos + spaces == line.size()) spaces = 1;
    return indent + marker_width + static_cast<int>(spaces);
}

std::string ascii_lower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

bool is_inline_visual_info(std::string_view info) {
    std::size_t start = 0;
    while (start < info.size() && (info[start] == ' ' || info[start] == '\t')) ++start;
    std::size_t stop = start;
    while (stop < info.size() && info[stop] != ' ' && info[stop] != '\t' && info[stop] != '{') {
        ++stop;
    }
    return ascii_lower(info.substr(start, stop - start)) == "visual";
}

std::vector<InlineVisualBlock> find_inline_visual_blocks(std::string_view markdown) {
    std::vector<InlineVisualBlock> blocks;
    const auto lines = split_lines(markdown);

    bool in_fence = false;
    FenceOpen fence;
    int fence_base = 0;        // 围栏所在容器的内容缩进(顶层为 0)
    bool fence_visual = false;
    std::size_t fence_begin = 0;
    int list_indent = 0;       // 当前列表项的内容缩进;0 = 不在列表里
    bool previous_blank = false;

    for (const auto& line : lines) {
        std::size_t first = 0;
        const int indent = indent_columns(line.text, first);

        if (in_fence) {
            if (indent - fence_base <= 3 && is_fence_close(line.text, first, fence)) {
                if (fence_visual) blocks.push_back({fence_begin, line.end, true});
                in_fence = false;
            }
            continue;
        }

        if (is_blank(line.text)) {
            previous_blank = true;
            continue;
        }

        if (list_indent > 0) {
            if (indent >= list_indent) {
                // 属于列表项的内容:里面的围栏是嵌套的,只跳过不记录。
                FenceOpen nested;
                if (indent - list_indent <= 3 && parse_fence_open(line.text, first, nested)) {
                    in_fence = true;
                    fence = nested;
                    fence_base = list_indent;
                    fence_visual = false;
                }
                previous_blank = false;
                continue;
            }
            const int next_item = list_content_indent(line.text, first, indent);
            if (next_item > 0) {
                list_indent = next_item;
                previous_blank = false;
                continue;
            }
            FenceOpen probe;
            const bool block_start = parse_fence_open(line.text, first, probe) ||
                                     (first < line.text.size() && line.text[first] == '>');
            if (!previous_blank && !block_start) {
                previous_blank = false;  // 段落的惰性续行,仍在列表里
                continue;
            }
            list_indent = 0;  // 缩进回到列表之外:列表结束
        }

        previous_blank = false;
        if (indent > 3) continue;  // 缩进代码块
        if (first < line.text.size() && line.text[first] == '>') continue;  // 引用块里的行
        FenceOpen open;
        if (parse_fence_open(line.text, first, open)) {
            in_fence = true;
            fence = open;
            fence_base = 0;
            fence_visual = is_inline_visual_info(open.info);
            fence_begin = line.begin;
            continue;
        }
        const int item = list_content_indent(line.text, first, indent);
        if (item > 0) list_indent = item;
    }

    if (in_fence && fence_visual) blocks.push_back({fence_begin, markdown.size(), false});
    return blocks;
}

std::string replace_inline_visual_blocks(std::string_view markdown, std::string_view placeholder) {
    const auto blocks = find_inline_visual_blocks(markdown);
    if (blocks.empty()) return std::string(markdown);
    std::string out;
    out.reserve(markdown.size());
    std::size_t cursor = 0;
    for (const auto& block : blocks) {
        out.append(markdown.substr(cursor, block.begin - cursor));
        out.append(placeholder);
        cursor = block.end;
    }
    out.append(markdown.substr(cursor));
    return out;
}

} // namespace acecode
