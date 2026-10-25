#pragma once

// 外部平台插件的传输层(openspec add-channel-platform-plugins D3):
// 插件是一个常驻子进程,stdin/stdout 每行一个 JSON 对象。
//   请求 {"id","method","params"} → 响应 {"id","ok","result"|"error"}
//   事件 {"event":"status"|"inbound","params"}
// 子进程意外退出、initialize 失败或协议帧不合法时按退避自动重启;stop() 先发 shutdown,
// 再关 stdin、限时等待,最后强杀。IM 本身的登录、长连接与防回环都在插件里。

#include "im/transport.hpp"
#include "platform/process/piped_process.hpp"
#include "utils/joining_thread.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace acecode::im::plugin {

inline constexpr int kProtocolVersion = 1;

struct PluginTransportOptions {
    std::string platform;            // 平台名(清单 name)
    std::string command;             // UTF-8 绝对路径
    std::vector<std::string> args;
    std::string cwd;                 // 子进程工作目录(清单目录)
    nlohmann::json settings = nlohmann::json::object();  // initialize 时交给插件
    std::chrono::milliseconds initial_backoff{std::chrono::seconds(1)};
    std::chrono::milliseconds max_backoff{std::chrono::seconds(60)};
    std::chrono::milliseconds initialize_timeout{std::chrono::seconds(30)};
    std::chrono::milliseconds send_timeout{std::chrono::seconds(60)};
    std::chrono::milliseconds file_timeout{std::chrono::seconds(120)};
    std::chrono::milliseconds typing_timeout{std::chrono::seconds(10)};
    std::chrono::milliseconds action_timeout{std::chrono::seconds(120)};
    std::chrono::milliseconds shutdown_timeout{std::chrono::seconds(3)};
};

// 插件状态里的 state 字符串;未知值按 connecting 处理。
LinkState parse_plugin_link_state(const std::string& state);
// 插件 inbound 事件 → 入站消息;address.platform / account 由调用方给出,插件给的值忽略。
// 不合法(缺 address、id 字符集不对、私聊 chat != sender)时返回 false。
bool parse_plugin_inbound(const nlohmann::json& params, const std::string& platform, const std::string& account,
                          Inbound& out);
nlohmann::json attachment_to_plugin_json(const Attachment& attachment);

class PluginTransport final : public Transport {
public:
    static constexpr std::size_t kMaxFrameBytes = 1024 * 1024;
    static constexpr std::size_t kMaxPending = 64;

    explicit PluginTransport(PluginTransportOptions options);
    ~PluginTransport() override;

    std::string platform() const override { return options_.platform; }
    Capabilities capabilities() const override;
    void start(TransportCallbacks callbacks) override;
    // 只作用于下一次 initialize;之后崩溃重启都按自动恢复报告。
    void set_user_initiated(bool user_initiated) override;
    void stop() override;
    TransportStatus status() const override;
    SendResult send_text(const Address& to, const std::string& text, const nlohmann::json& reply_context) override;
    SendResult send_file(const Address& to, const std::filesystem::path& path, const std::string& name,
                         const std::string& mime_type, const nlohmann::json& reply_context) override;
    // 单向通知(不带 id、不等响应):调用方是会话事件线程,不能被插件拖慢。
    void set_typing(const Address& to, bool on) override;
    bool download(const Attachment& attachment, const std::filesystem::path& dest, std::string* error) override;
    // 转发给插件的 action 方法("login" 等);"configure" 额外记下设置,重启子进程时沿用。
    nlohmann::json action(const std::string& name, const nlohmann::json& args) override;

private:
    struct Pending {
        bool done = false;
        nlohmann::json result;
        std::string error;
    };

    void supervise_loop();
    bool run_process_once();
    void read_loop();
    void write_loop();
    void handle_frame(const nlohmann::json& frame);
    void handle_event(const std::string& event, const nlohmann::json& params);
    void dispatch_event(const std::string& event, const nlohmann::json& params);
    void flush_early_events();
    void handle_event_logged(const std::string& event, const nlohmann::json& params);
    void fail_process(const std::string& reason);
    // 失败时抛 std::runtime_error(消息可直接展示)。
    nlohmann::json request(const std::string& method, const nlohmann::json& params, std::chrono::milliseconds timeout);
    void notify(const std::string& method, const nlohmann::json& params);
    SendResult send_result(const nlohmann::json& result) const;
    void set_local_status(LinkState state, const std::string& detail);
    void emit_status();
    bool wait_for(std::chrono::milliseconds duration);

    PluginTransportOptions options_;
    TransportCallbacks callbacks_;
    acecode::JoiningThread supervisor_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};

    std::mutex wake_mu_;
    std::condition_variable wake_;

    // 当前子进程的 IO 状态;下一个子进程开始前清空。
    mutable std::mutex io_mu_;
    std::condition_variable io_cv_;
    platform::PipedProcess process_;
    acecode::JoiningThread reader_, writer_;
    bool alive_ = false;
    bool initialized_ = false;
    std::string failure_;
    std::uint64_t next_id_ = 1;
    std::map<std::uint64_t, std::shared_ptr<Pending>> pending_;
    std::deque<std::string> writes_;
    std::vector<std::pair<std::string, nlohmann::json>> early_events_;  // 初始化完成前到达的事件
    std::mutex event_mu_;  // 串行化事件处理(reader 与初始化后的补发)

    mutable std::mutex mu_;  // status_、capabilities_、settings_、user_initiated_
    TransportStatus status_;
    Capabilities capabilities_;
    nlohmann::json settings_;
    bool user_initiated_ = false;
};

} // namespace acecode::im::plugin
