#include "host.hpp"

#include "utils/logger.hpp"

#include <stdexcept>

namespace acecode::channels::core {

ChannelHost::ChannelHost(std::filesystem::path root, HostServices services)
    : root_(std::move(root)), services_(std::move(services)) {
    for (const auto& name : platform_names()) {
        RuntimeServices runtime;
        runtime.conversation = services_.conversation;
        runtime.conversation.media_dir.clear();  // 由运行时放到 <root>/<platform>/media
        const auto ref = lifetime_.ref(*this);
        runtime.conversation.release_session = [ref](const std::string& session_id, const std::string& taker_key,
                                                     const std::string& label) {
            ref.with([&session_id, &taker_key, &label](ChannelHost& host) {
                host.release_across(session_id, taker_key, label);
            });
        };
        runtime.conversation.on_bindings_changed = [ref] { ref.with([](ChannelHost& host) { host.publish_all(); }); };
        if (services_.make_transport) {
            runtime.make_transport = services_.make_transport;
        } else {
            runtime.make_transport = [ref](const std::string& platform, const PlatformConfig& config,
                                           ChannelStore& store) {
                std::shared_ptr<im::Transport> transport;
                ref.with([&transport, &platform, &config, &store](ChannelHost& host) {
                    transport = make_platform_transport(platform, config, store, host.services_);
                });
                return transport;
            };
        }
        if (services_.validate_credentials) {
            runtime.validate_credentials = services_.validate_credentials;
        } else {
            runtime.validate_credentials = [ref](const std::string& platform, const nlohmann::json& credentials,
                                                 std::string* error) {
                nlohmann::json result;
                ref.with([&result, &platform, &credentials, error](ChannelHost& host) {
                    result = validate_platform_credentials(platform, credentials, host.services_, error);
                });
                return result;
            };
        }
        runtime.broadcast = services_.broadcast;
        runtime.current_pid = services_.current_pid;
        runtime.standby_retry = services_.standby_retry;
        runtimes_[name] = std::make_unique<PlatformRuntime>(name, root_ / name, std::move(runtime));
    }
}

ChannelHost::~ChannelHost() { stop(); }

void ChannelHost::start() {
    for (const auto& name : platform_names()) runtimes_.at(name)->start();
}

void ChannelHost::stop() {
    acecode::JoiningThread thread;
    {
        std::lock_guard<std::mutex> lock(bind_mu_);
        if (bind_cancel_) bind_cancel_->store(true);
        thread = std::move(bind_thread_);
    }
    if (thread.joinable()) thread.join();
    for (const auto& name : platform_names()) runtimes_.at(name)->stop();
}

bool ChannelHost::has_platform(const std::string& name) const { return runtimes_.count(name) > 0; }

PlatformRuntime& ChannelHost::platform(const std::string& name) {
    const auto it = runtimes_.find(name);
    if (it == runtimes_.end()) throw std::invalid_argument("未知的消息通道平台");
    return *it->second;
}

nlohmann::json ChannelHost::snapshot() const {
    nlohmann::json platforms = nlohmann::json::array();
    nlohmann::json binds = nlohmann::json::object();
    for (const auto& spec : platform_specs()) {
        platforms.push_back(runtimes_.at(spec.name)->snapshot());
        if (spec.scan_bind) binds[spec.name] = bind_state(spec.name);
    }
    return {{"platforms", platforms}, {"binds", binds}};
}

void ChannelHost::publish_all() {
    for (const auto& name : platform_names()) runtimes_.at(name)->publish_state();
}

void ChannelHost::publish_bind(const nlohmann::json& state) {
    if (services_.broadcast) services_.broadcast("channels_bind", state);
}

nlohmann::json ChannelHost::start_bind(const std::string& platform) {
    const auto* spec = find_platform_spec(platform);
    auto runner = spec && spec->scan_bind ? make_bind_runner(platform, services_) : BindRunner{};
    if (!runner) throw std::invalid_argument("该平台不支持扫码绑定");
    nlohmann::json state;
    {
        std::lock_guard<std::mutex> lock(bind_mu_);
        if (bind_running_) throw std::runtime_error("已有扫码流程在进行,请先完成或取消");
        if (bind_thread_.joinable()) bind_thread_.join();  // 上一次已结束的流程
        bind_running_ = true;
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        bind_cancel_ = cancelled;
        bind_state_ = {{"platform", platform}, {"phase", "starting"}};
        state = bind_state_;
        bind_thread_ = acecode::JoiningThread([ref = lifetime_.ref(*this), platform, runner, cancelled] {
            ref.with([&platform, &runner, &cancelled](ChannelHost& host) {
                host.run_bind_flow(platform, runner, cancelled);
            });
        });
    }
    LOG_INFO("[channels/" + platform + "] QR binding started");
    publish_bind(state);
    return state;
}

// 在扫码线程上运行;runner 在返回前同步回调 progress。
void ChannelHost::run_bind_flow(const std::string& platform, const BindRunner& runner,
                                const std::shared_ptr<std::atomic<bool>>& cancelled) {
    const auto outcome = runner(
        *cancelled, [&mu = bind_mu_, &state = bind_state_, &broadcast = services_.broadcast, &platform,
                     cancelled](const BindProgress& update) {
            if (cancelled->load()) return;
            // 二维码内容不写日志(spec:扫码载荷保密),只记进度。
            LOG_INFO(update.refreshes > 0 ? "[channels/" + platform + "] QR code expired and was refreshed (" +
                                                std::to_string(update.refreshes) + ")"
                                          : "[channels/" + platform + "] QR code ready, waiting for the scan");
            nlohmann::json waiting{{"platform", platform},
                                   {"phase", "waiting"},
                                   {"qr_url", update.qr_url},
                                   {"refreshes", update.refreshes}};
            {
                std::lock_guard<std::mutex> lock(mu);
                state = waiting;
            }
            if (broadcast) broadcast("channels_bind", waiting);
        });
    finish_bind(platform, outcome, cancelled);
}

void ChannelHost::finish_bind(const std::string& platform, const BindOutcome& outcome,
                              const std::shared_ptr<std::atomic<bool>>& cancelled) {
    nlohmann::json state{{"platform", platform}, {"phase", outcome.phase}};
    if (outcome.phase == "completed" && cancelled->load()) {
        state["phase"] = "cancelled";  // 取消与完成同时发生:按取消处理,不改配置
    } else if (outcome.phase == "completed") {
        try {
            auto& runtime = this->platform(platform);
            runtime.set_credentials(outcome.credentials, false);
            if (!outcome.owner.empty()) runtime.set_owner(outcome.owner, outcome.owner_name);
            else if (runtime.owner().empty()) runtime.open_owner_window();
            runtime.set_enabled(true);
            state["account"] = outcome.account;
            state["owner_bound"] = !outcome.owner.empty();
        } catch (const std::exception& e) {
            state = {{"platform", platform}, {"phase", "failed"}, {"error", std::string("保存配置失败:") + e.what()}};
        }
    } else if (outcome.phase == "failed") {
        state["error"] = outcome.error.empty() ? std::string("扫码绑定失败") : outcome.error;
    } else if (outcome.phase == "timed_out") {
        state["error"] = "扫码超时,请重新发起";
    }
    const auto phase = state.value("phase", std::string{});
    if (phase == "completed") {
        LOG_INFO("[channels/" + platform + "] QR binding completed, account " + outcome.account +
                 (state.value("owner_bound", false) ? ", the scanner is the owner" : ", owner window opened"));
    } else if (phase == "cancelled") {
        LOG_INFO("[channels/" + platform + "] QR binding cancelled, configuration unchanged");
    } else {
        LOG_WARN("[channels/" + platform + "] QR binding " + phase + ": " + state.value("error", std::string{}));
    }
    {
        std::lock_guard<std::mutex> lock(bind_mu_);
        bind_state_ = state;
        bind_running_ = false;
    }
    publish_bind(state);
}

void ChannelHost::cancel_bind(const std::string& platform) {
    std::lock_guard<std::mutex> lock(bind_mu_);
    if (bind_cancel_ && bind_state_.value("platform", std::string{}) == platform) bind_cancel_->store(true);
}

nlohmann::json ChannelHost::bind_state(const std::string& platform) const {
    std::lock_guard<std::mutex> lock(bind_mu_);
    if (bind_state_.value("platform", std::string{}) == platform) return bind_state_;
    return {{"platform", platform}, {"phase", "idle"}};
}

nlohmann::json ChannelHost::owner_link(const std::string& name) {
    const auto* spec = find_platform_spec(name);
    if (!spec) throw std::invalid_argument("未知的消息通道平台");
    auto& runtime = platform(name);
    const auto status = runtime.transport_status();
    const auto ttl = std::chrono::duration_cast<std::chrono::seconds>(AccessControl::kOwnerTtl).count();
    if (spec->owner_binding == OwnerBinding::Link) {
        const auto username =
            status.extra.is_object() ? status.extra.value("username", std::string{}) : std::string{};
        if (status.state != im::LinkState::Connected || username.empty())
            throw std::runtime_error(spec->label + " 尚未连接,连接成功后才能生成机主绑定链接");
        const auto code = runtime.issue_owner_code();
        return {{"link", "https://t.me/" + username + "?start=" + code}, {"expires_in_s", ttl}};
    }
    if (spec->owner_binding == OwnerBinding::Code) {
        if (status.state != im::LinkState::Connected)
            throw std::runtime_error(spec->label + " 尚未连接,连接成功后才能生成绑定码");
        return {{"code", runtime.issue_owner_pin()}, {"expires_in_s", ttl}};
    }
    throw std::runtime_error(spec->label + " 在扫码时自动设定机主");
}

std::map<std::string, std::string> ChannelHost::bound_sessions() const {
    std::map<std::string, std::string> out;
    for (const auto& name : platform_names())
        for (const auto& id : runtimes_.at(name)->bound_session_ids()) out[id] = name;
    return out;
}

void ChannelHost::release_across(const std::string& session_id, const std::string& taker_key,
                                 const std::string& label) {
    for (const auto& name : platform_names()) runtimes_.at(name)->release_session(session_id, taker_key, label);
}

} // namespace acecode::channels::core
