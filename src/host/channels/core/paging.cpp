#include "paging.hpp"

#include "remote_control/rc_session_navigation.hpp"

namespace acecode::channels::core {
namespace {

bool is_continuation(unsigned char byte) { return (byte & 0xC0) == 0x80; }

void push_page(PagedOutput& output, std::string& body) {
    if (body.empty()) return;
    output.pages.push_back(std::move(body));
    body.clear();
}

} // namespace

std::string one_line(const std::string& text, std::size_t max_codepoints) {
    std::string out;
    out.reserve(text.size());
    std::size_t codepoints = 0;
    bool space = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte == '\r' || byte == '\n' || byte == '\t' || byte == ' ') {
            space = !out.empty();
            continue;
        }
        if (!is_continuation(byte)) {
            if (space) {
                if (codepoints == max_codepoints) return out + "…";
                out.push_back(' ');
                ++codepoints;
                space = false;
            }
            if (codepoints == max_codepoints) return out + "…";
            ++codepoints;
        }
        out.push_back(text[i]);
    }
    return out;
}

PagedOutput paginate_rows(std::string heading, const std::vector<std::string>& rows, std::size_t page_size,
                          std::size_t max_bytes) {
    PagedOutput output;
    output.heading = std::move(heading);
    std::string body;
    std::size_t in_page = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto line = std::to_string(i + 1) + ". " + rows[i];
        if (in_page > 0 && (in_page >= page_size || body.size() + 1 + line.size() > max_bytes)) {
            push_page(output, body);
            in_page = 0;
        }
        if (!body.empty()) body += "\n";
        body += line;
        ++in_page;
    }
    push_page(output, body);
    return output;
}

PagedOutput paginate_text(std::string heading, const std::string& text, std::size_t max_bytes) {
    PagedOutput output;
    output.heading = std::move(heading);
    for (auto& chunk : rc::chunk_rc_session_output(text, max_bytes)) {
        // 切点落在换行上时,下一页开头会带着那个换行。
        while (!chunk.empty() && (chunk.front() == '\n' || chunk.front() == '\r')) chunk.erase(chunk.begin());
        push_page(output, chunk);
    }
    return output;
}

std::string render_page(const PagedOutput& output, std::size_t page) {
    const auto count = output.pages.size();
    if (count == 0) return output.heading;
    if (page == 0 || page > count) return "只有 " + std::to_string(count) + " 页。";
    std::string text = output.heading.empty() ? std::string{} : output.heading + "\n";
    text += output.pages[page - 1];
    if (count > 1) {
        text += "\n—— 第 " + std::to_string(page) + "/" + std::to_string(count) + " 页";
        text += page < count ? ",发送 /mr 看下一页" : "(最后一页)";
    }
    return text;
}

} // namespace acecode::channels::core
