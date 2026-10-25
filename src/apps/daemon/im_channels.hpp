#pragma once

// daemon 托管的消息通道(QQ、微信、飞书、钉钉、Telegram、Discord、LINE,openspec add-desktop-im-channels design D2 / D12):
// 把 SessionRegistry、会话目录、模型切换、技能展开接到通道宿主上。
// WS 推送经 broadcast 回调发出;宿主先于 WebServer 构造,调用方在 WebServer 建好后再接上。

#include "channels/core/host.hpp"
#include "remote_control/rc_session_navigation.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace acecode {
struct AppConfig;
class SessionClient;
class SessionRegistry;
} // namespace acecode

namespace acecode::daemon {

struct ImChannelDeps {
    SessionRegistry* registry = nullptr;
    SessionClient* client = nullptr;
    const AppConfig* config = nullptr;  // 读 saved_models;持 config_mu 共享锁访问
    std::shared_mutex* config_mu = nullptr;
    std::function<std::vector<rc::RcSessionTarget>(const std::optional<std::string>&)> catalog;
    channels::core::Broadcast broadcast;
    // 以下只有端到端测试会改:平台接入地址、扫码门户与数据根目录(为空时用 im_channels_root())。
    channels::core::HostEndpoints endpoints;
    im::qqbot::BindOptions bind;
    im::weixin::LoginOptions weixin_login;
    std::filesystem::path root;
    // 测试用:在宿主创建前调整服务(传输层参数、LINE 的隧道与回调端口等)。
    std::function<void(channels::core::HostServices&)> tune_services;
};

// 通道数据根目录 <data_dir>/channels,与 WhatsApp 的 channels/whatsapp 并列。
std::filesystem::path im_channels_root();

std::unique_ptr<channels::core::ChannelHost> make_im_channel_host(ImChannelDeps deps);

// 外部平台插件清单所在的连接器目录 <data_dir>/connectors(openspec add-channel-platform-plugins D1)。
std::filesystem::path im_channel_plugins_root();
// 扫描清单并注册插件平台,必须在 make_im_channel_host 之前调用;返回注册成功的平台名。
std::vector<std::string> register_im_channel_plugins(const std::filesystem::path& connectors_root);

} // namespace acecode::daemon
