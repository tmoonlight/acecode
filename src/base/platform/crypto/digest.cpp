#include "digest.hpp"

#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

namespace acecode::platform {
namespace {

#ifdef _WIN32
std::string bcrypt_hash(LPCWSTR algorithm, const std::string* key, const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    const auto cleanup = [&alg, &hash] {
        if (hash) BCryptDestroyHash(hash);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    };
    if (BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) < 0) {
        cleanup();
        return {};
    }
    DWORD length = 0;
    DWORD written = 0;
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &written, 0) <
        0) {
        cleanup();
        return {};
    }
    auto* secret = key ? reinterpret_cast<PUCHAR>(const_cast<char*>(key->data())) : nullptr;
    const auto secret_size = key ? static_cast<ULONG>(key->size()) : 0;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, secret, secret_size, 0) < 0 ||
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                       static_cast<ULONG>(data.size()), 0) < 0) {
        cleanup();
        return {};
    }
    std::vector<unsigned char> out(length);
    const bool ok = BCryptFinishHash(hash, out.data(), length, 0) >= 0;
    cleanup();
    return ok ? std::string(reinterpret_cast<const char*>(out.data()), out.size()) : std::string{};
}
#endif

} // namespace

std::string md5_digest(const std::string& data) {
#ifdef _WIN32
    return bcrypt_hash(BCRYPT_MD5_ALGORITHM, nullptr, data);
#else
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(data.data(), data.size(), out, &length, EVP_md5(), nullptr) != 1) return {};
    return std::string(reinterpret_cast<const char*>(out), length);
#endif
}

std::string hmac_sha256(const std::string& key, const std::string& data) {
#ifdef _WIN32
    return bcrypt_hash(BCRYPT_SHA256_ALGORITHM, &key, data);
#else
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(data.data()), data.size(), out, &length))
        return {};
    return std::string(reinterpret_cast<const char*>(out), length);
#endif
}

std::string to_hex(const std::string& bytes) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0f]);
    }
    return out;
}

} // namespace acecode::platform
