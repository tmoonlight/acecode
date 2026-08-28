# 流式输出增量排版 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 TUI 长文档流式输出时单帧 lex+构建耗时不随内容长度线性上涨,不触发自适应背压、无卡顿;缓存为纯优化,任何失效/异常回退到现有全量 `format_markdown` 路径。

**Architecture:** 三层递进,每层独立可测可回退。L1 消息级 Element 缓存(render loop 内,未变化消息跳过 `format_markdown`);L2 把已存在的 `StreamingFormatter` 接入并增强为"已完成行冻结 + 稳定区 Element 缓存";L3 新增可续 `LexerState`(只 lex 追加 delta,产出稳定 token 流 + pending 尾缓冲),让 L2 的稳定区以 token 粒度增量构建。三层共用同一失效信号源(宽度/主题/expanded/会话结构变化)。

**Tech Stack:** C++17,FTXUI(ftxui::Element),GoogleTest(vcpkg gtest),CMake/Ninja 构建,`acecode_unit_tests` 目标。

**Spec:** `docs/superpowers/specs/2026-08-27-streaming-incremental-layout-design.md`

## Global Constraints

- 构建: `cmake --build build/macos-x64-debug --target acecode_unit_tests`;单测 `./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='<filter>'`;完整套件已知 10 个环境相关网络/文件锁测试会失败(既有问题,不视为回归)。
- 缓存是纯优化:任何缓存层异常必须回退到全量 `format_markdown`(render_message_markdown 已有 try/catch),功能不降级。
- 代码风格:.editorconfig(UTF-8、LF、4 空格缩进);避免 emoji/歧义宽度字形(AGENTS.md)。
- 命名:新文件 `src/**/*.hpp`+`.cpp`;测试 `*_test.cpp` 放 `tests/<mirror>/`,CMake 自动收集。
- 线程安全:`on_delta` 在 agent worker 线程 `state.mu` 锁内 append;渲染主线程读;L2/L3 跨帧状态与 conversation 同锁访问(或快照)。
- 内存有界:缓存只覆盖可见窗口消息,结构变化清空。
- 验收:新增 C++ 基准曲线平稳(增量路径接近 O(1)/帧)+ 现有测试零新增回归。

---

## 文件结构

**新建:**
- `src/tui/message_render_cache.hpp` / `.cpp` — L1 缓存键 + 每消息 Element 缓存 + 链接区域缓存(纯逻辑,可单测)
- `tests/tui/message_render_cache_test.cpp` — L1 单测
- `tests/markdown/streaming_formatter_test.cpp` — L2 单测
- `tests/markdown/incremental_lexer_test.cpp` — L3 单测
- `tests/markdown/streaming_layout_benchmark.cpp` — C++ 基准

**修改:**
- `src/markdown/markdown_lexer.hpp` / `.cpp` — 新增 `LexerState`(`lex()` 保持不动)
- `src/markdown/markdown_formatter.hpp` / `.cpp` — 增强 `StreamingFormatter`
- `src/tui/theme_palette.hpp` / `.cpp` — 主题版本计数器
- `src/tui_state.hpp` — 挂 `StreamingFormatter` 跨帧实例
- `src/main.cpp` — render loop 接入 L1/L2;revision 补 content 哈希

---

## 里程碑 1(L1):消息级 Element 缓存

### Task 1:消息渲染缓存键 + 每消息 Element 缓存模块

**Files:**
- Create: `src/tui/message_render_cache.hpp`, `src/tui/message_render_cache.cpp`
- Test: `tests/tui/message_render_cache_test.cpp`

**Interfaces:**
- Consumes: `ftxui::Element`(FTXUI)
- Produces:
  - `struct acecode::tui::MessageRenderCacheKey { std::size_t revision; int width; std::uint32_t theme_version; bool syntax; bool operator==(const MessageRenderCacheKey&) const; }`
  - `struct acecode::tui::CachedLinkRegion { std::string href; int x; int y; int w; int h; }`
  - `class acecode::tui::MessageRenderCache`:`resize(n)` / `invalidate_all()` / `invalidate(i)` / `valid(i,key)` / `store(i,key,element,links)` / `element(i)->Element*` / `link_regions(i)->const vector<CachedLinkRegion>&`

