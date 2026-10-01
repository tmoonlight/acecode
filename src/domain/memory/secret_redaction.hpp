#pragma once

#include <string>
#include <string_view>

namespace acecode {

inline constexpr const char* kRedactedMarker = "[REDACTED]";

struct SecretRedactionResult {
    std::string text;
    int replacements = 0;

    bool redacted() const { return replacements > 0; }
};

// 写入记忆前的统一脱敏(openspec unify-memory-system D7)。逐字符扫描,不用
// std::regex(大输入下 MSVC 实现慢且有栈溢出风险)。识别:
//   - PEM 私钥块(BEGIN ... PRIVATE KEY 到对应 END 行,整块替换)
//   - URL 里的用户名密码(scheme://user:pass@host → scheme://[REDACTED]@host)
//   - 常见前缀的密钥 / 令牌(sk-、ghp_、gho_、ghu_、ghs_、ghr_、github_pat_、
//     xai-、xoxb-/xoxp-、AKIA、AIza),要求前缀后有足够长的令牌字符,
//     「sk-learn」这类短词不受影响
//   - Bearer 令牌(保留 Bearer 字样,只替换令牌)
//   - password / passwd / pwd / token / secret / api_key / apikey / 密码 后跟
//     = / : / :的值(键名可作为 access_token、DB_PASSWORD 这类复合名的后缀)
// 已经是 [REDACTED] 的值不重复计数,函数幂等。
SecretRedactionResult redact_secrets(std::string_view input);

} // namespace acecode
