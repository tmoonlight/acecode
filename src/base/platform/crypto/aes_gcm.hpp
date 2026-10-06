#pragma once

// AES-256-GCM 解密(只解密,不加密)。Windows 走 BCrypt,其它平台走 OpenSSL libcrypto。
// 用途:QQ 机器人扫码配置时,服务端用客户端临时生成的密钥加密 AppSecret 返回。

#include <string>

namespace acecode::platform {

// key 必须 32 字节,iv 必须 12 字节,tag 必须 16 字节。认证失败(密文或标签被篡改、
// 密钥不对)返回 false 且不输出明文;error 只描述原因,绝不包含密钥或明文。
bool aes_256_gcm_decrypt(const std::string& key, const std::string& iv,
                         const std::string& ciphertext, const std::string& tag,
                         std::string& plaintext, std::string* error);

} // namespace acecode::platform
