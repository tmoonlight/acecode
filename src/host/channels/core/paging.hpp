#pragma once

// IM 回复的分页(纯逻辑)。IM 单条消息有长度上限,长列表也难在手机上读:
// 每页同时限制条数与字节数,列表编号跨页连续(/resume 15、@15 直接用列表里的编号),
// 页脚给出翻页命令。

#include <cstddef>
#include <string>
#include <vector>

namespace acecode::channels::core {

constexpr std::size_t kListPageSize = 10;      // 每页条数
constexpr std::size_t kPageMaxBytes = 1800;    // 每页正文的字节上限(不含页脚)
constexpr std::size_t kMaxListItems = 100;     // 一次列表最多保留的条目(10 页)

struct PagedOutput {
    std::string heading;             // 每页第一行;可为空
    std::vector<std::string> pages;  // 每页正文,不含标题与页脚
};

// 列表:rows 是单行文本(不含编号),编号从 1 连续。
PagedOutput paginate_rows(std::string heading, const std::vector<std::string>& rows,
                          std::size_t page_size = kListPageSize, std::size_t max_bytes = kPageMaxBytes);
// 长文本:按行切页,单行超长时在 UTF-8 边界硬切。
PagedOutput paginate_text(std::string heading, const std::string& text, std::size_t max_bytes = kPageMaxBytes);
// 第 page 页(1 起)的完整回复;多于一页时附页脚。页码越界时返回「只有 N 页」的提示。
std::string render_page(const PagedOutput& output, std::size_t page);
// 折成一行:换行与制表符换成空格,超过 max_codepoints 个字符时截断并加「…」。
std::string one_line(const std::string& text, std::size_t max_codepoints);

} // namespace acecode::channels::core
