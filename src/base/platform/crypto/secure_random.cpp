#include "secure_random.hpp"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/rand.h>
#endif

namespace acecode::platform {

std::string secure_random_bytes(std::size_t count) {
    std::string out(count, '\0');
    if (count == 0) return out;
#ifdef _WIN32
    const NTSTATUS status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()),
                                            static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) return {};
#else
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(count)) != 1) return {};
#endif
    return out;
}

std::string secure_random_token(std::size_t count) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";  // 64 个字符
    const auto bytes = secure_random_bytes(count);
    if (bytes.size() != count) return {};
    std::string token;
    token.reserve(count);
    for (const char b : bytes) token.push_back(kAlphabet[static_cast<unsigned char>(b) & 0x3F]);
    return token;
}

} // namespace acecode::platform
