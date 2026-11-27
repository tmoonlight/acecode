#include "so360_parser.hpp"

#include "html_utils.hpp"

#include <utility>

namespace acecode::web_search {
namespace {

using View = std::string_view;
constexpr View kSpace = " \t\r\n\f";

struct Tag {
    View name;
    View attributes;
    std::size_t start;
    bool closing;
};

// Only tag boundaries and a few attributes are needed. Keep script/style
// contents and HTML comments opaque, including any markup quoted inside them.
bool next_tag(View html, std::size_t& pos, Tag& tag) {
    pos = html.find('<', pos);
    if (pos == View::npos) return false;
    const auto start = pos++;
    if (html.substr(start, 4) == "<!--") {
        const auto end = html.find("-->", pos + 3);
        pos = end == View::npos ? html.size() : end + 3;
        tag = {{}, {}, start, false};
        return true;
    }
    const bool closing = pos < html.size() && html[pos] == '/';
    if (closing) ++pos;
    const auto name_end = html.find_first_of(" \t\r\n\f/>", pos);
    if (name_end == View::npos) return false;
    const View name = html.substr(pos, name_end - pos);
    char quote = 0;
    for (pos = name_end; pos < html.size(); ++pos) {
        const char c = html[pos];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            tag = {name, html.substr(name_end, pos - name_end), start, closing};
            ++pos;
            if (!closing && (name == "script" || name == "style")) {
                const auto close = html.find(name == "script" ? "</script>" : "</style>", pos);
                pos = close == View::npos ? html.size() : close + name.size() + 3;
            }
            return true;
        }
    }
    return false;
}

View attr(View attributes, View wanted) {
    std::size_t pos = 0;
    while ((pos = attributes.find_first_not_of(kSpace, pos)) != View::npos) {
        const auto start = pos;
        pos = attributes.find_first_of(" \t\r\n\f=", pos);
        if (pos == View::npos) break;
        const View key = attributes.substr(start, pos - start);
        pos = attributes.find_first_not_of(kSpace, pos);
        if (pos == View::npos || attributes[pos] != '=') continue;
        pos = attributes.find_first_not_of(kSpace, pos + 1);
        if (pos == View::npos) break;
        const char quote = attributes[pos];
        const bool quoted = quote == '"' || quote == '\'';
        if (quoted) ++pos;
        const auto end = quoted ? attributes.find(quote, pos)
                                : attributes.find_first_of(kSpace, pos);
        if (key == wanted) return attributes.substr(pos, end - pos);
        pos = end == View::npos ? attributes.size() : end + quoted;
    }
    return {};
}

bool has_class(const Tag& tag, View wanted) {
    auto classes = attr(tag.attributes, "class");
    std::size_t pos = 0;
    while ((pos = classes.find_first_not_of(kSpace, pos)) != View::npos) {
        const auto end = classes.find_first_of(kSpace, pos);
        if (classes.substr(pos, end - pos) == wanted) return true;
        pos = end;
    }
    return false;
}

struct Element {
    View attributes;
    View content;
};

bool find_element(View html, std::size_t& pos, View name, View class_name, Element& out) {
    Tag tag{};
    while (next_tag(html, pos, tag)) {
        if (tag.closing || tag.name != name ||
            (!class_name.empty() && !has_class(tag, class_name))) continue;
        out.attributes = tag.attributes;
        const auto start = pos;
        int depth = 1;
        while (next_tag(html, pos, tag)) {
            if (tag.name != name) continue;
            depth += tag.closing ? -1 : 1;
            if (depth == 0) {
                out.content = html.substr(start, tag.start - start);
                return true;
            }
        }
    }
    return false;
}

Element first_element(View html, View name, View class_name = {}) {
    std::size_t pos = 0;
    Element element{};
    find_element(html, pos, name, class_name, element);
    return element;
}

std::string plain_text(View html) {
    std::string text;
    std::size_t pos = 0;
    auto start = pos;
    Tag tag{};
    while (next_tag(html, pos, tag)) {
        text.append(html.substr(start, tag.start - start));
        start = pos;
    }
    text.append(html.substr(start));
    return collapse_whitespace(html_decode_entities(text));
}

} // namespace

void visit_so360_results(View html, const std::function<bool(SearchHit)>& visit) {
    std::size_t pos = 0;
    Element item{};
    while (find_element(html, pos, "li", "res-list", item)) {
        if (!first_element(item.content, "div", "g-mohe").attributes.empty()) continue;
        const auto heading = first_element(item.content, "h3");
        const auto anchor = first_element(heading.content, "a");
        auto url = attr(anchor.attributes, "data-mdurl");
        if (url.empty()) url = attr(anchor.attributes, "href");
        if (url.empty()) continue;
        auto description = first_element(item.content, "p", "res-desc");
        if (description.content.empty()) {
            description = first_element(item.content, "div", "res-comm-con");
        }
        SearchHit hit;
        hit.title = plain_text(anchor.content);
        hit.url = html_decode_entities(url);
        hit.snippet = plain_text(description.content);
        hit.backend_name = "so360";
        if (!visit(std::move(hit))) break;
    }
}

} // namespace acecode::web_search