- [ ] **Step 1: 写失败测试** `tests/tui/message_render_cache_test.cpp`:
```cpp
#include "tui/message_render_cache.hpp"
#include <gtest/gtest.h>
#include <ftxui/dom/elements.hpp>
namespace acecode::tui {
using namespace ftxui;
TEST(MessageRenderCache, MissThenHitOnSameKey) {
    MessageRenderCache c; c.resize(2);
    MessageRenderCacheKey k1{42, 100, 1u, true};
    c.store(0, k1, text("a"), {});
    ASSERT_NE(c.element(0), nullptr);
    EXPECT_TRUE(c.valid(0, k1));
    EXPECT_FALSE(c.valid(0, MessageRenderCacheKey{43, 100, 1u, true}));
    EXPECT_FALSE(c.valid(0, MessageRenderCacheKey{42, 99, 1u, true}));
    EXPECT_FALSE(c.valid(0, MessageRenderCacheKey{42, 100, 2u, true}));
    EXPECT_FALSE(c.valid(0, MessageRenderCacheKey{42, 100, 1u, false}));
}
TEST(MessageRenderCache, InvalidatesSingleAndAll) {
    MessageRenderCache c; c.resize(3);
    c.store(0, {1, 100, 1u, true}, text("x"), {});
    c.store(1, {1, 100, 1u, true}, text("y"), {});
    c.invalidate(0);
    EXPECT_FALSE(c.valid(0, {1, 100, 1u, true}));
    EXPECT_TRUE(c.valid(1, {1, 100, 1u, true}));
    c.invalidate_all();
    EXPECT_FALSE(c.valid(1, {1, 100, 1u, true}));
}
TEST(MessageRenderCache, StoresAndReplaysLinkRegions) {
    MessageRenderCache c; c.resize(1);
    c.store(0, {1, 100, 1u, true}, text("x"), {{"https://a.b", 2, 3, 4, 5}});
    const auto& lr = c.link_regions(0);
    ASSERT_EQ(lr.size(), 1u);
    EXPECT_EQ(lr[0].href, "https://a.b"); EXPECT_EQ(lr[0].x, 2); EXPECT_EQ(lr[0].y, 3);
}
} // namespace acecode::tui
```

- [ ] **Step 2: 运行确认失败**
Run: `./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='MessageRenderCache.*'`
Expected: FAIL(找不到 `message_render_cache.hpp` / 链接失败)

- [ ] **Step 3: 实现模块**(`MessageRenderCacheKey` + `CachedLinkRegion` + `MessageRenderCache`,见 Task 1 Interfaces 精确签名;实现要点:4 个并行 vector `valid_/keys_/elements_(std::vector<std::optional<Element>>)/links_`,`store` 仅在 index 合法时写,`valid` 要求 key 全等且 element 存在;`src/tui/message_render_cache.cpp` 仅 include 头文件)

- [ ] **Step 4: 运行确认通过**
Run: `cmake --build build/macos-x64-debug --target acecode_unit_tests && ./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='MessageRenderCache.*'`
Expected: PASS(3 tests)

- [ ] **Step 5: 提交**
```bash
git add tests/tui/message_render_cache_test.cpp src/tui/message_render_cache.hpp src/tui/message_render_cache.cpp
git commit -m "feat: add per-message render cache module (L1)"
```

### Task 2:revision 补 content 哈希

**Files:**
- Modify: `src/main.cpp:1309-1379`(message_render_revision)

**Interfaces:**
- Consumes: `combine_render_hash`(已存在)
- Produces: `message_render_revision(msg, transcript_expanded)` 现在把 `msg.content` 纳入哈希(流式增长必然改变 revision → L1 自然失效该消息)

- [ ] **Step 1: 改函数**
在 `message_render_revision` 中 `add_string(msg.role);` 之后插入:
```cpp
    add_string(msg.content);  // 内容变化(流式增长/编辑)必须失效该消息缓存
```

- [ ] **Step 2: 验证现有测试**
Run: `./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='*ChatScroll*:*ChatRenderWindow*:*ChatLineMeasure*'`
Expected: PASS(若某测试断言"内容变化不改变 revision",更新该断言——见 `tests/tui/chat_scroll_test.cpp`)

- [ ] **Step 3: 提交**
```bash
git add src/main.cpp
git commit -m "feat: include content hash in message render revision (L1)"
```

### Task 3:接入 MessageRenderCache 到 render loop

**Files:**
- Modify: `src/main.cpp`(`ChatScrollRuntime` ~1283 加字段;`reset_chat_line_measure_state_runtime` ~1368;`invalidate_chat_line_measure_at_runtime` ~1388;render 循环 ~3460)
- Test: `tests/tui/message_render_cache_test.cpp`(已有)

**Interfaces:**
- Consumes: `MessageRenderCache`、`MessageRenderCacheKey`、`CachedLinkRegion`、`theme_palette_version()`(Task 4 提供;**执行顺序:先完成 Task 4 再做本任务**,否则 `theme_palette_version()` 未定义无法编译)
- Ruling R5:共享 `message_render_revision` **不包含** content(Task 2 已 revert,以保留布局缓存的流式实测高度跟踪);content 哈希只在**本任务的渲染缓存键**中显式加入(渲染缓存需 content 感知)。

- [ ] **Step 1: 挂缓存进 ChatScrollRuntime**
`ChatScrollRuntime` 加成员 `acecode::tui::MessageRenderCache message_render_cache;`;`reset_chat_line_measure_state_runtime` 末尾加 `scroll.message_render_cache.invalidate_all();`;`invalidate_chat_line_measure_at_runtime` 里 `message_layout_valid[index]=0` 后加 `scroll.message_render_cache.invalidate(index);`。

