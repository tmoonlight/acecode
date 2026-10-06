#include "aes_ecb.hpp"

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
// BCrypt 的 ECB 不做填充(不传 BCRYPT_BLOCK_PADDING),填充在外面按 PKCS#7 自己处理,
// 两个平台的行为因此完全一致。
bool run_native(const std::string& key, const std::string& input, bool encrypt, std::string& output,
                std::string* error) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    const auto cleanup = [&alg, &handle] {
        if (handle) BCryptDestroyKey(handle);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    };
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) {
        cleanup();
        return fail(error, "AES provider unavailable");
    }
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
                          sizeof(BCRYPT_CHAIN_MODE_ECB), 0) < 0) {
        cleanup();
        return fail(error, "AES-ECB mode unavailable");
    }
    if (BCryptGenerateSymmetricKey(alg, &handle, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                                   static_cast<ULONG>(key.size()), 0) < 0) {
        cleanup();
        return fail(error, "Invalid AES key");
    }
    std::vector<unsigned char> out(input.empty() ? 1 : input.size());
    ULONG written = 0;
    auto* in = reinterpret_cast<PUCHAR>(const_cast<char*>(input.data()));
    const auto size = static_cast<ULONG>(input.size());
    const NTSTATUS status = encrypt
        ? BCryptEncrypt(handle, in, size, nullptr, nullptr, 0, out.data(), size, &written, 0)
        : BCryptDecrypt(handle, in, size, nullptr, nullptr, 0, out.data(), size, &written, 0);
    cleanup();
    if (status < 0) return fail(error, encrypt ? "AES-ECB encryption failed" : "AES-ECB decryption failed");
    output.assign(reinterpret_cast<const char*>(out.data()), written);
    return true;
}
#else
bool run_native(const std::string& key, const std::string& input, bool encrypt, std::string& output,
                std::string* error) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return fail(error, "AES provider unavailable");
    std::vector<unsigned char> out(input.size() + 16);
    int len = 0;
    int final_len = 0;
    const auto* k = reinterpret_cast<const unsigned char*>(key.data());
    const auto* in = reinterpret_cast<const unsigned char*>(input.data());
    const bool ok = EVP_CipherInit_ex(ctx, EVP_aes_128_ecb(), nullptr, k, nullptr, encrypt ? 1 : 0) == 1 &&
                    EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
                    EVP_CipherUpdate(ctx, out.data(), &len, in, static_cast<int>(input.size())) == 1 &&
                    EVP_CipherFinal_ex(ctx, out.data() + len, &final_len) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return fail(error, encrypt ? "AES-ECB encryption failed" : "AES-ECB decryption failed");
    output.assign(reinterpret_cast<const char*>(out.data()), static_cast<std::size_t>(len + final_len));
    return true;
}
#endif

} // namespace

std::size_t aes_128_ecb_padded_size(std::size_t plaintext_size) { return (plaintext_size / 16 + 1) * 16; }

bool aes_128_ecb_encrypt(const std::string& key, const std::string& plaintext, std::string& ciphertext,
                         std::string* error) {
    ciphertext.clear();
    if (key.size() != 16) return fail(error, "AES-128 key must be 16 bytes");
    std::string padded = plaintext;
    const auto pad = static_cast<char>(16 - plaintext.size() % 16);
    padded.append(static_cast<std::size_t>(pad), pad);
    return run_native(key, padded, true, ciphertext, error);
}

bool aes_128_ecb_decrypt(const std::string& key, const std::string& ciphertext, std::string& plaintext,
                         std::string* error) {
    plaintext.clear();
    if (key.size() != 16) return fail(error, "AES-128 key must be 16 bytes");
    if (ciphertext.empty() || ciphertext.size() % 16 != 0) return fail(error, "AES-ECB ciphertext size is invalid");
    std::string padded;
    if (!run_native(key, ciphertext, false, padded, error)) return false;
    const auto pad = static_cast<unsigned char>(padded.back());
    if (pad == 0 || pad > 16 || pad > padded.size()) return fail(error, "AES-ECB padding is invalid");
    for (std::size_t i = padded.size() - pad; i < padded.size(); ++i)
        if (static_cast<unsigned char>(padded[i]) != pad) return fail(error, "AES-ECB padding is invalid");
    padded.resize(padded.size() - pad);
    plaintext = std::move(padded);
    return true;
}

} // namespace acecode::platform
