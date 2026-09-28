#pragma once

#include <cstddef>

namespace acecode {

// 逐字节跟踪 Markdown 代码围栏(``` / ~~~)与行内代码的轻量状态机。
//
// 从 DsmlToolCallStreamFilter 里原样抽出(行为不变,DSML 测试守护),供文本
// 工具调用恢复复用:围栏里的 `<invoke ...>` 是示例,不能当调用执行。
//
// 用法:在「决定当前字节 c 怎么处理」之前查询状态,处理完再 feed(c)。
// 查询语义都是「c 之前的所有字节已 feed」时的状态。
class MarkdownFenceTracker {
public:
    void feed(char c);
    void reset();

    // 处于已开启的围栏块内部(围栏开启行的下一行起,到闭合行为止)。
    bool in_fence() const { return in_fence_; }
    // 当前行是一条围栏开启行(行首 >=3 个 ` 或 ~),其余内容是 info string。
    bool line_opening_fence() const { return line_opening_fence_; }
    // 当前行到目前为止只出现了最多 3 个前导空格 —— 下一个字节就是「行首」。
    // 第 4 个空格起属于缩进代码块,不再算行首。
    bool at_line_prefix() const { return line_prefix_active_; }
    // 下一个字节处于同一行里成对反引号之间(行内代码)。只在围栏外有意义;
    // 换行即结束(未闭合的反引号不跨行)。
    bool in_inline_code() const;

private:
    void finish_line();
    void reset_line();
    void finish_inline_run();

    bool in_fence_ = false;
    char fence_char_ = '\0';
    std::size_t fence_length_ = 0;
    std::size_t line_leading_spaces_ = 0;
    bool line_prefix_active_ = true;
    bool line_fence_run_active_ = false;
    char line_fence_char_ = '\0';
    std::size_t line_fence_run_ = 0;
    bool line_opening_fence_ = false;
    bool line_nonspace_after_fence_ = false;

    // 行内代码:正在累积的反引号串长度、它是否是行首的围栏候选串,
    // 以及当前打开的行内代码的定界串长度(0 = 未打开)。
    std::size_t inline_run_ = 0;
    bool inline_run_is_line_fence_ = false;
    std::size_t inline_open_length_ = 0;
};

} // namespace acecode
