#include "jb_slot.hpp"

#include "../utils/sha256.hpp"

#include <zlib.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace acecode {
namespace {

#include "jb_slot_table.inc"

int b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<unsigned char> b64_decode(const char* text) {
    std::vector<unsigned char> out;
    int val = 0;
    int valb = -8;
    for (const char* p = text; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int d = b64_value(c);
        if (d < 0) return {};
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<unsigned char>((val >> valb) & 0xff));
            valb -= 8;
        }
    }
    return out;
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::array<unsigned char, 32> sha256_bytes(const std::string& input) {
    const std::string hex = sha256_hex(input);
    std::array<unsigned char, 32> out{};
    for (int i = 0; i < 32; ++i) {
        const int hi = hex_nibble(hex[static_cast<std::size_t>(i * 2)]);
        const int lo = hex_nibble(hex[static_cast<std::size_t>(i * 2 + 1)]);
        out[static_cast<std::size_t>(i)] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return out;
}

std::array<unsigned char, 32> mix_key(const char* mask) {
    std::string material = "cordis.slot.v2";
    material.push_back('\x1e');
    material += "dsh-purge";
    material.push_back('\x1e');
    material += "surface:web|desktop";
    material.push_back('\x1e');
    material += "ov.table.1";
    material.push_back('\x1e');
    material += mask;
    return sha256_bytes(material);
}

bool aes_gcm_decrypt(const unsigned char* key, std::size_t key_len,
                     const unsigned char* iv, std::size_t iv_len,
                     const unsigned char* ct, std::size_t ct_len,
                     const unsigned char* tag, std::size_t tag_len,
                     std::vector<unsigned char>* plain) {
    if (!plain || key_len != 32 || iv_len != 12 || tag_len != 16) return false;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return false;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    BCRYPT_KEY_HANDLE handle = nullptr;
    if (BCryptGenerateSymmetricKey(alg, &handle, nullptr, 0,
                                   const_cast<PUCHAR>(key),
                                   static_cast<ULONG>(key_len), 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    std::vector<unsigned char> tag_buf(tag, tag + tag_len);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(iv);
    info.cbNonce = static_cast<ULONG>(iv_len);
    info.pbTag = tag_buf.data();
    info.cbTag = static_cast<ULONG>(tag_buf.size());
    plain->assign(ct_len, 0);
    ULONG written = 0;
    const NTSTATUS status = BCryptDecrypt(
        handle, const_cast<PUCHAR>(ct), static_cast<ULONG>(ct_len), &info,
        nullptr, 0, plain->empty() ? nullptr : plain->data(),
        static_cast<ULONG>(plain->size()), &written, 0);
    BCryptDestroyKey(handle);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status < 0) {
        plain->clear();
        return false;
    }
    plain->resize(written);
    return true;
#else
    (void)key;
    (void)iv;
    (void)ct;
    (void)ct_len;
    (void)tag;
    plain->clear();
    return false;
#endif
}

std::string inflate_zlib(const unsigned char* data, std::size_t len) {
    z_stream strm{};
    if (inflateInit(&strm) != Z_OK) return {};
    strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    strm.avail_in = static_cast<uInt>(len);
    std::string out;
    unsigned char buf[16384];
    int ret = Z_OK;
    while (ret == Z_OK) {
        strm.next_out = buf;
        strm.avail_out = sizeof(buf);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&strm);
            return {};
        }
        out.append(reinterpret_cast<char*>(buf), sizeof(buf) - strm.avail_out);
    }
    inflateEnd(&strm);
    return ret == Z_STREAM_END ? out : std::string{};
}

} // namespace

bool jb_slot_crypto_matches_vector() {
    unsigned char key[32];
    std::memset(key, 0x11, sizeof(key));
    unsigned char iv[12];
    std::memset(iv, 0x22, sizeof(iv));
    const unsigned char ct[] = {0x76, 0x95, 0x64};
    const unsigned char tag[] = {
        0x38, 0xb0, 0xbf, 0xc4, 0xf5, 0x7a, 0x3c, 0x21,
        0xdf, 0x1d, 0xca, 0xd8, 0xa7, 0xef, 0x37, 0x98};
    std::vector<unsigned char> plain;
    if (!aes_gcm_decrypt(key, 32, iv, 12, ct, 3, tag, 16, &plain)) return false;
    return plain.size() == 3 && plain[0] == 'a' && plain[1] == 'b' && plain[2] == 'c';
}

std::string open_jb_slot() {
    const auto body = b64_decode(kJbSlot);
    const auto iv = b64_decode(kJbIv);
    const auto tag = b64_decode(kJbTag);
    if (body.empty() || iv.size() != 12 || tag.size() != 16) return {};
    const auto key = mix_key(kJbMask);
    std::vector<unsigned char> packed;
    if (!aes_gcm_decrypt(key.data(), key.size(), iv.data(), iv.size(),
                         body.data(), body.size(), tag.data(), tag.size(), &packed)) {
        return {};
    }
    return inflate_zlib(packed.data(), packed.size());
}

} // namespace acecode
