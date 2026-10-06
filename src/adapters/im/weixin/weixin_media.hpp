#pragma once

// 微信 iLink 媒体:CDN 上的文件内容一律 AES-128-ECB + PKCS#7 加密。
//
//   入站:按 encrypt_query_param(或白名单内的 full_url)下载密文 → 用消息里给的密钥解密;
//         去填充采用宽松规则(填充不合法时原样保留,与 hermes / 官方插件一致)。
//   出站:随机 16 字节密钥与 filekey → getuploadurl 拿上传参数 → POST 密文到 CDN →
//         响应头 x-encrypted-param 作为该文件的 encrypt_query_param 发出。
// 密钥、文件内容都不写日志。

#include "im/weixin/weixin_api.hpp"
#include "im/weixin/weixin_protocol.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace acecode::im::weixin {

// 解密 CDN 下载的密文。密文长度必须是 16 的倍数;空密文得到空明文。
bool decrypt_media(const std::string& key, const std::string& ciphertext, std::string& plaintext,
                   std::string* error);

struct PreparedUpload {
    std::string filekey;      // 32 个小写十六进制字符
    std::string aes_key;      // 16 字节原始密钥
    std::string md5_hex;      // 明文 MD5(小写十六进制)
    std::uint64_t raw_size = 0;
    std::string ciphertext;   // 长度 = PKCS#7 填充后的长度
};

// 生成一次性密钥与 filekey 并加密内容。
bool prepare_upload(const std::string& plaintext, PreparedUpload& out, std::string* error);

// getuploadurl 的请求体(base_info 由 Api 补上)。
nlohmann::json upload_url_request(const PreparedUpload& upload, UploadMediaType type, const std::string& peer);
// 从 getuploadurl 的响应确定上传地址:优先 upload_full_url,否则用 upload_param 拼 CDN 地址;
// 两者都没有时返回空串。
std::string upload_target(const nlohmann::json& response, const std::string& cdn_base, const std::string& filekey);

// 各类媒体的下载超时(图片 30 秒,文件/语音 60 秒,视频 120 秒)。
std::chrono::milliseconds download_timeout(const std::string& kind);

// 下载并解密到 dest;明文超过 max_bytes 时失败并删除半截文件。error 为中文一句话。
bool download_media(const ApiOptions& options, const MediaRef& ref, const std::filesystem::path& dest,
                    std::uint64_t max_bytes, std::string* error);

// 给用户看的大小:不足 1 MB 时按 KB 显示(如 "50 MB"、"512 KB")。
std::string size_label(std::uint64_t bytes);

// 整读文件;超过 limit 时失败(limit 为 0 表示不限)。
bool read_whole_file(const std::filesystem::path& path, std::uint64_t limit, std::string& data, std::string* error);

} // namespace acecode::im::weixin
