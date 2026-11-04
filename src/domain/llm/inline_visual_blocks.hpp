#pragma once

// 助手正文里的 visual 代码块(openspec add-inline-visuals,docs/inline-visuals.md)。
//
// Web / Desktop 把顶层的 ```visual 代码块画在对话里的隔离框中;画不了图的地方(IM 消息
// 通道、TUI)用这里的纯函数找出同样的块,换成一行占位提示,不把整段 HTML / SVG 源码
// 发出去。「顶层」的判定与前端 markdown-it 对齐:行首最多 3 个空格的围栏,且不在引用块、
// 列表项里;嵌套的 visual 代码块在 Web 上按普通代码显示,这里也原样保留。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace acecode {

// IM 与 TUI 共用的占位提示。
inline constexpr const char* kInlineVisualPlaceholder =
    u8"[可视化图表] 请在 ACECode 桌面版或网页中查看";

struct InlineVisualBlock {
    std::size_t begin = 0;  // 起始围栏所在行的行首
    std::size_t end = 0;    // 结束围栏所在行的行尾(不含换行);未闭合时为正文末尾
    bool closed = false;
};

// 信息串的第一个词是否为 visual(不区分大小写)。info 是围栏符号之后的整段信息串。
bool is_inline_visual_info(std::string_view info);

// 按出现顺序返回顶层 visual 代码块的位置。其它语言的代码块里出现的 ```visual 字样
// 不算(按 CommonMark 围栏规则跳过整块)。
std::vector<InlineVisualBlock> find_inline_visual_blocks(std::string_view markdown);

// 把每个顶层 visual 代码块(含两端围栏)换成 placeholder;没有时原样返回。
std::string replace_inline_visual_blocks(std::string_view markdown, std::string_view placeholder);

} // namespace acecode
