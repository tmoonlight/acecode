#pragma once

// 摘要与消息认证码。Windows 走 BCrypt,其它平台走 OpenSSL libcrypto。
// 用途:LINE 回调签名(HMAC-SHA256)、微信 iLink 媒体上传要求的原文件 MD5。

#include <string>

namespace acecode::platform {

// 原始摘要字节(MD5 16 字节、HMAC-SHA256 32 字节);失败返回空串。
std::string md5_digest(const std::string& data);
std::string hmac_sha256(const std::string& key, const std::string& data);

// 小写十六进制。
std::string to_hex(const std::string& bytes);

} // namespace acecode::platform
