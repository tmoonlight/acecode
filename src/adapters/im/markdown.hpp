#pragma once

// 助手回复(Markdown)在 IM 上的两种呈现:
//   - markdown_to_plain:去掉标记的纯文本,用于 QQ Markdown 发送被拒时回退;
//   - markdown_to_telegram_html:Telegram 支持的 HTML 子集(b/i/s/code/pre/a/blockquote),
//     其余文本一律转义 & < >。标签只按配对结构输出,保证不会出现未闭合标签;
//     无法识别的标记原样保留为文字。

#include <string>
#include <string_view>

namespace acecode::im {

std::string markdown_to_plain(std::string_view markdown);
std::string markdown_to_telegram_html(std::string_view markdown);

// HTML 文本转义(& < >);attribute=true 时额外转义双引号。
std::string html_escape(std::string_view text, bool attribute = false);

} // namespace acecode::im
