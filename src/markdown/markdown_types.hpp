#pragma once

#include <string>
#include <vector>
#include <memory>

namespace acecode::markdown {

class MarkdownLinkRegionCollector;

enum class TokenType {
    // Block-level
    Heading,
    Paragraph,
    Code,        // Fenced code block
    Blockquote,
    List,
    ListItem,
    Table,
    Hr,          // Thematic break
    Space,       // Empty line

    // Inline-level
    Text,
    Strong,      // **bold**
    Em,          // *italic*
    CodeSpan,    // `inline code`
    Link,        // [text](url)
    Image,       // ![alt](url)
    Br,          // Line break
    Escape,      // \char
    Html,        // Raw HTML (ignored)
    Del,         // ~strikethrough~ (disabled, ignored)
};

struct Token {
    TokenType type = TokenType::Text;
    std::string raw;          // Original source text
    std::string text;         // Processed text content
    int depth = 0;            // Heading level (1-6)
    std::string lang;         // Code block language
    std::string href;         // Link/Image URL
    bool ordered = false;     // List: ordered flag
    int start = 1;            // Ordered list: start number
    std::vector<Token> children;  // Sub-tokens (recursive)

    // Table-specific data
    std::vector<std::string> align;                          // Column alignments: "left", "center", "right", ""
    std::vector<std::vector<Token>> header_cells;            // header_cells[col] = inline tokens
    std::vector<std::vector<std::vector<Token>>> body_rows;  // body_rows[row][col] = inline tokens
};

struct FormatOptions {
    int terminal_width = 80;
    bool syntax_highlight = true;
    bool hyperlinks = true;
    // 终端支持 OSC 8(由 src/utils/terminal_capability 探测,main.cpp 接线):
    // 渲染 is_link span 时套用 ftxui::hyperlink() 装饰器,让终端原生
    // Cmd/Ctrl+点击、悬停、右键打开/复制生效。与 hyperlinks(应用内点击
    // link_regions 收集)互相独立:两条通道共享链接元数据但代码路径分开。
    bool osc8_hyperlinks = false;
    bool strip_xml = true;
    MarkdownLinkRegionCollector* link_regions = nullptr;
};

// Style attributes for a run of text (used in inline rendering)
struct TextStyle {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool dim = false;
    bool is_code = false;       // Inline code span
    bool is_link = false;       // Link text (bright blue + underline)
    std::string href;           // For links
};

// A run of text with uniform styling
struct StyledRun {
    std::string text;
    TextStyle style;
};

} // namespace acecode::markdown