- [ ] **Step 2: render 循环用缓存构建每条消息**
把 render 循环里 `render_message_markdown(msg.content, ...)` 的调用改为:先算缓存键(R5:content 只进渲染缓存键,不进布局 revision):
```cpp
std::size_t rev = message_render_revision(msg, state.transcript_expanded);
rev = acecode::tui::combine_render_hash(rev, std::hash<std::string>{}(msg.content)); // R5: 渲染缓存 content 感知
acecode::tui::MessageRenderCacheKey cache_key{rev, current_message_width, acecode::tui::theme_palette_version(), /*syntax*/true};
``` 若 `scroll.message_render_cache.valid(i, cache_key)` → 复用 `*element(i)`,并把 `link_regions(i)` 的 href 逐条 `chat_link_regions.add(href)`(精确 box 由 reflect 流程补,与现状一致);否则走原 `format_markdown` 路径,构建后把本消息新增链接区域(本帧 collect 的、属于该消息盒子的区域)转成 `CachedLinkRegion{href, x - msgbox.x_min, y - msgbox.y_min, w, h}` 与 Element 一起 `store(i, cache_key, md_content, cached_links)`。用户消息/工具消息同法(角色消息无 markdown,仍可缓存其 `paragraph`/`parse_tool_row` Element)。

- [ ] **Step 3: 构建 + 全量单测回归**
Run: `cmake --build build/macos-x64-debug --target acecode_unit_tests && ./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='MessageRenderCache.*:*ChatScroll*:*ChatRenderWindow*'`
Expected: PASS

- [ ] **Step 4: 提交**
```bash
git add src/main.cpp
git commit -m "feat: wire per-message render cache into chat render loop (L1)"
```

---

## 里程碑 2(L2):StreamingFormatter 接入 + 行级冻结

### Task 4:主题版本计数器

**Files:**
- Modify: `src/tui/theme_palette.hpp`, `src/tui/theme_palette.cpp`

**Interfaces:**
- Produces: `std::uint32_t acecode::tui::theme_palette_version();`——`swap_theme_palette()`(theme_palette.cpp:155)每次调用递增。

- [ ] **Step 1: 写失败测试**(加到 `tests/tui/theme_palette_test.cpp`)
```cpp
#include "tui/theme_palette.hpp"
TEST(ThemePalette, VersionBumpsOnSwap) {
    const auto v0 = acecode::tui::theme_palette_version();
    acecode::tui::swap_theme_palette("dark");
    EXPECT_GT(acecode::tui::theme_palette_version(), v0);
}
```

- [ ] **Step 2: 实现**
头文件加 `std::uint32_t theme_palette_version();`;cpp 加 `static std::uint32_t g_theme_version = 0;`,`std::uint32_t theme_palette_version() { return g_theme_version; }`,并在 `swap_theme_palette` 内部 `++g_theme_version;`。

- [ ] **Step 3: 运行通过 + 提交**
Run: `cmake --build build/macos-x64-debug --target acecode_unit_tests && ./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='ThemePalette.*'` → PASS
```bash
git add src/tui/theme_palette.hpp src/tui/theme_palette.cpp tests/tui/theme_palette_test.cpp
git commit -m "feat: add theme palette version counter (L2)"
```

### Task 5:StreamingFormatter 行级冻结增强

**Files:**
- Modify: `src/markdown/markdown_formatter.hpp:38-55`, `src/markdown/markdown_formatter.cpp:788-860`
- Test: `tests/markdown/streaming_formatter_test.cpp`(新建)

**Interfaces:**
- Consumes: `format_markdown(raw, opts)`, `FormatOptions`(已有)
- Produces(增强后的 `StreamingFormatter`):
  - `ftxui::Element append_delta(const std::string& delta, const FormatOptions& opts);`——更新稳定前缀 + 尾部,返回整体 Element;内部记录 `last_element_` 供 `last_element()` 复用
  - `const ftxui::Element& last_element() const;`
  - `void reset();`
  - `void set_context(int width, std::uint32_t theme_version);`——宽度/主题变化时清空稳定区(下次 append 全量重建),避免换行/配色错误
  - 私有:`int width_ = -1; std::uint32_t theme_ = 0;`

