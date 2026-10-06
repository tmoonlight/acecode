#pragma once

// 按平台名创建传输层、联网校验凭据、运行扫码绑定。平台清单与凭据字段见 platforms.hpp;
// 这里是唯一需要认识各平台传输层类型的地方。

#include "channels/core/host_services.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace acecode::channels::core {

// 未知平台返回 nullptr。
std::shared_ptr<im::Transport> make_platform_transport(const std::string& platform, const PlatformConfig& config,
                                                       ChannelStore& store, const HostServices& services);

// 成功返回要保存的凭据(去空白,可能补上机器人 id 等派生字段);失败返回 null 并写 error(不含凭据)。
nlohmann::json validate_platform_credentials(const std::string& platform, const nlohmann::json& credentials,
                                             const HostServices& services, std::string* error);

// 扫码绑定(QQ、微信):进度里只有二维码内容与刷新次数;结果给出要保存的凭据与扫码人。
struct BindProgress {
    std::string qr_url;
    int refreshes = 0;
};

struct BindOutcome {
    std::string phase = "failed";  // completed / failed / cancelled / timed_out
    nlohmann::json credentials = nlohmann::json::object();
    std::string owner;        // 扫码人的 principal;空 = 平台没给出
    std::string owner_name;
    std::string account;      // 机器人账号 id,只用于日志与界面
    std::string error;        // 失败原因(不含凭据)
};

using BindRunner = std::function<BindOutcome(const std::atomic<bool>& cancelled,
                                             const std::function<void(const BindProgress&)>& progress)>;

// 没有扫码流程的平台返回空函数。
BindRunner make_bind_runner(const std::string& platform, const HostServices& services);

} // namespace acecode::channels::core
