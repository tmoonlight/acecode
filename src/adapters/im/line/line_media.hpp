#pragma once

// LINE 出站图片的临时公网链接登记表。LINE 机器人只能按公网 HTTPS 地址发图片,
// 传输层把要发的文件登记在这里,经本机 webhook 端口的 /line/media/<令牌>/<文件名> 提供下载:
//   - 令牌 32 个 URL 安全随机字符,链接默认 30 分钟后失效;
//   - 只按令牌精确查找,文件名也必须一致;返回的永远是登记时的路径,URL 里的内容
//     绝不参与拼接路径,不存在目录穿越;
//   - 登记表有条数上限,超出时丢最旧的。

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace acecode::im::line {

inline constexpr const char* kMediaRoutePrefix = "/line/media/";

class MediaRegistry {
public:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        std::filesystem::path path;
        std::string name;       // URL 里的文件名(已清洗)
        std::string mime_type;
        Clock::time_point expires_at;
    };

    explicit MediaRegistry(std::chrono::milliseconds ttl = std::chrono::minutes(30), std::size_t max_entries = 256);

    // 返回 "<令牌>/<文件名>"(拼在 <公网地址>/line/media/ 之后);随机数失败时返回空串。
    std::string add(const std::filesystem::path& path, const std::string& name, const std::string& mime_type,
                    Clock::time_point now = Clock::now());
    // 令牌不存在、已过期或文件名不一致时返回 nullopt。
    std::optional<Entry> find(const std::string& token, const std::string& name, Clock::time_point now = Clock::now());
    // 解析 /line/media/<令牌>/<文件名> 并查找;路径格式不对返回 nullopt。
    std::optional<Entry> find_path(const std::string& url_path, Clock::time_point now = Clock::now());
    std::size_t size() const;

private:
    void prune_locked(Clock::time_point now);

    std::chrono::milliseconds ttl_;
    std::size_t max_entries_;
    mutable std::mutex mu_;
    std::map<std::string, Entry> entries_;
};

} // namespace acecode::im::line