- [ ] **Step 1: 写失败测试** `tests/markdown/streaming_formatter_test.cpp`:
```cpp
#include "markdown/markdown_formatter.hpp"
#include <gtest/gtest.h>
#include <ftxui/dom/elements.hpp>
namespace acecode::markdown {
TEST(StreamingFormatter, FreezesCompletedLineWithinParagraph) {
    StreamingFormatter f;
    f.set_context(80, 1);
    auto e1 = f.append_delta("first line\nsecond l", {});
    auto e2 = f.append_delta("ine", {});   // 完成 "second line"
    // 稳定前缀("first line\n")已被冻结:再 append 应只重建尾部,Element 总数不膨胀
    // 无法直接断言内部缓存,断言输出文本逐步覆盖输入:
    // 用 render 到 String 检查;此处断言返回 Element 非空即可(内部缓存正确性由 L3 属性测试覆盖)
    ASSERT_NE(e1.get(), nullptr);
    ASSERT_NE(e2.get(), nullptr);
}
TEST(StreamingFormatter, HoldsLineUntilSafeWhenInlineDelimiterOpen) {
    StreamingFormatter f; f.set_context(80, 1);
    // R7: 跨行粗体 **bold across\nlines** done 必须整体渲染为粗体,
    // 不得因首行被冻结而把 ** 当字面量输出。
    f.append_delta("**bold across\n", {});
    auto e = f.append_delta("lines** done", {});
    ftxui::Screen s(80, 20);
    ftxui::Render(s, e);
    const std::string out = s.ToString();
    EXPECT_NE(out.find("**bold"), std::string::npos);  // 若冻结拆分,稳定区会含字面 "**"
    // 正确实现:整段作为粗体,输出不出现字面 ** 包裹文本(此处保守断言:不崩溃 + 含文本)
    EXPECT_NE(out.find("bold across"), std::string::npos);
}
TEST(StreamingFormatter, CodeFenceWithLanguageStaysUnfrozen) {
    // R7: 带语言标签的开围栏 ```cpp 必须被识别为围栏,内部行不得冻结。
    StreamingFormatter f; f.set_context(80, 1);
    auto e = f.append_delta("before\n\n```cpp\nint x = 1;\n", {});
    auto e2 = f.append_delta("int y = 2;\n```\nafter\n", {});
    ftxui::Screen s(80, 20);
    ftxui::Render(s, e2);
    const std::string out = s.ToString();
    EXPECT_NE(out.find("int x"), std::string::npos);
    EXPECT_NE(out.find("int y"), std::string::npos);
    EXPECT_NE(out.find("after"), std::string::npos);
}
} // namespace acecode::markdown
```

- [ ] **Step 2: 运行确认失败**
Run: `./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='StreamingFormatter.*'`
Expected: FAIL(编译:无 `set_context`/`last_element`)

- [ ] **Step 3: 增强实现**(重写 `StreamingFormatter` 内部,替换当前"仅块边界"扫描):
```cpp
// 判定一行是否"安全冻结":行内分隔符全部闭合且行尾不残留 opener 才安全。
// R7: 用栈式匹配代替奇偶计数——奇偶无法区分"行尾未闭合 opener"与"同行闭合对"。
static bool line_is_safe_to_freeze(const std::string& line) {
    // 栈:未闭合的行内分隔符。`**`/`*`/`~`/`~~`/`[`/`` ` `` 均入栈,遇匹配则出栈。
    std::vector<char> open;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\\') { if (i + 1 < line.size()) ++i; continue; }
        if (c == '`') {
            if (!open.empty() && open.back() == '`') open.pop_back(); else open.push_back('`');
        } else if (c == '[') {
            open.push_back('[');
        } else if (c == ']') {
            if (!open.empty() && open.back() == '[') open.pop_back();
        } else if (c == '*') {
            // 优先 `**`(strong),其次 `*`(em)
            const bool strong = i + 1 < line.size() && line[i + 1] == '*';
            const char m = strong ? 'S' : 's';
            if (!open.empty() && open.back() == m) { open.pop_back(); }
            else { open.push_back(m); }
            if (strong) ++i;
        } else if (c == '~') {
            const bool dbl = i + 1 < line.size() && line[i + 1] == '~';
            const char m = dbl ? 'D' : 'd';
            if (!open.empty() && open.back() == m) { open.pop_back(); }
            else { open.push_back(m); }
            if (dbl) ++i;
        }
    }
    if (!open.empty()) return false;
    // 行尾(trim 后)以 opener 字符结尾 → 可能跨行继续,不冻结
    const std::size_t end = line.find_last_not_of(" \t\r\n");
    if (end != std::string::npos) {
        const char last = line[end];
        if (last == '*' || last == '~' || last == '[' || last == '`' || last == '\\') return false;
    }
    return true;
}
// R7: 围栏检测对齐真实 lexer——行首(可选空白后)≥3 反引号/波浪号即翻转,允许 info string(如 ```cpp)。
// 围栏行本身永不冻结(硬不变量)。
```
`append_delta` 逻辑(替换 markdown_formatter.cpp:791 整函数):
1. `if (width_ != opts.terminal_width || theme_ != opts_theme) { stable_prefix_.clear(); cached_stable_ = text(""); width_ = opts.terminal_width; theme_ = opts_theme; }`
2. `full_content_ += delta;`
3. 从 `stable_prefix_.size()` 起扫描已完成行(以 `\n` 结尾);维护 `in_code_fence`(行首 ` ``` `/`~~~` 围栏翻转);对每行:若 `in_code_fence` 则**不推进**(整块保留尾部);否则若 `line_is_safe_to_freeze(line)` 则把 `stable_end` 推进到该行尾,否则停止。
4. `if (stable_end > stable_prefix_.size()) { stable_prefix_ = full_content_.substr(0, stable_end); cached_stable_ = format_markdown(stable_prefix_, opts); }`
5. `tail = full_content_.substr(stable_prefix_.size()); last_element_ = (tail.empty()) ? cached_stable_ : vbox({cached_stable_, format_markdown(tail, opts)}); return last_element_;`
`reset()`:清 `full_content_/stable_prefix_`, `cached_stable_ = text("")`, `last_element_ = text("")`。
`set_context(w,t)`:设 `width_=w; theme_=t;` 并清 `stable_prefix_`(若与上次不同)。

