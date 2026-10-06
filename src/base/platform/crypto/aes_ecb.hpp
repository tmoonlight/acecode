#pragma once

// AES-128-ECB + PKCS#7 填充。Windows 走 BCrypt,其它平台走 OpenSSL libcrypto。
// 用途:微信 iLink 机器人的媒体文件经 CDN 传输时,文件内容用一次性的 16 字节密钥加解密
// (协议规定的算法;ECB 不适合一般场景,不要拿来做别的)。

#include <string>

namespace acecode::platform {

// key 必须 16 字节。成功返回 true;error 只描述原因,绝不包含密钥或内容。
bool aes_128_ecb_encrypt(const std::string& key, const std::string& plaintext, std::string& ciphertext,
                         std::string* error);
// 密文长度必须是 16 的倍数,填充不合法时返回 false。
bool aes_128_ecb_decrypt(const std::string& key, const std::string& ciphertext, std::string& plaintext,
                         std::string* error);

// PKCS#7 填充后的长度(明文长度向上取整到 16 的倍数,正好整除时再加一块)。
std::size_t aes_128_ecb_padded_size(std::size_t plaintext_size);

} // namespace acecode::platform
