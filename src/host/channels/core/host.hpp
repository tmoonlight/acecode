#pragma once

// 消息通道宿主(design D4 / D9 / D12):在 daemon 里托管各平台运行时(清单见 platforms.hpp),
// 承接设置页的全部操作,运行扫码绑定流程(QQ、微信),并负责跨平台的会话绑定转移。
// 每个平台的数据在 <root>/<platform>/ 下(root 通常是 ~/.acecode/channels)。

#include "channels/core/host_services.hpp"
#include "channels/core/platform_adapters.hpp"
#include "channels/core/platforms.hpp"
#include "utils/joining_thread.hpp"
#include "utils/lifetime_token.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace acecode::channels::core {

class ChannelHost {
public:
    ChannelHost(std::filesystem::path root, HostServices services);
    ~ChannelHost();
    ChannelHost(const ChannelHost&) = delete;
    ChannelHost& operator=(const ChannelHost&) = delete;

    void start();
    void stop();

    // {"platforms":[平台快照...], "binds":{"qq":{扫码状态}, "weixin":{...}}}
    nlohmann::json snapshot() const;
    bool has_platform(const std::string& name) const;
    // 未知平台抛 std::invalid_argument。
    PlatformRuntime& platform(const std::string& name);

    // 扫码绑定(QQ、微信):同一时间只允许一个流程(重复发起抛 std::runtime_error,
    // 不支持扫码的平台抛 std::invalid_argument);二维码刷新与结果经 broadcast("channels_bind")
    // 推送,同时可用 bind_state() 查询。成功后保存凭据、设定机主(拿不到扫码人时打开 10 分钟窗口)
    // 并开启该平台。
    nlohmann::json start_bind(const std::string& platform);
    void cancel_bind(const std::string& platform);
    nlohmann::json bind_state(const std::string& platform) const;

    // 机主绑定:Telegram 返回一次性链接 {"link","expires_in_s"};飞书、钉钉、Discord、LINE 返回
    // 6 位绑定码 {"code","expires_in_s"},机主私聊机器人发送它即可。需要已连接且尚无机主。
    nlohmann::json owner_link(const std::string& platform);

    // 会话 id → 平台名;会话列表的 channel_bound 字段用它。
    std::map<std::string, std::string> bound_sessions() const;

private:
    void release_across(const std::string& session_id, const std::string& taker_key, const std::string& label);
    void publish_all();
    void publish_bind(const nlohmann::json& state);
    void run_bind_flow(const std::string& platform, const BindRunner& runner,
                       const std::shared_ptr<std::atomic<bool>>& cancelled);
    void finish_bind(const std::string& platform, const BindOutcome& outcome,
                     const std::shared_ptr<std::atomic<bool>>& cancelled);

    std::filesystem::path root_;
    HostServices services_;
    std::map<std::string, std::unique_ptr<PlatformRuntime>> runtimes_;

    mutable std::mutex bind_mu_;
    nlohmann::json bind_state_ = {{"phase", "idle"}};  // 最近一次扫码流程的状态(带 platform)
    std::shared_ptr<std::atomic<bool>> bind_cancel_;
    acecode::JoiningThread bind_thread_;
    bool bind_running_ = false;
    // 平台运行时与扫码线程经它回调宿主;最后声明,析构时最先撤销(此时 stop() 已停掉它们)。
    LifetimeToken lifetime_;
};

} // namespace acecode::channels::core
