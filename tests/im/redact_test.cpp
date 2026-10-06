#include <gtest/gtest.h>

#include "im/redact.hpp"

// im/redact:通道凭据只能以脱敏形式出现在接口返回、日志与错误文本里。

namespace acecode::im {
namespace {

// 场景:设置页展示已保存的 AppSecret / token。
// 期望:只露出末 4 位;过短的值整体隐藏。
TEST(ImRedact, MaskKeepsOnlyLastFourCharacters) {
    EXPECT_EQ(mask_secret("abcdefgh1234"), "****1234");
    EXPECT_EQ(mask_secret("short"), "****");
    EXPECT_EQ(mask_secret(""), "****");
}

// 场景:Telegram 请求失败的错误文本里带着完整 URL(token 在路径里)。
// 期望:token 被替换成 ***,其余诊断信息保留。
TEST(ImRedact, RemovesTelegramTokenFromUrls) {
    const std::string error =
        "GET https://api.telegram.org/bot123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw0/getMe failed";
    EXPECT_EQ(redact_secrets(error), "GET https://api.telegram.org/bot***/getMe failed");
}

// 场景:错误文本中出现已知的 AppSecret;另有一个只有 3 位的“密钥”和普通的时间/端口文字。
// 期望:已知密钥被抹掉;过短的值不处理;12345:short 这类文字不被误伤。
TEST(ImRedact, RemovesListedSecretsWithoutFalsePositives) {
    EXPECT_EQ(redact_secrets("secret=Zx9QpL0mN7 appears twice: Zx9QpL0mN7", {"Zx9QpL0mN7", "abc"}),
              "secret=*** appears twice: ***");
    EXPECT_EQ(redact_secrets("port 12345:short abc"), "port 12345:short abc");
}

} // namespace
} // namespace acecode::im
