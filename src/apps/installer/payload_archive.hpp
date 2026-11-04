#pragma once

// 安装包内容:安装程序资源区里的 zip(与 CI 产出的 acecode-windows-<arch>.zip 相同)、
// 卸载程序副本,以及描述两者的 JSON。这里负责解析描述、校验哈希、把 zip 安全地解到
// 目标目录;读取资源区在 install_engine 里。

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace acecode::installer {

inline constexpr const char* kPayloadResourceName = "ACECODE_PAYLOAD";
inline constexpr const char* kPayloadInfoResourceName = "ACECODE_PAYLOAD_INFO";
inline constexpr const char* kUninstallerResourceName = "ACECODE_UNINSTALLER";

struct PayloadInfo {
    int schema = 1;
    std::string version;
    std::string target;
    std::string payload_sha256;
    std::uint64_t payload_size = 0;
    std::string uninstaller_sha256;
};

std::optional<PayloadInfo> parse_payload_info(const std::string& json, std::string* error);
std::string serialize_payload_info(const PayloadInfo& info);

// 与 make_setup.py 的检查一致:zip 内路径只能是相对路径,不能有 ..、盘符、冒号。
bool is_safe_archive_entry(const std::string& name);

std::string sha256_of(std::string_view bytes);

class ExtractObserver {
public:
    virtual ~ExtractObserver() = default;
    virtual void on_extract_progress(std::uint64_t done_bytes, std::uint64_t total_bytes) = 0;
};

struct ExtractResult {
    bool ok = false;
    std::uint64_t bytes = 0;
    std::size_t files = 0;
    std::string error;
};

ExtractResult extract_archive(std::string_view zip_bytes,
                              const std::filesystem::path& destination,
                              ExtractObserver* observer);

}  // namespace acecode::installer
