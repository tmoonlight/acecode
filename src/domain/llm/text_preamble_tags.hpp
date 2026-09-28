#pragma once

// 历史里残留的 <text_preamble> 标签的流式识别与剥除,以及标题文本的通用规整(P2-02 自
// tool_preamble 拆出)。纯字符串逻辑,无 IO、无 provider 依赖;agent_loop、TUI 回放与 session
// 都要用,所以放在 domain 层的 llm 模块,tool_preamble 经 using 声明复用。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace acecode::llm {

// 历史标签的标签名与 type 取值(扫描器只用来剥标签)。kind 由工具类型决定
// (读 / 写),透传给界面,「读放大镜 / 写笔触」效果留给以后接。
inline constexpr const char* kTagName = "text_preamble";
inline constexpr const char* kKindRead = "read";
inline constexpr const char* kKindWrite = "write";

// 标签正文的标题长度上限(Unicode code point,扫描器规整用)。
inline constexpr std::size_t kTextPreambleMaxCodePoints = 200;
// 标签体流了这么多字节还没闭合、也没换行,就按到此为止处理(防模型忘了闭合
// 把整段回答都吞进 loading)。
inline constexpr std::size_t kTextPreambleMaxBodyBytes = 1200;

// 通用规整:换行 / 制表符压成空格、折叠连续空白、去掉包裹的引号 / 反引号 /
// 加粗记号 / 标题井号 / 列表记号、去掉末尾的冒号 / 句号 / 省略号,再按
// code point 截断(截断处追加 "…")。返回空串表示没有可用文本。
std::string normalize_title_line(const std::string& text, std::size_t max_code_points);

// 按 code point 截断(前缀 + "…")。
std::string truncate_code_points(const std::string& text, std::size_t max_code_points);

// ---- 历史里残留的 <text_preamble> 标签:流式识别并剥掉 ----

struct TextPreamble {
    std::string title;   // 规整后的标签正文(空 = 标签没有可用内容)
    std::string kind;    // "read" / "write" / ""(没写 type 或写了别的)
};

// 流式扫描器:把模型正文的增量切成「可见正文」与「前言」。
//   - `<text_preamble type="read">…</text_preamble>` 整段不进可见正文;闭合标签
//     一到就产出一条 TextPreamble(所以 loading 能在工具调用流出来之前换文案)。
//   - 开标签可能被切在任意字节处:尾部是 "<text_preamble" 的前缀时先扣住,
//     等下一段增量再判;不是标签的 "<" 原样放行。
//   - 宽松:没写 type 也认;`</text_preamble>` 缺失时正文遇到换行就当闭合;
//     正文超过 kTextPreambleMaxBodyBytes 仍未闭合也当闭合;`<text_preamble/>`
//     空标签直接跳过;标签名大小写不敏感。
//   - 流开头与每个闭合标签之后紧跟的空白(通常是 "\n\n")一并吞掉,免得界面
//     为一段空白建一条空气泡;第一个非空白可见字符之后恢复原样透传。
//   - flush():流结束时把扣住的字节结清 —— 没闭合的标签正文仍算前言,
//     只是半截开标签("<text_pre")按普通文本放行。
class TextPreambleScanner {
public:
    struct Output {
        std::string visible;                 // 可以直接当 token 下发的正文
        std::vector<TextPreamble> preambles; // 本次增量里闭合的标签(通常 0 或 1 条)
    };

    Output feed(std::string_view delta);
    Output flush();
    void reset();

private:
    void drain(Output& out, bool at_end);
    void emit_visible(Output& out, std::string_view text);
    void close_tag(Output& out, std::string_view body);

    std::string pending_;      // 扣住的字节:半截开标签,或未闭合的标签正文
    bool in_tag_ = false;      // pending_ 是不是标签正文
    std::string kind_;         // 当前开标签的 type
    bool swallow_leading_ws_ = true;
};

// 整段文本剥掉标签(渲染层用:TUI 回放 / on_message 的完整正文、导出等;Web 端
// 在 toolPreamble.js 里有同款)。就是「喂给一个新扫描器再 flush」。
std::string strip_text_preamble_tags(const std::string& text);

}  // namespace acecode::llm
