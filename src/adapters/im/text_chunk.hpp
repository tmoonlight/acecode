#pragma once

// IM 出站文本分段。平台单条消息有长度上限(QQ 按字符,Telegram 按 UTF-16 单位),
// 长回答要切成多条发送。规则:
//   - 每段不超过 max_units;绝不切断 UTF-8 字符;
//   - 优先在段落(空行)处切,其次换行,再次空格,最后才按字符硬切;
//   - 切点落在 ``` 代码块里时,本段末尾补上闭合围栏,下一段开头重开同语言的围栏,
//     保证每段都是完整可渲染的 Markdown。

#include <string>
#include <string_view>
#include <vector>

namespace acecode::im {

// 文本长度:utf16=false 时按 Unicode 码点计,true 时按 UTF-16 单位计(增补平面字符算 2)。
std::size_t text_units(std::string_view utf8, bool utf16);

// 返回的每段都非空;输入为空(或只有空白)时返回空列表。max_units 小于 16 时按 16 处理。
std::vector<std::string> chunk_text(std::string_view utf8, std::size_t max_units, bool utf16);

} // namespace acecode::im
