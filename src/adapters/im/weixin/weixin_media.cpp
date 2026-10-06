#include "weixin_media.hpp"

#include "im/http.hpp"
#include "im/redact.hpp"
#include "platform/crypto/aes_ecb.hpp"
#include "platform/crypto/digest.hpp"
#include "platform/crypto/secure_random.hpp"

#include <fstream>
#include <iterator>

namespace acecode::im::weixin {
namespace {

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

void remove_quietly(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace

std::string size_label(std::uint64_t bytes) {
    constexpr std::uint64_t kMiB = 1024u * 1024u;
    if (bytes >= kMiB) return std::to_string(bytes / kMiB) + " MB";
    return std::to_string((bytes + 1023u) / 1024u) + " KB";
}

bool decrypt_media(const std::string& key, const std::string& ciphertext, std::string& plaintext,
                   std::string* error) {
    plaintext.clear();
    if (key.size() != 16) {
        if (error) *error = "附件密钥格式无法识别";
        return false;
    }
    if (ciphertext.empty()) return true;
    if (ciphertext.size() % 16 != 0) {
        if (error) *error = "附件内容不完整,无法解密";
        return false;
    }
    // ECB 的每个块独立:在末尾补一个“整块填充”的密文块,平台库按严格 PKCS#7 只会去掉这一块,
    // 原文最后的填充留给下面的宽松规则处理(平台偶尔给出不规范的填充,不能因此丢文件)。
    std::string pad_block;
    if (!platform::aes_128_ecb_encrypt(key, std::string{}, pad_block, nullptr) || pad_block.size() != 16) {
        if (error) *error = "附件解密失败";
        return false;
    }
    std::string raw;
    if (!platform::aes_128_ecb_decrypt(key, ciphertext + pad_block, raw, nullptr)) {
        if (error) *error = "附件解密失败";
        return false;
    }
    plaintext = strip_pkcs7_lenient(std::move(raw));
    return true;
}

bool prepare_upload(const std::string& plaintext, PreparedUpload& out, std::string* error) {
    out = PreparedUpload{};
    const auto filekey = platform::secure_random_bytes(16);
    out.aes_key = platform::secure_random_bytes(16);
    const auto digest = platform::md5_digest(plaintext);
    if (filekey.size() != 16 || out.aes_key.size() != 16 || digest.size() != 16) {
        if (error) *error = "无法生成上传所需的随机密钥";
        return false;
    }
    out.filekey = platform::to_hex(filekey);
    out.md5_hex = platform::to_hex(digest);
    out.raw_size = plaintext.size();
    if (!platform::aes_128_ecb_encrypt(out.aes_key, plaintext, out.ciphertext, nullptr) ||
        out.ciphertext.size() != platform::aes_128_ecb_padded_size(plaintext.size())) {
        if (error) *error = "文件加密失败";
        return false;
    }
    return true;
}

nlohmann::json upload_url_request(const PreparedUpload& upload, UploadMediaType type, const std::string& peer) {
    return {{"filekey", upload.filekey},
            {"media_type", static_cast<int>(type)},
            {"to_user_id", peer},
            {"rawsize", upload.raw_size},
            {"rawfilemd5", upload.md5_hex},
            {"filesize", upload.ciphertext.size()},
            {"no_need_thumb", true},
            {"aeskey", platform::to_hex(upload.aes_key)}};
}

std::string upload_target(const nlohmann::json& response, const std::string& cdn_base, const std::string& filekey) {
    if (!response.is_object()) return {};
    const auto full = response.contains("upload_full_url") && response["upload_full_url"].is_string()
                          ? trim(response["upload_full_url"].get<std::string>())
                          : std::string{};
    if (!full.empty()) return full;
    const auto param = response.contains("upload_param") && response["upload_param"].is_string()
                           ? response["upload_param"].get<std::string>()
                           : std::string{};
    if (param.empty()) return {};
    return cdn_upload_url(cdn_base, param, filekey);
}

std::chrono::milliseconds download_timeout(const std::string& kind) {
    if (kind == "image") return std::chrono::seconds(30);
    if (kind == "video") return std::chrono::seconds(120);
    return std::chrono::seconds(60);
}

bool read_whole_file(const std::filesystem::path& path, std::uint64_t limit, std::string& data, std::string* error) {
    data.clear();
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        if (error) *error = "文件不存在或不可读";
        return false;
    }
    if (limit && size > limit) {
        if (error) *error = "文件超过微信 " + size_label(limit) + " 上限";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "文件不存在或不可读";
        return false;
    }
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool download_media(const ApiOptions& options, const MediaRef& ref, const std::filesystem::path& dest,
                    std::uint64_t max_bytes, std::string* error) {
    if (ref.key_invalid) {
        if (error) *error = "附件密钥格式无法识别";
        return false;
    }
    std::string url;
    if (!ref.query_param.empty()) {
        url = cdn_download_url(options.cdn_base, ref.query_param);
    } else if (media_host_allowed(ref.full_url, options.cdn_base)) {
        url = ref.full_url;
    } else {
        if (error) *error = "附件下载地址不是微信的域名,已拒绝";
        return false;
    }
    auto temp = dest;
    temp += ".wxdl";
    HttpRequest request;
    request.url = url;
    request.timeout = download_timeout(ref.kind);
    request.use_proxy = options.use_proxy;
    request.download_to = temp;
    // 密文比明文多出至多 16 字节的填充。
    request.max_download_bytes = max_bytes ? max_bytes + (ref.aes_key.empty() ? 0 : 16) : 0;
    const auto response = http_send(request);
    if (response.too_large) {
        remove_quietly(temp);
        if (error) *error = "文件超过微信 " + size_label(max_bytes) + " 下载上限";
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        remove_quietly(temp);
        if (error) {
            *error = response.status == 0 ? (response.error.empty() ? std::string("下载微信文件失败:无法连接文件服务器")
                                                                    : "下载微信文件失败:" + redact_secrets(response.error))
                                          : "下载微信文件失败:HTTP " + std::to_string(response.status);
        }
        return false;
    }
    std::string data;
    const bool read = read_whole_file(temp, 0, data, error);
    remove_quietly(temp);
    if (!read) return false;
    std::string plaintext;
    if (ref.aes_key.empty()) {
        plaintext = std::move(data);
    } else if (!decrypt_media(ref.aes_key, data, plaintext, error)) {
        return false;
    }
    if (max_bytes && plaintext.size() > max_bytes) {
        if (error) *error = "文件超过微信 " + size_label(max_bytes) + " 下载上限";
        return false;
    }
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(plaintext.data(), static_cast<std::streamsize>(plaintext.size()))) {
        out.close();
        remove_quietly(dest);
        if (error) *error = "无法保存下载的文件";
        return false;
    }
    return true;
}

} // namespace acecode::im::weixin
