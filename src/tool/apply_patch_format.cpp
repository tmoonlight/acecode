#include "apply_patch_format.hpp"

#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <tuple>

namespace acecode::apply_patch {

namespace {

constexpr const char* kBeginMarker = "*** Begin Patch";
constexpr const char* kEndMarker = "*** End Patch";
constexpr const char* kEndOfFileMarker = "*** End of File";
constexpr const char* kAddHeader = "*** Add File:";
constexpr const char* kDeleteHeader = "*** Delete File:";
constexpr const char* kUpdateHeader = "*** Update File:";
constexpr const char* kMoveHeader = "*** Move to:";

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string rstrip(std::string value) {
    while (!value.empty() && is_space(value.back())) value.pop_back();
    return value;
}

std::string trim(std::string value) {
    value = rstrip(std::move(value));
    std::size_t start = 0;
    while (start < value.size() && is_space(value[start])) ++start;
    return value.substr(start);
}

bool starts_with(const std::string& value, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    return value.size() >= n && value.compare(0, n, prefix) == 0;
}

std::string normalize_newlines(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::vector<std::string> split_lines(const std::string& lf_text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : lf_text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    lines.push_back(current);
    return lines;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out.push_back('\n');
        out += lines[i];
    }
    return out;
}

bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// `apply_patch <<'EOF' ... EOF` / `cat <<EOF ... EOF`:模型偶尔把 Codex 的 shell
// 调用形态原样塞进参数。识别首行的 heredoc 起始与末行的终止符即可剥掉。
void strip_heredoc(std::vector<std::string>& lines) {
    while (!lines.empty() && trim(lines.front()).empty()) lines.erase(lines.begin());
    while (!lines.empty() && trim(lines.back()).empty()) lines.pop_back();
    if (lines.size() < 2) return;

    const std::string first = trim(lines.front());
    const std::size_t marker = first.find("<<");
    if (marker == std::string::npos) return;
    std::string tag = trim(first.substr(marker + 2));
    if (tag.empty()) return;
    if (tag.front() == '\'' || tag.front() == '"') {
        const char quote = tag.front();
        const std::size_t close = tag.find(quote, 1);
        if (close == std::string::npos) return;
        tag = tag.substr(1, close - 1);
    } else if (tag.front() == '-') {
        tag = trim(tag.substr(1));
    }
    if (tag.empty() || !std::all_of(tag.begin(), tag.end(), is_word_char)) return;
    if (trim(lines.back()) != tag) return;
    lines.erase(lines.begin());
    lines.pop_back();
}

void strip_code_fence(std::vector<std::string>& lines) {
    while (!lines.empty() && trim(lines.front()).empty()) lines.erase(lines.begin());
    while (!lines.empty() && trim(lines.back()).empty()) lines.pop_back();
    if (lines.size() < 2) return;
    if (!starts_with(trim(lines.front()), "```") || trim(lines.back()) != "```") return;
    lines.erase(lines.begin());
    lines.pop_back();
}

std::string line_label(std::size_t index) {
    return "line " + std::to_string(index + 1);
}

std::string preview_line(const std::string& line) {
    const std::size_t kMax = 80;
    if (line.size() <= kMax) return line;
    return line.substr(0, kMax) + "...";
}

ParseResult fail(const std::string& error) {
    ParseResult result;
    result.success = false;
    result.error = error;
    return result;
}

using Comparator = std::function<bool(const std::string&, const std::string&)>;

// 返回匹配起点;-1 = 没找到。eof 为 true 时先试从文件末尾对齐。
long long try_match(const std::vector<std::string>& lines,
                    const std::vector<std::string>& pattern,
                    std::size_t start_index,
                    const Comparator& compare,
                    bool eof) {
    if (pattern.empty() || lines.size() < pattern.size()) return -1;
    const std::size_t last_start = lines.size() - pattern.size();

    auto matches_at = [&](std::size_t at) {
        for (std::size_t j = 0; j < pattern.size(); ++j) {
            if (!compare(lines[at + j], pattern[j])) return false;
        }
        return true;
    };

    if (eof && last_start >= start_index && matches_at(last_start)) {
        return static_cast<long long>(last_start);
    }
    for (std::size_t i = start_index; i <= last_start; ++i) {
        if (matches_at(i)) return static_cast<long long>(i);
    }
    return -1;
}

long long seek_sequence(const std::vector<std::string>& lines,
                        const std::vector<std::string>& pattern,
                        std::size_t start_index,
                        bool eof) {
    if (pattern.empty()) return -1;
    const long long exact = try_match(
        lines, pattern, start_index,
        [](const std::string& a, const std::string& b) { return a == b; }, eof);
    if (exact != -1) return exact;
    const long long rstripped = try_match(
        lines, pattern, start_index,
        [](const std::string& a, const std::string& b) { return rstrip(a) == rstrip(b); },
        eof);
    if (rstripped != -1) return rstripped;
    const long long trimmed = try_match(
        lines, pattern, start_index,
        [](const std::string& a, const std::string& b) { return trim(a) == trim(b); },
        eof);
    if (trimmed != -1) return trimmed;
    return try_match(
        lines, pattern, start_index,
        [](const std::string& a, const std::string& b) {
            return normalize_unicode_punctuation(trim(a)) ==
                   normalize_unicode_punctuation(trim(b));
        },
        eof);
}

} // namespace

std::string normalize_unicode_punctuation(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char b0 = static_cast<unsigned char>(text[i]);
        // U+2010..U+2015 破折号族、U+2018..U+201F 引号族、U+2026 省略号:
        // 都是 E2 80 xx 三字节序列。
        if (b0 == 0xE2 && i + 2 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0x80) {
            const unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
            if (b2 >= 0x90 && b2 <= 0x95) {
                out.push_back('-');
                i += 3;
                continue;
            }
            if (b2 >= 0x98 && b2 <= 0x9B) {
                out.push_back('\'');
                i += 3;
                continue;
            }
            if (b2 >= 0x9C && b2 <= 0x9F) {
                out.push_back('"');
                i += 3;
                continue;
            }
            if (b2 == 0xA6) {
                out += "...";
                i += 3;
                continue;
            }
        }
        // U+00A0 不换行空格:C2 A0。
        if (b0 == 0xC2 && i + 1 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0xA0) {
            out.push_back(' ');
            i += 2;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

ParseResult parse_patch(const std::string& patch_text) {
    std::vector<std::string> lines = split_lines(normalize_newlines(patch_text));
    strip_code_fence(lines);
    strip_heredoc(lines);

    std::size_t begin = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (trim(lines[i]) == kBeginMarker) {
            begin = i;
            break;
        }
    }
    if (begin == lines.size()) {
        return fail("Invalid patch format: missing '*** Begin Patch' marker");
    }
    std::size_t end = lines.size();
    for (std::size_t i = begin + 1; i < lines.size(); ++i) {
        if (trim(lines[i]) == kEndMarker) {
            end = i;
            break;
        }
    }
    if (end == lines.size()) {
        return fail("Invalid patch format: missing '*** End Patch' marker");
    }

    ParseResult result;
    result.success = true;
    std::size_t i = begin + 1;
    while (i < end) {
        const std::string& line = lines[i];
        if (starts_with(line, kAddHeader)) {
            PatchHunk hunk;
            hunk.kind = HunkKind::Add;
            hunk.path = trim(line.substr(std::char_traits<char>::length(kAddHeader)));
            if (hunk.path.empty()) {
                return fail("Missing file path in '*** Add File:' header at " + line_label(i));
            }
            ++i;
            while (i < end && !starts_with(lines[i], "***")) {
                const std::string& body = lines[i];
                if (!body.empty() && body[0] == '+') {
                    hunk.contents += body.substr(1);
                    hunk.contents.push_back('\n');
                } else if (trim(body).empty()) {
                    // 与 opencode 一致:Add 段里没带 `+` 的空行当作段落之间的
                    // 分隔符忽略(工具描述已要求内容行都带 `+`)。
                } else {
                    return fail("Unexpected line in '*** Add File: " + hunk.path +
                                "' section at " + line_label(i) +
                                " (every content line must start with '+'): " +
                                preview_line(body));
                }
                ++i;
            }
            result.patch.hunks.push_back(std::move(hunk));
            continue;
        }
        if (starts_with(line, kDeleteHeader)) {
            PatchHunk hunk;
            hunk.kind = HunkKind::Delete;
            hunk.path = trim(line.substr(std::char_traits<char>::length(kDeleteHeader)));
            if (hunk.path.empty()) {
                return fail("Missing file path in '*** Delete File:' header at " + line_label(i));
            }
            ++i;
            result.patch.hunks.push_back(std::move(hunk));
            continue;
        }
        if (starts_with(line, kUpdateHeader)) {
            PatchHunk hunk;
            hunk.kind = HunkKind::Update;
            hunk.path = trim(line.substr(std::char_traits<char>::length(kUpdateHeader)));
            if (hunk.path.empty()) {
                return fail("Missing file path in '*** Update File:' header at " + line_label(i));
            }
            ++i;
            if (i < end && starts_with(lines[i], kMoveHeader)) {
                hunk.move_path = trim(lines[i].substr(std::char_traits<char>::length(kMoveHeader)));
                if (hunk.move_path.empty()) {
                    return fail("Missing target path in '*** Move to:' header at " + line_label(i));
                }
                ++i;
            }
            while (i < end) {
                const std::string& body = lines[i];
                if (trim(body) == kEndOfFileMarker) {
                    if (hunk.chunks.empty()) hunk.chunks.emplace_back();
                    hunk.chunks.back().is_end_of_file = true;
                    ++i;
                    continue;
                }
                if (starts_with(body, "***")) break;
                if (starts_with(body, "@@")) {
                    UpdateChunk chunk;
                    chunk.change_context = trim(body.substr(2));
                    hunk.chunks.push_back(std::move(chunk));
                    ++i;
                    continue;
                }
                // 第一个 chunk 允许省略 `@@`:隐式空锚点。但 header 与首个 `@@`
                // 之间的空行只是分隔符,不能凭它开出一个只含空上下文行的 chunk。
                if (hunk.chunks.empty() && body.empty()) {
                    ++i;
                    continue;
                }
                if (hunk.chunks.empty()) hunk.chunks.emplace_back();
                UpdateChunk& chunk = hunk.chunks.back();
                if (body.empty()) {
                    chunk.old_lines.emplace_back();
                    chunk.new_lines.emplace_back();
                } else if (body[0] == ' ') {
                    chunk.old_lines.push_back(body.substr(1));
                    chunk.new_lines.push_back(body.substr(1));
                } else if (body[0] == '-') {
                    chunk.old_lines.push_back(body.substr(1));
                } else if (body[0] == '+') {
                    chunk.new_lines.push_back(body.substr(1));
                } else {
                    return fail("Unexpected line in '*** Update File: " + hunk.path +
                                "' section at " + line_label(i) +
                                " (expected a line starting with ' ', '-', '+', '@@' or '***'): " +
                                preview_line(body));
                }
                ++i;
            }
            // 只由空行(段落分隔)组成的 chunk 不是变更:去掉,免得它去文件里
            // 找一行空行、找不到就让整份补丁失败。
            hunk.chunks.erase(
                std::remove_if(hunk.chunks.begin(), hunk.chunks.end(),
                               [](const UpdateChunk& chunk) {
                                   auto all_blank = [](const std::vector<std::string>& v) {
                                       return std::all_of(v.begin(), v.end(),
                                                          [](const std::string& s) {
                                                              return trim(s).empty();
                                                          });
                                   };
                                   return chunk.change_context.empty() &&
                                          !chunk.is_end_of_file &&
                                          all_blank(chunk.old_lines) &&
                                          all_blank(chunk.new_lines);
                               }),
                hunk.chunks.end());
            bool has_change = false;
            for (const auto& chunk : hunk.chunks) {
                if (!chunk.old_lines.empty() || !chunk.new_lines.empty()) {
                    has_change = true;
                    break;
                }
            }
            if (!has_change) {
                return fail("'*** Update File: " + hunk.path +
                            "' section has no change lines");
            }
            result.patch.hunks.push_back(std::move(hunk));
            continue;
        }
        if (trim(line).empty()) {
            ++i;
            continue;
        }
        return fail("Unexpected line at " + line_label(i) +
                    " (expected '*** Add File:', '*** Delete File:' or '*** Update File:'): " +
                    preview_line(line));
    }
    return result;
}

DeriveResult derive_new_contents(const std::string& file_path,
                                 const std::vector<UpdateChunk>& chunks,
                                 const std::string& original_lf_text) {
    DeriveResult result;
    const bool ends_with_newline =
        original_lf_text.empty() || original_lf_text.back() == '\n';
    std::vector<std::string> lines;
    if (!original_lf_text.empty()) {
        lines = split_lines(original_lf_text);
        if (ends_with_newline && !lines.empty() && lines.back().empty()) lines.pop_back();
    }

    // (起始行, 旧行数, 新行)
    std::vector<std::tuple<std::size_t, std::size_t, std::vector<std::string>>> replacements;
    std::size_t line_index = 0;
    for (const auto& chunk : chunks) {
        if (!chunk.change_context.empty()) {
            const long long ctx = seek_sequence(lines, {chunk.change_context}, line_index, false);
            if (ctx == -1) {
                result.error = "Failed to find context '" + chunk.change_context +
                               "' in " + file_path;
                return result;
            }
            line_index = static_cast<std::size_t>(ctx) + 1;
        }

        if (chunk.old_lines.empty()) {
            // 纯新增且无旧行:与上游一致,追加到文件末尾。
            replacements.emplace_back(lines.size(), 0, chunk.new_lines);
            continue;
        }

        std::vector<std::string> pattern = chunk.old_lines;
        std::vector<std::string> new_slice = chunk.new_lines;
        long long found = seek_sequence(lines, pattern, line_index, chunk.is_end_of_file);
        if (found == -1 && !pattern.empty() && pattern.back().empty()) {
            pattern.pop_back();
            if (!new_slice.empty() && new_slice.back().empty()) new_slice.pop_back();
            found = seek_sequence(lines, pattern, line_index, chunk.is_end_of_file);
        }
        if (found == -1) {
            result.error = "Failed to find expected lines in " + file_path + ":\n" +
                           join_lines(chunk.old_lines);
            return result;
        }
        replacements.emplace_back(static_cast<std::size_t>(found), pattern.size(), new_slice);
        line_index = static_cast<std::size_t>(found) + pattern.size();
    }

    std::stable_sort(replacements.begin(), replacements.end(),
                     [](const auto& a, const auto& b) {
                         return std::get<0>(a) < std::get<0>(b);
                     });
    for (auto it = replacements.rbegin(); it != replacements.rend(); ++it) {
        const std::size_t start = std::get<0>(*it);
        const std::size_t old_len = std::get<1>(*it);
        const auto& new_segment = std::get<2>(*it);
        const std::size_t clamped_start = (std::min)(start, lines.size());
        const std::size_t clamped_len = (std::min)(old_len, lines.size() - clamped_start);
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(clamped_start),
                    lines.begin() + static_cast<std::ptrdiff_t>(clamped_start + clamped_len));
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(clamped_start),
                     new_segment.begin(), new_segment.end());
    }

