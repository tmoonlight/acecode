#pragma once

// 密码学安全的随机字节:Windows BCryptGenRandom,其它平台 OpenSSL RAND_bytes。
// 用于 QQ 扫码配置的一次性密钥、Telegram 机主绑定码等。

#include <cstddef>
#include <string>

namespace acecode::platform {

// 失败时返回空串(调用方必须当作错误处理,绝不能退化成弱随机)。
std::string secure_random_bytes(std::size_t count);

// count 个 URL 安全字符([A-Za-z0-9_-]),基于 secure_random_bytes。失败返回空串。
std::string secure_random_token(std::size_t count);

} // namespace acecode::platform
