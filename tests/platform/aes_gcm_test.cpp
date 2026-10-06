#include <gtest/gtest.h>

#include "platform/crypto/aes_gcm.hpp"

#include <string>

// platform/crypto/aes_gcm:QQ 扫码配置用它解密服务端返回的 AppSecret。
// 向量取自 GCM 规范(McGrew & Viega)Test Case 13/14/15,并用 Python
// cryptography 库交叉核对过。

namespace acecode::platform {
namespace {

std::string hex(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2)
        out.push_back(static_cast<char>(std::stoi(text.substr(i, 2), nullptr, 16)));
    return out;
}

// 场景:GCM 规范 Test Case 15(AES-256,64 字节明文,无附加数据)。
// 期望:解密结果与规范给出的明文逐字节一致。
TEST(AesGcm, DecryptsSpecificationTestCase15) {
    const auto key = hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    const auto iv = hex("cafebabefacedbaddecaf888");
    const auto ciphertext = hex(
        "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
        "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad");
    const auto tag = hex("b094dac5d93471bdec1a502270e3cc6c");
    const auto expected = hex(
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
        "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255");
    std::string plaintext, error;
    ASSERT_TRUE(aes_256_gcm_decrypt(key, iv, ciphertext, tag, plaintext, &error)) << error;
    EXPECT_EQ(plaintext, expected);
}

// 场景:Test Case 14(全零密钥与 IV,16 字节全零明文)与 Test Case 13(空明文,只有标签)。
// 期望:前者解出 16 个零字节;后者解出空串且认证通过。
TEST(AesGcm, DecryptsZeroKeyVectorsIncludingEmptyPlaintext) {
    const std::string key(32, '\0');
    const std::string iv(12, '\0');
    std::string plaintext, error;
    ASSERT_TRUE(aes_256_gcm_decrypt(key, iv, hex("cea7403d4d606b6e074ec5d3baf39d18"),
                                    hex("d0d1c8a799996bf0265b98b5d48ab919"), plaintext, &error))
        << error;
    EXPECT_EQ(plaintext, std::string(16, '\0'));
    ASSERT_TRUE(aes_256_gcm_decrypt(key, iv, "", hex("530f8afbc74536b9a963b4f1c4cb738b"),
                                    plaintext, &error))
        << error;
    EXPECT_TRUE(plaintext.empty());
}

// 场景:密文被改动一个比特、或标签被改动、或换了一把密钥。
// 期望:三种情况都认证失败,返回 false 且不输出任何明文。
TEST(AesGcm, RejectsTamperedCiphertextTagOrWrongKey) {
    const auto key = hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    const auto iv = hex("cafebabefacedbaddecaf888");
    auto ciphertext = hex(
        "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
        "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad");
    auto tag = hex("b094dac5d93471bdec1a502270e3cc6c");
    std::string plaintext, error;

    auto flipped = ciphertext;
    flipped[5] = static_cast<char>(flipped[5] ^ 0x01);
    EXPECT_FALSE(aes_256_gcm_decrypt(key, iv, flipped, tag, plaintext, &error));
    EXPECT_TRUE(plaintext.empty());
    EXPECT_NE(error.find("authentication"), std::string::npos) << error;

    auto bad_tag = tag;
    bad_tag[0] = static_cast<char>(bad_tag[0] ^ 0x80);
    EXPECT_FALSE(aes_256_gcm_decrypt(key, iv, ciphertext, bad_tag, plaintext, &error));
    EXPECT_TRUE(plaintext.empty());

    auto wrong_key = key;
    wrong_key[31] = static_cast<char>(wrong_key[31] ^ 0x01);
    EXPECT_FALSE(aes_256_gcm_decrypt(wrong_key, iv, ciphertext, tag, plaintext, &error));
    EXPECT_TRUE(plaintext.empty());
}

// 场景:调用方传入长度不对的密钥、nonce 或标签(例如 base64 解码出错)。
// 期望:不调用底层加密库,直接返回 false 并说明哪一项长度不对。
TEST(AesGcm, ValidatesKeyNonceAndTagLengths) {
    std::string plaintext, error;
    EXPECT_FALSE(aes_256_gcm_decrypt(std::string(16, 'k'), std::string(12, 'n'), "x",
                                     std::string(16, 't'), plaintext, &error));
    EXPECT_NE(error.find("32 bytes"), std::string::npos) << error;
    EXPECT_FALSE(aes_256_gcm_decrypt(std::string(32, 'k'), std::string(16, 'n'), "x",
                                     std::string(16, 't'), plaintext, &error));
    EXPECT_NE(error.find("12 bytes"), std::string::npos) << error;
    EXPECT_FALSE(aes_256_gcm_decrypt(std::string(32, 'k'), std::string(12, 'n'), "x",
                                     std::string(8, 't'), plaintext, &error));
    EXPECT_NE(error.find("16 bytes"), std::string::npos) << error;
}

} // namespace
} // namespace acecode::platform
