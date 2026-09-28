#include <gtest/gtest.h>
#include "platform/utf8_command_line.hpp"
#include <vector>

TEST(Utf8CommandLine, NarrowTailKeepsSpacesAndEmptyArguments) {
    // 已经分词的 UTF-8 参数不能再次按空白切开,显式空参数也要保留。
    std::vector<std::string> values{"acecode", "-p", u8"你好 世界", ""};
    std::vector<char*> args;
    for (auto& value : values) args.push_back(value.data());
    EXPECT_EQ(acecode::platform::argv_tail(
        static_cast<int>(args.size()), args.data(), 1),
        (std::vector<std::string>{"-p", u8"你好 世界", ""}));
}
#ifdef _WIN32
TEST(Utf8CommandLine, WideWindowsInputPreservesChinesePromptAndQuotedProgram) {
    // Windows 的 ANSI argv 不可信,真正的中文提示来自宽字符命令行。
    std::vector<std::string> values{"acecode", "-p", "broken-ansi"};
    std::vector<char*> args;
    for (auto& value : values) args.push_back(value.data());
    EXPECT_EQ(acecode::platform::utf8_command_line_tail(
        static_cast<int>(args.size()), args.data(),
        LR"("C:\工具\acecode.exe" -p "你好 世界" --output-format json)"),
        (std::vector<std::string>{"-p", u8"你好 世界", "--output-format", "json"}));
}
#endif
