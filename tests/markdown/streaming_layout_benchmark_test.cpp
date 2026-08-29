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
