#include <gtest/gtest.h>

#include "platform/crypto/aes_ecb.hpp"
#include "platform/crypto/digest.hpp"

#include <string>

// platform/crypto 的 MD5、HMAC-SHA256、AES-128-ECB:用公开标准向量校验(RFC 1321 / RFC 4231 /
// NIST SP 800-38A),确保 Windows BCrypt 与其它平台 OpenSSL 两条实现给出同样的结果。

namespace acecode::platform {
namespace {

std::string from_hex(const std::string& hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

// 场景:对空串与 "abc" 求 MD5(微信媒体上传要求附原文件 MD5)。
// 期望:与 RFC 1321 附录的标准值一致。
TEST(CryptoDigest, Md5MatchesRfcVectors) {
    EXPECT_EQ(to_hex(md5_digest("")), "d41d8cd98f00b204e9800998ecf8427e");
    EXPECT_EQ(to_hex(md5_digest("abc")), "900150983cd24fb0d6963f7d28e17f72");
}

// 场景:LINE 回调签名 = base64(HMAC-SHA256(channel secret, 原始请求体))。
// 期望:RFC 4231 测试用例 2 与用例 6(密钥长于分组,要先摘要)都与标准值一致。
TEST(CryptoDigest, HmacSha256MatchesRfc4231) {
    EXPECT_EQ(to_hex(hmac_sha256("Jefe", "what do ya want for nothing?")),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    EXPECT_EQ(to_hex(hmac_sha256(std::string(131, '\xaa'),
                                 "Test Using Larger Than Block-Size Key - Hash Key First")),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

// 场景:微信 CDN 的文件用 AES-128-ECB + PKCS#7 加密。
// 期望:第一块密文与 NIST SP 800-38A F.1.1 一致;整块明文会多出一整块填充;解密还原原文。
TEST(CryptoAesEcb, EncryptMatchesNistAndRoundTrips) {
    const auto key = from_hex("2b7e151628aed2a6abf7158809cf4f3c");
    const auto plain = from_hex("6bc1bee22e409f96e93d7e117393172a");
    std::string cipher;
    ASSERT_TRUE(aes_128_ecb_encrypt(key, plain, cipher, nullptr));
    ASSERT_EQ(cipher.size(), 32u);
    EXPECT_EQ(cipher.size(), aes_128_ecb_padded_size(plain.size()));
    EXPECT_EQ(to_hex(cipher.substr(0, 16)), "3ad77bb40d7a3660a89ecaf32466ef97");
    std::string back;
    ASSERT_TRUE(aes_128_ecb_decrypt(key, cipher, back, nullptr));
    EXPECT_EQ(back, plain);

    std::string short_cipher;
    ASSERT_TRUE(aes_128_ecb_encrypt(key, "hello", short_cipher, nullptr));
    EXPECT_EQ(short_cipher.size(), 16u);
    ASSERT_TRUE(aes_128_ecb_decrypt(key, short_cipher, back, nullptr));
    EXPECT_EQ(back, "hello");
}

// 场景:密钥长度不对、密文长度不是 16 的倍数、用错密钥解出非法填充。
// 期望:都返回 false 且给出原因,不输出明文。
TEST(CryptoAesEcb, RejectsBadKeyLengthAndPadding) {
    std::string out;
    std::string error;
    EXPECT_FALSE(aes_128_ecb_encrypt("short", "x", out, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(aes_128_ecb_decrypt(std::string(16, 'k'), "123", out, &error));
    std::string cipher;
    ASSERT_TRUE(aes_128_ecb_encrypt(std::string(16, 'k'), "payload", cipher, nullptr));
    EXPECT_FALSE(aes_128_ecb_decrypt(std::string(16, 'x'), cipher, out, &error));
    EXPECT_TRUE(out.empty());
}

} // namespace
} // namespace acecode::platform
