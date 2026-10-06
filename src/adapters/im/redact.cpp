#include "redact.hpp"

#include <cctype>

namespace acecode::im {
namespace {

bool token_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
}

bool digit(char c) { return c >= '0' && c <= '9'; }

// 把 <至少 5 位数字>:<至少 30 位 token 字符> 换成 ***。
std::string redact_bot_tokens(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (digit(text[i]) && (i == 0 || !digit(text[i - 1]))) {
            std::size_t j = i;
            while (j < text.size() && digit(text[j])) ++j;
            if (j - i >= 5 && j < text.size() && text[j] == ':') {
                std::size_t k = j + 1;
                while (k < text.size() && token_char(text[k])) ++k;
                if (k - (j + 1) >= 30) {
                    out += "***";
                    i = k;
                    continue;
                }
            }
            out.append(text, i, j - i);
            i = j;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

} // namespace

std::string mask_secret(std::string_view secret) {
    if (secret.size() < 8) return "****";
    return "****" + std::string(secret.substr(secret.size() - 4));
}

std::string redact_secrets(std::string text, const std::vector<std::string>& secrets) {
    for (const auto& secret : secrets) {
        if (secret.size() < 6) continue;
        std::size_t pos = 0;
        while ((pos = text.find(secret, pos)) != std::string::npos) {
            text.replace(pos, secret.size(), "***");
            pos += 3;
        }
    }
    return redact_bot_tokens(text);
}

} // namespace acecode::im