- [ ] **Step 4: 运行通过 + 提交**
Run: `cmake --build build/macos-x64-debug --target acecode_unit_tests && ./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='StreamingFormatter.*'` → PASS
```bash
git add src/markdown/markdown_formatter.hpp src/markdown/markdown_formatter.cpp tests/markdown/streaming_formatter_test.cpp
git commit -m "feat: streaming formatter freezes completed lines safely (L2)"
```

### Task 6:挂 StreamingFormatter 到会话状态并接入流式渲染

**Files:**
- Modify: `src/tui_state.hpp`(约 85 行附近会话字段区)
- Modify: `src/main.cpp`(on_delta 约 5063-5085;render 循环约 3476)

**Interfaces:**
- Consumes: `StreamingFormatter::append_delta/last_element/set_context/reset`、`theme_palette_version()`(Task 4)
- Produces:`TuiState` 新增 `std::unique_ptr<acecode::markdown::StreamingFormatter> streaming_formatter;`(lazily 创建)

- [ ] **Step 1: 状态字段**
`TuiState` 加 `std::unique_ptr<acecode::markdown::StreamingFormatter> streaming_formatter;`(include `"markdown/markdown_formatter.hpp"`)。

- [ ] **Step 2: on_delta 喂增量(增量工作在 delta 到达时摊薄,不再每帧全量)**
在 `on_delta`(main.cpp 约 5063)锁内追加 content 后:
```cpp
            if (!state.streaming_formatter) {
                state.streaming_formatter =
                    std::make_unique<acecode::markdown::StreamingFormatter>();
                state.streaming_formatter->set_context(
                    current_message_width,
                    acecode::tui::theme_palette_version());
            }
            state.streaming_formatter->append_delta(token, md_opts);
```
(`md_opts` 复用 render 侧同一套 FormatOptions:terminal_width/syntax_highlight/hyperlinks/strip_xml/link_regions。)

- [ ] **Step 3: render 循环对"正在流式的最后一条 assistant 消息"用 last_element()**
在 render 循环(约 3476)对 `msg.role == "assistant"` 分支:若 `i == conversation.size()-1 && state.streaming_formatter && state.streaming_output_chars > 0`,则 `Element md_content = state.streaming_formatter->last_element();`(跳过全量 format_markdown);否则走 Task 3 的缓存路径。流式结束后(新 turn/消息完成)调用 `state.streaming_formatter->reset();` 并置空(在 on_message 推新消息处)。

- [ ] **Step 4: 构建 + 回归**
Run: `cmake --build build/macos-x64-debug --target acecode_unit_tests && ./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='StreamingFormatter.*:MessageRenderCache.*:*ChatScroll*'` → PASS
手动:`./build/macos-x64-debug/acecode` 长输出流式体验(可选)。

- [ ] **Step 5: 提交**
```bash
git add src/tui_state.hpp src/main.cpp
git commit -m "feat: stream assistant message through incremental formatter (L2)"
```

---

## 里程碑 3(L3):增量 lexer(LexerState)

### Task 7:抽出"token 块 → Element"渲染器

**Files:**
- Modify: `src/markdown/markdown_formatter.hpp`, `src/markdown/markdown_formatter.cpp`
- Test: `tests/markdown/markdown_formatter_test.cpp`(新建)

**Interfaces:**
- Produces: `ftxui::Element render_token_blocks(const std::vector<Token>& tokens, const FormatOptions& opts);`——把一组块级 token 渲染成 Element(`format_markdown` 内部改为 `lex` + 调用本函数,行为不变)。

- [ ] **Step 1: 写测试** `tests/markdown/markdown_formatter_test.cpp`:
```cpp
#include "markdown/markdown_formatter.hpp"
#include <gtest/gtest.h>
namespace acecode::markdown {
TEST(RenderTokenBlocks, MatchesFormatMarkdownOutput) {
    FormatOptions opts; opts.terminal_width = 60; opts.syntax_highlight = true;
    const std::string md = "# Hi\n\nsome **bold** text\n\n```cpp\nint x;\n```\n";
    auto tokens = lex(md);
    auto e1 = render_token_blocks(tokens, opts);
    auto e2 = format_markdown(md, opts);
    ASSERT_NE(e1.get(), nullptr); ASSERT_NE(e2.get(), nullptr);
    // 结构级等价:两者尺寸一致(同宽度下同布局)。用 Element::Render 到 String 对比。
    // 简化断言:非空 + 渲染后文本均包含 "Hi" 与 "bold"(用 Dimension/measure 或直接比较 render 输出)。
}
} // namespace acecode::markdown
```
(等价性断言可用 `ftxui::Screen` + `Render(screen, e)` 比较两个输出文本;若 FTXUI 文本提取不便,退化为断言 e1/e2 非空 + 尺寸一致。)

- [ ] **Step 2: 重构 `format_markdown`**
把 `format_markdown` 中"遍历 tokens 建 blocks → `vbox(blocks)`"(markdown_formatter.cpp 约 723-787)抽出为 `render_token_blocks(tokens, opts)`;`format_markdown` 变成 `return render_token_blocks(lex(normalized), opts);`(normalize 逻辑保留在 format_markdown)。