    result.content = join_lines(lines);
    if (ends_with_newline && !lines.empty()) result.content.push_back('\n');
    result.success = true;
    return result;
}

std::string patch_text_from_arguments(const std::string& arguments_json) {
    const auto args = nlohmann::json::parse(arguments_json, nullptr, false);
    if (!args.is_object()) return {};
    for (const char* key : {"input", "patchText", "patch"}) {
        const auto it = args.find(key);
        if (it != args.end() && it->is_string()) {
            const std::string value = it->get<std::string>();
            if (!value.empty()) return value;
        }
    }
    return {};
}

std::vector<PatchFileHeader> summarize_patch_headers(const std::string& patch_text) {
    std::vector<PatchFileHeader> headers;
    for (const std::string& raw : split_lines(normalize_newlines(patch_text))) {
        const std::string line = trim(raw);
        if (starts_with(line, kAddHeader)) {
            headers.push_back({HunkKind::Add,
                               trim(line.substr(std::char_traits<char>::length(kAddHeader))),
                               {}});
        } else if (starts_with(line, kDeleteHeader)) {
            headers.push_back({HunkKind::Delete,
                               trim(line.substr(std::char_traits<char>::length(kDeleteHeader))),
                               {}});
        } else if (starts_with(line, kUpdateHeader)) {
            headers.push_back({HunkKind::Update,
                               trim(line.substr(std::char_traits<char>::length(kUpdateHeader))),
                               {}});
        } else if (starts_with(line, kMoveHeader) && !headers.empty() &&
                   headers.back().kind == HunkKind::Update) {
            headers.back().move_path =
                trim(line.substr(std::char_traits<char>::length(kMoveHeader)));
        }
    }
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [](const PatchFileHeader& h) { return h.path.empty(); }),
                  headers.end());
    return headers;
}

std::string resolve_patch_path(const std::string& raw_path, const std::string& cwd) {
    std::filesystem::path path = path_from_utf8(raw_path);
    if (path.is_relative() && !cwd.empty()) {
        path = path_from_utf8(cwd) / path;
    }
    return path_to_utf8(path.lexically_normal());
}

std::vector<std::string> extract_target_paths(const std::string& arguments_json,
                                              const std::string& cwd) {
    std::vector<std::string> paths;
    const std::string text = patch_text_from_arguments(arguments_json);
    if (text.empty()) return paths;
    const ParseResult parsed = parse_patch(text);
    if (!parsed.success) return paths;
    auto add = [&](const std::string& raw) {
        if (raw.empty()) return;
        const std::string resolved = resolve_patch_path(raw, cwd);
        if (std::find(paths.begin(), paths.end(), resolved) == paths.end()) {
            paths.push_back(resolved);
        }
    };
    for (const auto& hunk : parsed.patch.hunks) {
        add(hunk.path);
        add(hunk.move_path);
    }
    return paths;
}

} // namespace acecode::apply_patch
