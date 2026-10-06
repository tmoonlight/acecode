#include "aes_gcm.hpp"

#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

namespace acecode::platform {
namespace {

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

#ifdef _WIN32
constexpr NTSTATUS kAuthTagMismatch = static_cast<NTSTATUS>(0xC000A002L);

bool decrypt_native(const std::string& key, const std::string& iv, const std::string& ciphertext,
                    const std::string& tag, std::string& plaintext, std::string* error) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    auto cleanup = [&] {
        if (handle) BCryptDestroyKey(handle);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    };
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) {
        cleanup();
        return fail(error, "AES provider unavailable");
    }
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0) {
        cleanup();
        return fail(error, "AES-GCM mode unavailable");
    }
    if (BCryptGenerateSymmetricKey(alg, &handle, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                   static_cast<ULONG>(key.size()), 0) < 0) {
        cleanup();
        return fail(error, "Invalid AES key");
    }
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char*>(iv.data()));
    info.cbNonce = static_cast<ULONG>(iv.size());
    info.pbTag = reinterpret_cast<PUCHAR>(const_cast<char*>(tag.data()));
    info.cbTag = static_cast<ULONG>(tag.size());
    std::vector<unsigned char> out(ciphertext.empty() ? 1 : ciphertext.size());
    ULONG written = 0;
    const NTSTATUS status = BCryptDecrypt(
        handle, reinterpret_cast<PUCHAR>(const_cast<char*>(ciphertext.data())),
        static_cast<ULONG>(ciphertext.size()), &info, nullptr, 0, out.data(),
        static_cast<ULONG>(ciphertext.size()), &written, 0);
    cleanup();
    if (status == kAuthTagMismatch) return fail(error, "AES-GCM authentication failed");
    if (status < 0) return fail(error, "AES-GCM decryption failed");
    plaintext.assign(reinterpret_cast<const char*>(out.data()), written);
    return true;
}
#else
bool decrypt_native(const std::string& key, const std::string& iv, const std::string& ciphertext,
                    const std::string& tag, std::string& plaintext, std::string* error) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return fail(error, "AES provider unavailable");
    std::vector<unsigned char> out(ciphertext.size() + 16);
    int len = 0;
    int final_len = 0;
    const auto* k = reinterpret_cast<const unsigned char*>(key.data());
    const auto* n = reinterpret_cast<const unsigned char*>(iv.data());
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr, k, n) == 1 &&
              EVP_DecryptUpdate(ctx, out.data(), &len,
                                reinterpret_cast<const unsigned char*>(ciphertext.data()),
                                static_cast<int>(ciphertext.size())) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()),
                                  const_cast<char*>(tag.data())) == 1;
    if (!ok) {
        EVP_CIPHER_CTX_free(ctx);
        return fail(error, "AES-GCM decryption failed");
    }
    const bool authentic = EVP_DecryptFinal_ex(ctx, out.data() + len, &final_len) > 0;
    EVP_CIPHER_CTX_free(ctx);
    if (!authentic) return fail(error, "AES-GCM authentication failed");
    plaintext.assign(reinterpret_cast<const char*>(out.data()), static_cast<std::size_t>(len + final_len));
    return true;
}
#endif

} // namespace

bool aes_256_gcm_decrypt(const std::string& key, const std::string& iv,
                         const std::string& ciphertext, const std::string& tag,
                         std::string& plaintext, std::string* error) {
    plaintext.clear();
    if (key.size() != 32) return fail(error, "AES-256 key must be 32 bytes");
    if (iv.size() != 12) return fail(error, "AES-GCM nonce must be 12 bytes");
    if (tag.size() != 16) return fail(error, "AES-GCM tag must be 16 bytes");
    std::string result;
    if (!decrypt_native(key, iv, ciphertext, tag, result, error)) return false;
    plaintext = std::move(result);
    return true;
}

} // namespace acecode::platform