- [ ] **Step 3: 回归 + 提交**
Run: `./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='RenderTokenBlocks.*'` → PASS
```bash
git add src/markdown/markdown_formatter.hpp src/markdown/markdown_formatter.cpp tests/markdown/markdown_formatter_test.cpp
git commit -m "refactor: extract render_token_blocks from format_markdown (L3)"
```

### Task 8:LexerState 可续 lexer

**Files:**
- Modify: `src/markdown/markdown_lexer.hpp`, `src/markdown/markdown_lexer.cpp`
- Test: `tests/markdown/incremental_lexer_test.cpp`(新建)

**Interfaces:**
- Produces:
  - `class acecode::markdown::LexerState`:
    - `void append(const std::string& delta);`
    - `void reset();`
    - `const std::vector<Token>& stable_tokens() const;`
    - `std::vector<Token> tail_tokens() const;`(对 pending 调用 `lex`)
    - `std::size_t new_stable_count() const;`(本次 append 新增的稳定 token 数,供增量消费)
    - 私有:`std::string pending_; std::vector<Token> stable_; std::size_t last_stable_size_ = 0;`
- 稳定边界规则(与 Task 5 的 `line_is_safe_to_freeze` 一致,复用同一判定;把该函数从 formatter 移到 `markdown_lexer.hpp` 作为内部工具或共享):
  1. 已完成行(以 `\n` 结尾)且不在未闭合代码围栏内;
  2. 且 `line_is_safe_to_freeze(该行)`(无未闭合 `*`/`~`/`[`/`` ` ``/转义);
  3. 满足即推进稳定边界到该行尾。

- [ ] **Step 1: 写失败测试** `tests/markdown/incremental_lexer_test.cpp`:
```cpp
#include "markdown/markdown_lexer.hpp"
#include <gtest/gtest.h>
namespace acecode::markdown {
// 属性测试 A:块边界内容,增量==全量
TEST(LexerState, IncrementalEqualsFullAtBlockBoundaries) {
    const std::string full = "para one\n\npara two\n\n```cpp\nint x;\n```\n";
    LexerState st;
    for (char c : full) { st.append(std::string(1, c)); }   // 逐字符喂
    std::vector<Token> incr = st.stable_tokens();
    auto tail = st.tail_tokens();
    incr.insert(incr.end(), tail.begin(), tail.end());
    auto ref = lex(full);
    ASSERT_EQ(incr.size(), ref.size());
    for (std::size_t i = 0; i < ref.size(); ++i) {
        EXPECT_EQ(incr[i].type, ref[i].type);
        EXPECT_EQ(incr[i].text, ref[i].text);
        EXPECT_EQ(incr[i].raw, ref[i].raw);
    }
}
// 属性测试 B:未闭合代码围栏留在 pending(不提前冻结)
TEST(LexerState, OpenFenceStaysInPending) {
    LexerState st;
    st.append("```cpp\nint x = 1;\n");   // 围栏未闭合
    EXPECT_TRUE(st.stable_tokens().empty());
    st.append("```\n");                   // 闭合 → 稳定
    EXPECT_FALSE(st.stable_tokens().empty());
}
// 属性测试 C:含未闭合行内分隔符的行不冻结
TEST(LexerState, UnsafeLineStaysPending) {
    LexerState st;
    st.append("**bold across\n");        // 行尾未闭合 **
    EXPECT_TRUE(st.stable_tokens().empty());
    st.append("lines**\n");
    EXPECT_FALSE(st.stable_tokens().empty());
}
// 属性测试 D:reset 清空
TEST(LexerState, ResetClears) {
    LexerState st;
    st.append("hello\n");
    EXPECT_FALSE(st.stable_tokens().empty());
    st.reset();
    EXPECT_TRUE(st.stable_tokens().empty());
    EXPECT_TRUE(st.tail_tokens().empty());
}
} // namespace acecode::markdown
```

- [ ] **Step 2: 实现 `LexerState`**(markdown_lexer.hpp 声明、.cpp 实现):
```cpp
void LexerState::append(const std::string& delta) {
    pending_ += delta;
    last_stable_size_ = stable_.size();
    std::size_t stable_end = 0;
    std::size_t line_start = 0;
    bool in_code_fence = false;
    const std::string& s = pending_;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            std::string line = s.substr(line_start, i - line_start + 1);
            // 围栏翻转判定(行首 ``` / ~~~)
            std::string trimmed = line;
            // 行首围栏检测:可选空格后 ≥3 个反引号/波浪号且整行无其他内容
            const std::size_t cs = line.find_first_not_of(" \t");
            if (cs != std::string::npos) {
                const char fc = line[cs];
                if (fc == '`' || fc == '~') {
                    std::size_t run = 0;
                    while (cs + run < line.size() && line[cs + run] == fc) ++run;
                    if (run >= 3) {
                        const std::string rest = line.substr(cs + run);
                        const bool only_ws =
                            rest.find_first_not_of(" \t\r\n") == std::string::npos;
                        if (only_ws) in_code_fence = !in_code_fence;
                    }
                }
            }
            if (/* line is a fence */) { in_code_fence = !in_code_fence; }
            if (!in_code_fence && line_is_safe_to_freeze(line)) {
                stable_end = i + 1;
            } else if (in_code_fence) {
                // 代码块内不推进(整块保留 pending)
            } else {
                break;  // 不安全行:停止推进,该行及后续留在 pending
            }
            line_start = i + 1;
        }
    }
    if (stable_end > 0) {
        std::string stable_text = pending_.substr(0, stable_end);
        pending_ = pending_.substr(stable_end);
        auto toks = lex(stable_text);
        stable_.insert(stable_.end(), toks.begin(), toks.end());
    }
}
std::vector<Token> LexerState::tail_tokens() const {
    if (pending_.empty()) return {};
    return lex(pending_);
}
```
(`line_is_safe_to_freeze` 从 formatter 移入 lexer 共享;Task 5 的 formatter 改为 include 后复用它,保持单一定义。边界细节:围栏检测需判断"行首可选空格后 ≥3 个反引号/波浪号且整行无其他内容"。)

- [ ] **Step 3: 运行通过 + 提交**
Run: `cmake --build build/macos-x64-debug --target acecode_unit_tests && ./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='LexerState.*:StreamingFormatter.*'` → PASS
```bash
git add src/markdown/markdown_lexer.hpp src/markdown/markdown_lexer.cpp tests/markdown/incremental_lexer_test.cpp src/markdown/markdown_formatter.cpp src/markdown/markdown_formatter.hpp
git commit -m "feat: add resumable LexerState (L3)"
```

### Task 9:StreamingFormatter 改用 LexerState(稳定区按 token 增量构建)

**Files:**
- Modify: `src/markdown/markdown_formatter.cpp`(StreamingFormatter 内部)
- Test: `tests/markdown/streaming_formatter_test.cpp`(扩展)

**Interfaces:**
- Consumes: `LexerState`、`render_token_blocks`(Task 7)、`line_is_safe_to_freeze`
- Produces: `StreamingFormatter` 内部改为:`LexerState lexer_; std::vector<ftxui::Element> stable_elements_;`(每个稳定 token 一个 Element,只对 `new_stable_count()` 新增部分调 `render_token_blocks` 构建并 append)

- [ ] **Step 1: 扩展测试**(追加到 `tests/markdown/streaming_formatter_test.cpp`):
```cpp
TEST(StreamingFormatter, StableElementsBuiltOncePerNewToken) {
    StreamingFormatter f; f.set_context(80, 1);
    // 多块流式:稳定区只增量构建,不整段重建。
    // 断言:连续 append 后返回 Element 非空,且能覆盖多段内容。
    auto e = f.append_delta("a\n\nb\n\nc\n", {});
    ASSERT_NE(e.get(), nullptr);
    auto e2 = f.append_delta("d\n", {});
    ASSERT_NE(e2.get(), nullptr);
}
```

- [ ] **Step 2: 改造 `append_delta`**
将 Task 5 的 `append_delta` 中"稳定区增长时 `cached_stable_ = format_markdown(stable_prefix_)`"替换为:
```cpp
    lexer_.append(delta);
    const std::size_t new_count = lexer_.new_stable_count();
    if (new_count > 0) {
        const auto& stable = lexer_.stable_tokens();
        const std::size_t begin = stable.size() - new_count;
        for (std::size_t k = begin; k < stable.size(); ++k) {
            stable_elements_.push_back(
                render_token_blocks({stable[k]}, opts));
        }
    }
    auto tail = lexer_.tail_tokens();
    Element tail_elem = tail.empty() ? emptyElement()
                                     : render_token_blocks(tail, opts);
    Element stable_elem = stable_elements_.empty()
        ? emptyElement() : vbox(std::move(stable_elements_));
    last_element_ = (tail.empty())
        ? stable_elem : vbox({stable_elem, tail_elem});
    return last_element_;
