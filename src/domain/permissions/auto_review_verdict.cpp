#include "auto_review_verdict.hpp"

#include <cstddef>

namespace acecode::auto_review {
namespace {

// 理由只是一句话;超过这个长度多半是模型把整段分析塞了进来,截短即可。
constexpr std::size_t kMaxReasonBytes = 400;

char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

// 大小写不敏感地找 needle(needle 必须已是小写 ASCII)。
std::size_t find_ci(const std::string& text, const char* needle, std::size_t from) {
    std::size_t len = 0;
    while (needle[len] != '\0') ++len;
    if (len == 0 || text.size() < len) return std::string::npos;
    for (std::size_t i = from; i + len <= text.size(); ++i) {
        std::size_t k = 0;
        while (k < len && lower_ascii(text[i + k]) == needle[k]) ++k;
        if (k == len) return i;
    }
    return std::string::npos;
}

// 去掉一种思考块:成对的整块删除,只有开标签没有闭标签的从开标签删到末尾。
// 思考里常出现「也许该 <block>yes</block>」这类自言自语,不能算结论。
std::string strip_block(const std::string& text, const char* open, const char* close) {
    std::size_t close_len = 0;
    while (close[close_len] != '\0') ++close_len;
    std::string out;
    std::size_t pos = 0;
    while (true) {
        const std::size_t start = find_ci(text, open, pos);
        if (start == std::string::npos) {
            out.append(text, pos, std::string::npos);
            return out;
        }
        out.append(text, pos, start - pos);
        const std::size_t end = find_ci(text, close, start);
        if (end == std::string::npos) return out;
        pos = end + close_len;
    }
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// 折成单行、去两端空白,并按 UTF-8 边界截短。
std::string tidy_reason(const std::string& raw) {
    std::string out;
    bool pending_space = false;
    for (const char c : raw) {
        if (is_space(c)) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) out.push_back(' ');
        pending_space = false;
        out.push_back(c);
    }
    if (out.size() <= kMaxReasonBytes) return out;
    std::size_t cut = kMaxReasonBytes;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
    out += "...";
    return out;
}

} // namespace

ParsedVerdict parse_verdict(const std::string& text) {
    const std::string visible =
        strip_block(strip_block(text, "<thinking>", "</thinking>"), "<think>", "</think>");

    bool saw_yes = false;
    bool saw_no = false;
    static constexpr const char* kTag = "<block>";
    constexpr std::size_t kTagLen = 7;
    std::size_t pos = 0;
    while (true) {
        const std::size_t tag = find_ci(visible, kTag, pos);
        if (tag == std::string::npos) break;
        std::size_t value = tag + kTagLen;
        while (value < visible.size() && is_space(visible[value])) ++value;
        const auto word_is = [&](const char* word, std::size_t len) {
            if (value + len > visible.size()) return false;
            for (std::size_t i = 0; i < len; ++i) {
                if (lower_ascii(visible[value + i]) != word[i]) return false;
            }
            // 后面还跟着字母数字的不算(`<block>none`、`<block>yesterday`)。
            return value + len == visible.size() || !is_word_char(visible[value + len]);
        };
        if (word_is("yes", 3)) saw_yes = true;
        else if (word_is("no", 2)) saw_no = true;
        pos = tag + kTagLen;
    }

    ParsedVerdict verdict;
    if (saw_yes == saw_no) return verdict;   // 都没出现,或两种都出现:无法解析。
    if (saw_no) {
        verdict.kind = ParsedVerdict::Kind::Allow;
        return verdict;
    }
    verdict.kind = ParsedVerdict::Kind::Block;
    const std::size_t open = find_ci(visible, "<reason>", 0);
    if (open != std::string::npos) {
        const std::size_t begin = open + 8;
        const std::size_t close = find_ci(visible, "</reason>", begin);
        verdict.reason = tidy_reason(
            visible.substr(begin, close == std::string::npos ? std::string::npos : close - begin));
    }
    return verdict;
}

} // namespace acecode::auto_review
