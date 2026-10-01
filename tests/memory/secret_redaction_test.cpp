// 覆盖 src/domain/memory/secret_redaction.{hpp,cpp}(openspec unify-memory-system 2.3 / D7):
// 写入记忆前的统一脱敏,逐类命中,且普通文本(含 token 一词的自然语言)不被误改。

#include <gtest/gtest.h>

#include "memory/secret_redaction.hpp"

namespace {

std::string redact(const std::string& text) { return acecode::redact_secrets(text).text; }

} // namespace

// 场景:常见前缀的密钥 / 令牌。期望:整段令牌替换为 [REDACTED],前后文字保留。
TEST(SecretRedactionTest, KnownTokenPrefixes) {
    EXPECT_EQ(redact("key sk-abcdefghijklmnopqrstuvwx end"), "key [REDACTED] end");
    EXPECT_EQ(redact("sk-ant-api03-ABCDEFGHIJKLMNOPQRSTUV"), "[REDACTED]");
    EXPECT_EQ(redact("gh ghp_0123456789abcdefghijABCDEFGHIJ"), "gh [REDACTED]");
    EXPECT_EQ(redact("gho_0123456789abcdefghijABCDEFGHIJ"), "[REDACTED]");
    EXPECT_EQ(redact("xai-0123456789abcdefghijABCDEFGHIJ"), "[REDACTED]");
    EXPECT_EQ(redact("aws AKIAIOSFODNN7EXAMPLE id"), "aws [REDACTED] id");
    EXPECT_EQ(redact("github_pat_11ABCDEFG0123456789_abcdefghij"), "[REDACTED]");
}

// 场景:像前缀但很短的普通词(scikit-learn 的 sk-learn、函数名 my_sk-x)。
// 期望:不足最短令牌长度、或前面紧挨着标识符字符时不替换。
TEST(SecretRedactionTest, ShortOrEmbeddedPrefixesAreKept) {
    EXPECT_EQ(redact("pip install sk-learn"), "pip install sk-learn");
    EXPECT_EQ(redact("mask-abcdefghijklmnopqrstuvwxyz"), "mask-abcdefghijklmnopqrstuvwxyz");
    EXPECT_EQ(redact("AKIA is the AWS id prefix"), "AKIA is the AWS id prefix");
}

// 场景:Authorization: Bearer <token>。期望:保留 Bearer 字样,只替换令牌;
// 「bearer of bad news」这类自然语言不变。
TEST(SecretRedactionTest, BearerTokens) {
    EXPECT_EQ(redact("Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload.sig"),
              "Authorization: Bearer [REDACTED]");
    EXPECT_EQ(redact("he was the bearer of bad news"), "he was the bearer of bad news");
}

// 场景:PEM 私钥块。期望:BEGIN 到对应 END 整块替换;公钥证书不受影响。
TEST(SecretRedactionTest, PrivateKeyBlock) {
    const std::string text =
        "before\n-----BEGIN PRIVATE KEY-----\nMIIEvQIBADANBg\nkqhkiG9w0BAQEF\n"
        "-----END PRIVATE KEY-----\nafter";
    EXPECT_EQ(redact(text), "before\n[REDACTED]\nafter");
    const std::string rsa = "-----BEGIN RSA PRIVATE KEY-----\nabc\n-----END RSA PRIVATE KEY-----";
    EXPECT_EQ(redact(rsa), "[REDACTED]");
    const std::string cert = "-----BEGIN CERTIFICATE-----\nabc\n-----END CERTIFICATE-----";
    EXPECT_EQ(redact(cert), cert);
}

// 场景:URL 中带用户名密码。期望:凭据部分替换,主机与路径保留;只有用户名的
// ssh://git@host 保持原样。
TEST(SecretRedactionTest, UrlCredentials) {
    EXPECT_EQ(redact("clone https://user:pass@example.com/path now"),
              "clone https://[REDACTED]@example.com/path now");
    EXPECT_EQ(redact("ssh://git@github.com/org/repo.git"), "ssh://git@github.com/org/repo.git");
}

// 场景:password= / passwd: / token= / api_key= / 复合键名 / JSON 键。
// 期望:只替换值,键名与分隔符保留。
TEST(SecretRedactionTest, KeyValueAssignments) {
    EXPECT_EQ(redact("password=hunter2"), "password=[REDACTED]");
    EXPECT_EQ(redact("passwd: s3cr3t next"), "passwd: [REDACTED] next");
    EXPECT_EQ(redact("token=abc123&x=1"), "token=[REDACTED]&x=1");
    EXPECT_EQ(redact("api_key = 'xyz789'"), "api_key = '[REDACTED]'");
    EXPECT_EQ(redact("APIKEY:qwerty"), "APIKEY:[REDACTED]");
    EXPECT_EQ(redact("DB_PASSWORD=pa55"), "DB_PASSWORD=[REDACTED]");
    EXPECT_EQ(redact("access_token=abc"), "access_token=[REDACTED]");
    EXPECT_EQ(redact(R"({"password": "hunter2"})"), R"({"password": "[REDACTED]"})");
    EXPECT_EQ(redact("pwd=x1"), "pwd=[REDACTED]");
    EXPECT_EQ(redact("client secret: abc"), "client secret: [REDACTED]");
}

// 场景:中文「密码」后跟全角或半角冒号(规格示例:测试账号 tester01 / 密码:abc#2026)。
// 期望:密码值被替换,其余文字保留。
TEST(SecretRedactionTest, ChinesePasswordWithFullWidthColon) {
    EXPECT_EQ(redact(u8"测试账号 tester01 / 密码：abc#2026"),
              u8"测试账号 tester01 / 密码：[REDACTED]");
    EXPECT_EQ(redact(u8"密码: abc 其余"), u8"密码: [REDACTED] 其余");
}

// 场景:包含 token / password 等词的普通自然语言(没有赋值分隔符)。
// 期望:原样保留,不计替换次数。
TEST(SecretRedactionTest, NaturalLanguageIsNotChanged) {
    const std::string text =
        "Count the token budget before sending; the password reset page is at /help. "
        "Tokens are cheap. token_type is bearer. a==b compares tokens == 1.";
    const auto result = acecode::redact_secrets(text);
    EXPECT_EQ(result.text, text);
    EXPECT_EQ(result.replacements, 0);
}

// 场景:已经脱敏过的文本再次经过脱敏(整合产物、遗忘时的改写)。
// 期望:幂等,不再计数。
TEST(SecretRedactionTest, AlreadyRedactedTextIsStable) {
    const std::string once = redact("password=hunter2 https://u:p@h.com Bearer abcdefghij");
    const auto twice = acecode::redact_secrets(once);
    EXPECT_EQ(twice.text, once);
    EXPECT_EQ(twice.replacements, 0);
}