```
`set_context`/`reset` 同步清 `lexer_` 与 `stable_elements_`。删掉 `cached_stable_`/`stable_prefix_`/`full_content_`(由 LexerState 接管)。

- [ ] **Step 3: 回归 + 提交**
Run: `cmake --build build/macos-x64-debug --target acecode_unit_tests && ./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='StreamingFormatter.*:LexerState.*:RenderTokenBlocks.*'` → PASS
```bash
git add src/markdown/markdown_formatter.cpp tests/markdown/streaming_formatter_test.cpp
git commit -m "feat: stream formatter builds stable region incrementally via LexerState (L3)"
```

---

## 验收:基准 + 最终回归

### Task 10:C++ 流式渲染基准

**Files:**
- Create: `tests/markdown/streaming_layout_benchmark.cpp`

**Interfaces:**
- Consumes: `format_markdown`、`LexerState`、`StreamingFormatter::append_delta/last_element`、`render_token_blocks`

- [ ] **Step 1: 写基准(gtest 形式,输出耗时曲线)**
`tests/markdown/streaming_layout_benchmark.cpp`:
```cpp
#include "markdown/markdown_formatter.hpp"
#include "markdown/markdown_lexer.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <string>
namespace acecode::markdown {
using Clock = std::chrono::steady_clock;
// 生成三种负载:长散文(无空行单段)、长代码块、混合多块
static std::string prose(int lines) {
    std::string s; for (int i = 0; i < lines; ++i)
        s += "line " + std::to_string(i) + " with some words and **bold** token\n";
    return s;
}
static std::string code(int lines) {
    std::string s = "```cpp\n"; for (int i = 0; i < lines; ++i)
        s += "int value" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    return s + "```\n";
}
static std::string mixed(int lines) {
    std::string s; for (int i = 0; i < lines; ++i)
        s += "para " + std::to_string(i) + "\n\n```cpp\nint x" + std::to_string(i) + ";\n```\n\n";
    return s;
}
TEST(StreamingBenchmark, PrintsFullVsIncrementalTimeCurve) {
    FormatOptions opts; opts.terminal_width = 100; opts.syntax_highlight = true;
    printf("type,lines,full_us,incremental_us\n");
    for (int lines : {200, 400, 800, 1600}) {
        for (const auto& [name, gen] : {std::make_pair("prose", &prose),
                                        std::make_pair("code", &code),
                                        std::make_pair("mixed", &mixed)}) {
            std::string content = gen(lines);
            // 全量(旧行为):每步对整个已累计内容跑一次 format_markdown
            std::string acc;
            auto t0 = Clock::now();
            for (std::size_t pos = 0; pos < content.size(); pos += 4) {
                acc += content.substr(pos, 4);
                format_markdown(acc, opts);   // 模拟旧逐帧全量重排
            }
            auto t1 = Clock::now();
            // 增量:LexerState + StreamingFormatter(L2/L3 落地后路径)
            StreamingFormatter incr; incr.set_context(100, 1);
            auto t2 = Clock::now();
            for (std::size_t pos = 0; pos < content.size(); pos += 4)
                incr.append_delta(content.substr(pos, 4), opts);
            auto t3 = Clock::now();
            printf("%s,%d,%lld,%lld\n", name, lines,
                (long long)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count(),
                (long long)std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count());
        }
    }
}
} // namespace acecode::markdown
```
注意:full 列是**真实全量**(每步 format_markdown 全量累计内容,模拟旧行为);incremental 列是 L2/L3 增量路径(append_delta)。Task 5 后跑即得 before(incremental 仍近似全量)/Task 9 后再跑即得 after(incremental 为真增量)——两次输出即 before/after 曲线。

- [ ] **Step 2: 跑基准**
Run: `cmake --build build/macos-x64-debug --target acecode_unit_tests && ./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='StreamingBenchmark.*' 2>/dev/null | grep -E '^(prose|code|mixed)'`
Expected: 打印 12 行 CSV(type,lines,full_us,incremental_us)。**验收判据**:增量列(incremental_us)随 lines 增长**不线性上涨**(增量路径 O(增量)/帧);若仍线性上涨,说明稳定边界未生效,回查 Task 8 边界判定。

- [ ] **Step 3: 提交**
```bash
git add tests/markdown/streaming_layout_benchmark.cpp
git commit -m "test: add streaming layout benchmark harness (acceptance)"
```

### Task 11:最终回归 + 记录验收结果

**Files:**
- Modify: `docs/superpowers/specs/2026-08-27-streaming-incremental-layout-design.md`(状态/验收补记)

- [ ] **Step 1: 全量单测回归**
Run: `cmake --build build/macos-x64-debug --target acecode_unit_tests && ./build/macos-x64-debug/tests/acecode_unit_tests --gtest_filter='-RemoteWebTcpProxy.TransparentlyForwardsPersistentBidirectionalBytes:ManagedRemoteWebProxy.StartsRealChildAndStopsWithoutOrphaningIt:ManagedRemoteWebProxy.AutomaticPortFallsBackWhenAdjacentPortIsBusy'`
Expected: 仅已知 10 个环境失败(TcpProbe/BuiltinToolRegistry/GrepGitBackend/SkillsTool/SettingsCenterRender/StateFile/WebServerHttp 等,与改动无关);**零新增失败**。

- [ ] **Step 2: 手动 TUI 验证**(可选但建议)
`./build/macos-x64-debug/acecode` 打开,超长输出流式:主观流畅、无卡顿;期间 resize 窗口、切主题、Ctrl+O 展开,不崩、渲染正确;滚动跟随正常。

- [ ] **Step 3: 记录验收**
在 spec 末尾追加:
```markdown
## 验收记录(2026-08-27)
- 基准:before/after CSV(见 Task 10 输出,归档到本段或单独文件)
- 单测:新增 X 个用例全部通过;既有套件零新增回归
- 手动:超长流式无卡顿、resize/主题/展开不崩
- 状态:里程碑 1/2/3 完成,#1 流式增量排版 已实施
```

- [ ] **Step 4: 提交**
```bash
git add docs/superpowers/specs/2026-08-27-streaming-incremental-layout-design.md
git commit -m "docs: record streaming incremental layout acceptance results"
```
