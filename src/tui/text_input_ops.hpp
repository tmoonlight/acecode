#pragma once

// 文本输入框的 UTF-8 光标 / 插入 / 删除纯函数。与 main.cpp 的 CatchEvent
// 分支里逐字扫描 UTF-8 continuation byte(`(c & 0xC0) == 0x80`)的行为字节级
// 等价,但抽成纯函数后可以在 acecode_testable 单测里直接覆盖,不用跑 FTXUI。
//
// 所有函数都把 `cursor` 当字节偏移处理,越界时会先 clamp 到 text.size(),
// 再执行相应操作;Backspace / Delete 在 empty buffer 或边界上是幂等 no-op,
// 不抛异常。这一组函数保持 mutex-agnostic —— 调用方负责锁的生命周期。

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace acecode {

struct TextSelectionRange {
    std::size_t begin = 0;
    std::size_t end = 0;

    bool operator==(const TextSelectionRange& other) const {
        return begin == other.begin && end == other.end;
    }
};

// Clamp a byte offset to text.size() and, if it points into a UTF-8 glyph,
// retreat to that glyph's first byte.
std::size_t clamp_utf8_boundary(const std::string& text,
                                std::size_t offset);

// Return normalized half-open selection bounds, or nullopt when there is no
// non-empty selection. Both returned offsets are UTF-8 boundaries.
std::optional<TextSelectionRange> text_selection_range(
    const std::string& text,
    std::size_t cursor,
    const std::optional<std::size_t>& anchor);

bool has_text_selection(const std::string& text,
                        std::size_t cursor,
                        const std::optional<std::size_t>& anchor);

// Erase the selected range, put the cursor at its lower bound, and clear the
// anchor. Returns true only when non-empty text was selected.
bool erase_text_selection(std::string& text,
                          std::size_t& cursor,
                          std::optional<std::size_t>& anchor);

// Move to target. With extend=true the original cursor becomes the stable
// anchor; with extend=false any selection is cleared.
void move_cursor_with_selection(const std::string& text,
                                std::size_t& cursor,
                                std::optional<std::size_t>& anchor,
                                std::size_t target,
                                bool extend);

// Conventional unmodified-arrow behavior: collapse an active selection to
// its lower/upper bound without moving an additional glyph.
bool collapse_selection_left(const std::string& text,
                             std::size_t& cursor,
                             std::optional<std::size_t>& anchor);
bool collapse_selection_right(const std::string& text,
                              std::size_t& cursor,
                              std::optional<std::size_t>& anchor);

// Select the complete buffer with the active cursor at its end.
void select_all_text(const std::string& text,
                     std::size_t& cursor,
                     std::optional<std::size_t>& anchor);

// 在 cursor 位置插入 ch 的所有字节,并把 cursor 前进 ch.size() 个字节。
// cursor 越界时先 clamp 到 text.size()。
void insert_at_cursor(std::string& text, std::size_t& cursor, std::string_view ch);

// Selection-aware variants used by editable TUI prompts.
void insert_replacing_selection(std::string& text,
                                std::size_t& cursor,
                                std::optional<std::size_t>& anchor,
                                std::string_view ch);
void backspace_replacing_selection(std::string& text,
                                   std::size_t& cursor,
                                   std::optional<std::size_t>& anchor);
void delete_replacing_selection(std::string& text,
                                std::size_t& cursor,
                                std::optional<std::size_t>& anchor);

// 删除 cursor 位置之前的一个完整 UTF-8 glyph;空串或 cursor==0 时 no-op。
void backspace_utf8(std::string& text, std::size_t& cursor);

// 删除 cursor 位置的下一个完整 UTF-8 glyph;cursor 已在末尾时 no-op。
void delete_utf8(std::string& text, std::size_t& cursor);

// 把 cursor 后退到前一个 UTF-8 glyph 的首字节;cursor==0 时 no-op。
void move_cursor_left_utf8(const std::string& text, std::size_t& cursor);

// 把 cursor 前进到下一个 UTF-8 glyph 的首字节;cursor 已在末尾时 no-op。
void move_cursor_right_utf8(const std::string& text, std::size_t& cursor);

} // namespace acecode
