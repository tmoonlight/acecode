#include "platform_runtime.hpp"

#include "channels/core/platforms.hpp"
#include "channels/core/state_log.hpp"
#include "channels/owner_lock.hpp"
#include "im/redact.hpp"
#include "utils/atomic_file.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace acecode::channels::core {
namespace {

std::int64_t read_holder_pid(const std::filesystem::path& directory) {
    try {
        std::ifstream in(directory / "owner.json", std::ios::binary);
        if (!in) return 0;
        const auto json = nlohmann::json::parse(in);
        return json.value("pid", std::int64_t{0});
    } catch (...) {
        return 0;
    }
}

} // namespace

struct PlatformRuntime::LockHandle {
    OwnerLock lock;
};

PlatformRuntime::PlatformRuntime(std::string platform, std::filesystem::path directory, RuntimeServices services)
    : platform_(std::move(platform)), directory_(std::move(directory)), services_(std::move(services)),
      store_(directory_) {
    auto deps = services_.conversation;
    if (deps.media_dir.empty()) deps.media_dir = directory_ / "media";
    conversations_ = std::make_unique<Conversations>(platform_, store_, access_, slot_, std::move(deps));
}

PlatformRuntime::~PlatformRuntime() { stop(); }

bool PlatformRuntime::configured(const PlatformConfig& config) const {
    const auto* spec = find_platform_spec(platform_);
    return spec && credentials_complete(*spec, config.credentials);
}

// 绑定地址里的账号(平台描述表定义取哪个凭据字段;Telegram 取 token 冒号前一段)。
std::string PlatformRuntime::account_of(const PlatformConfig& config) const {
    const auto* spec = find_platform_spec(platform_);
    return spec ? core::account_of(*spec, config.credentials) : std::string{};
}

void PlatformRuntime::forget_account_locked(const std::string& account) {
    for (const auto& record : store_.bindings())
        if (record.address.account == account) store_.erase_binding(record.address.key());
    store_.clear_transport_state();  // 游标、上下文令牌都属于旧机器人
    const auto* spec = find_platform_spec(platform_);
    if (spec && spec->principals_scoped_to_account) {
        store_.update_config([](PlatformConfig& config) {
            config.owner.clear();
            config.access.clear();
        });
    }
}

void PlatformRuntime::ensure_loaded() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!running_) throw std::runtime_error("消息通道尚未启动");
    if (!load_error_.empty()) throw std::runtime_error(load_error_);
}

void PlatformRuntime::start() {
    {
        std::lock_guard<std::mutex> ops(ops_mu_);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (running_) return;
            running_ = true;
            load_error_.clear();
        }
        stopping_ = false;
        try {
            store_.load();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mu_);
            load_error_ = e.what();
            LOG_ERROR("[channels/" + platform_ + "] " + load_error_);
        }
        worker_ = acecode::JoiningThread(&PlatformRuntime::worker_loop, this);
        bool load_failed;
        {
            std::lock_guard<std::mutex> lock(mu_);
            load_failed = !load_error_.empty();
        }
        const auto config = store_.config();
        if (!load_failed && config.enabled && configured(config)) {
            try {
                connect_locked();
            } catch (const std::exception& e) {
                LOG_ERROR("[channels/" + platform_ + "] connect failed: " + e.what());
            }
        }
    }
    publish_state();
}

void PlatformRuntime::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_) return;
    }
    stopping_ = true;
    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> ops(ops_mu_);
    disconnect_locked();
    std::lock_guard<std::mutex> lock(mu_);
    running_ = false;
}

void PlatformRuntime::connect_locked() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (transport_) return;
    }
    if (!owns_lock_) {
        std::error_code ec;
        std::filesystem::create_directories(directory_, ec);
        if (!lock_) lock_ = std::make_unique<LockHandle>();
        if (!lock_->lock.acquire(directory_ / "owner.lock")) {
            std::lock_guard<std::mutex> lock(mu_);
            standby_ = true;
            holder_pid_ = read_holder_pid(directory_);
            return;
        }
        owns_lock_ = true;
        const std::int64_t pid = services_.current_pid ? services_.current_pid() : 0;
        atomic_write_file(path_to_utf8(directory_ / "owner.json"), nlohmann::json{{"pid", pid}}.dump(), true);
    }
    const auto config = store_.config();
    auto transport = services_.make_transport ? services_.make_transport(platform_, config, store_) : nullptr;
    if (!transport) throw std::runtime_error("无法创建通道连接");
    {
        std::lock_guard<std::mutex> lock(mu_);
        standby_ = false;
        holder_pid_ = 0;
        transport_ = transport;
    }
    slot_->set(transport);
    std::weak_ptr<im::Transport> weak = transport;
    im::TransportCallbacks callbacks;
    callbacks.on_inbound = [ref = lifetime_.ref(*this)](im::Inbound inbound) {
        ref.with([&inbound](PlatformRuntime& runtime) { runtime.on_inbound(std::move(inbound)); });
    };
    callbacks.on_status = [ref = lifetime_.ref(*this), weak](const im::TransportStatus&) {
        ref.with([&weak](PlatformRuntime& runtime) { runtime.on_transport_status(weak); });
    };
    transport->start(std::move(callbacks));
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        restore_pending_ = true;
    }
    queue_cv_.notify_all();
}

void PlatformRuntime::on_transport_status(const std::weak_ptr<im::Transport>& source) {
    {
        const auto current = source.lock();
        std::lock_guard<std::mutex> lock(mu_);
        if (!current || current != transport_) return;
    }
    publish_state();
}

void PlatformRuntime::disconnect_locked() {
    std::shared_ptr<im::Transport> transport;
    {
        std::lock_guard<std::mutex> lock(mu_);
        transport = std::move(transport_);
        transport_.reset();
        standby_ = false;
        holder_pid_ = 0;
    }
    slot_->set(nullptr);
    if (transport) transport->stop();  // 不持 mu_:传输层线程的回调需要它
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        queue_.clear();
        restore_pending_ = false;
    }
    {
        std::lock_guard<std::mutex> process(process_mu_);
        conversations_->shutdown();
    }
    if (owns_lock_) {
        std::error_code ec;
        std::filesystem::remove(directory_ / "owner.json", ec);
        lock_->lock.release();
        owns_lock_ = false;
    }
}

void PlatformRuntime::on_inbound(im::Inbound inbound) {
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        if (queue_.size() >= kMaxQueue) {
            LOG_WARN("[channels/" + platform_ + "] inbound queue full, dropping message");
            return;
        }
        queue_.push_back(std::move(inbound));
    }
    queue_cv_.notify_all();
}

void PlatformRuntime::worker_loop() {
    while (true) {
        std::unique_lock<std::mutex> lock(queue_mu_);
        queue_cv_.wait_for(lock, services_.standby_retry,
                           [this] { return stopping_.load() || !queue_.empty() || restore_pending_; });
        if (stopping_) break;
        if (restore_pending_) {
            restore_pending_ = false;
            lock.unlock();
            std::lock_guard<std::mutex> process(process_mu_);
            if (slot_->get()) conversations_->restore();
            continue;
        }
        if (queue_.empty()) {
            lock.unlock();
            try_takeover();
            continue;
        }
        auto inbound = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        HandleOutcome outcome;
        {
            std::lock_guard<std::mutex> process(process_mu_);
            if (!slot_->get()) continue;  // 处理前已被关闭
            try {
                outcome = conversations_->handle(inbound);
            } catch (const std::exception& e) {
                LOG_ERROR("[channels/" + platform_ + "] inbound failed: " + e.what());
                continue;
            }
        }
        if (outcome.pending_created && services_.broadcast) {
            const auto& request = *outcome.pending_created;
            services_.broadcast("channels_request", {{"platform", platform_},
                                                     {"id", request.id},
                                                     {"kind", request.kind},
                                                     {"name", request.name},
                                                     {"label", Conversations::label_for(request.address)}});
        }
        if (outcome.pending_created || outcome.owner_claimed) publish_state();
    }
}

// 待命时由工作线程空闲轮询:持有者退出后接管连接。
void PlatformRuntime::try_takeover() {
    std::unique_lock<std::mutex> ops(ops_mu_, std::try_to_lock);
    if (!ops) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!standby_) return;
    }
    if (!store_.config().enabled) return;
    bool connected = false;
    try {
        connect_locked();
        std::lock_guard<std::mutex> lock(mu_);
        connected = transport_ != nullptr;
    } catch (const std::exception& e) {
        LOG_ERROR("[channels/" + platform_ + "] takeover failed: " + e.what());
    }
    ops.unlock();
    if (connected) publish_state();
}

void PlatformRuntime::set_enabled(bool enabled) {
    {
        std::lock_guard<std::mutex> ops(ops_mu_);
        ensure_loaded();
        if (enabled) {
            if (!configured(store_.config())) throw std::runtime_error("请先完成配置");
            store_.update_config([](PlatformConfig& config) { config.enabled = true; });
            // 鉴权失败等致命错误后已停止重试:再次打开开关即重新连接。
            const auto current = slot_->get();
            if (current && current->status().retry_stopped) disconnect_locked();
            connect_locked();
        } else {
            store_.update_config([](PlatformConfig& config) { config.enabled = false; });
            disconnect_locked();
        }
    }
    publish_state();
}

void PlatformRuntime::set_credentials(const nlohmann::json& credentials, bool validate) {
    {
        std::lock_guard<std::mutex> ops(ops_mu_);
        ensure_loaded();
        const auto previous = store_.config();
        nlohmann::json merged = previous.credentials.is_object() ? previous.credentials : nlohmann::json::object();
        if (credentials.is_object()) {
            const auto* spec = find_platform_spec(platform_);
            for (const auto& [key, value] : credentials.items()) {
                if (!value.is_string()) continue;
                if (!value.get<std::string>().empty()) {
                    merged[key] = value;
                    continue;
                }
                // 空串:可选字段表示清除(如 LINE 的公网地址改回自动隧道),必填字段沿用已保存的值。
                if (!spec) continue;
                for (const auto& field : spec->fields)
                    if (field.key == key && !field.required) merged.erase(key);
            }
        }
        nlohmann::json normalized = merged;
        if (validate) {
            std::string error;
            normalized = services_.validate_credentials
                ? services_.validate_credentials(platform_, merged, &error)
                : nlohmann::json(nullptr);
            if (!normalized.is_object()) throw std::runtime_error(error.empty() ? "凭据校验失败" : error);
            // 校验只规整它认识的字段;用户填写的其它可选设置(飞书域名、LINE 公网地址…)原样保留。
            if (const auto* spec = find_platform_spec(platform_)) {
                for (const auto& field : spec->fields)
                    if (field.user_input && !normalized.contains(field.key) && merged.contains(field.key))
                        normalized[field.key] = merged[field.key];
            }
        }
        PlatformConfig next = previous;
        next.credentials = normalized;
        if (!configured(next)) throw std::runtime_error("凭据不完整");
        disconnect_locked();
        store_.update_config([&normalized](PlatformConfig& config) { config.credentials = normalized; });
        const auto old_account = account_of(previous);
        if (!old_account.empty() && old_account != account_of(next)) forget_account_locked(old_account);
        if (store_.config().enabled) connect_locked();  // 按开关状态用新凭据连接
    }
    publish_state();
}

void PlatformRuntime::approve(const std::string& request_id, bool approve) {
    ensure_loaded();
    const auto request = access_.take(request_id, AccessControl::Clock::now());
    if (!request) throw std::runtime_error("该请求已过期或不存在");
    if (approve) {
        store_.update_config([&request](PlatformConfig& config) {
            if (!config.has_access(request->principal))
                config.access.push_back({request->principal, request->name, now_wall_ms()});
            if (request->kind == "user" && config.owner.empty()) config.owner = request->principal;
        });
    }
    publish_state();
}

void PlatformRuntime::revoke(const std::string& principal) {
    ensure_loaded();
    store_.update_config([&principal](PlatformConfig& config) {
        config.access.erase(std::remove_if(config.access.begin(), config.access.end(),
                                           [&principal](const AccessEntry& entry) { return entry.principal == principal; }),
                            config.access.end());
        if (config.owner == principal) config.owner.clear();
    });
    access_.drop_principal(principal);
    conversations_->abort_principal(principal);
    publish_state();
}

std::string PlatformRuntime::issue_owner_code() {
    ensure_loaded();
    if (!store_.config().owner.empty())
        throw std::runtime_error("已经有机主了;如需更换,先在授权名单里移除当前机主");
    const auto code = access_.issue_owner_code(AccessControl::Clock::now());
    if (code.empty()) throw std::runtime_error("无法生成绑定码");
    return code;
}

std::string PlatformRuntime::issue_owner_pin() {
    ensure_loaded();
    if (!store_.config().owner.empty())
        throw std::runtime_error("已经有机主了;如需更换,先在授权名单里移除当前机主");
    const auto code = access_.issue_owner_pin(AccessControl::Clock::now());
    if (code.empty()) throw std::runtime_error("无法生成绑定码");
    return code;
}

void PlatformRuntime::open_owner_window() {
    access_.open_owner_window(AccessControl::Clock::now());
    publish_state();
}

void PlatformRuntime::set_owner(const std::string& principal, const std::string& name) {
    ensure_loaded();
    store_.update_config([&principal, &name](PlatformConfig& config) {
        config.owner = principal;
        if (!config.has_access(principal)) config.access.push_back({principal, name, now_wall_ms()});
    });
    publish_state();
}

nlohmann::json PlatformRuntime::action(const std::string& name, const nlohmann::json& args) {
    const auto transport = slot_->get();
    if (!transport) throw std::runtime_error("通道未连接");
    auto result = transport->action(name, args);
    publish_state();
    return result;
}

im::TransportStatus PlatformRuntime::transport_status() const {
    const auto transport = slot_->get();
    return transport ? transport->status() : im::TransportStatus{};
}

bool PlatformRuntime::release_session(const std::string& session_id, const std::string& except_key,
                                      const std::string& label) {
    return conversations_->release_session(session_id, except_key, label);
}

std::vector<std::string> PlatformRuntime::bound_session_ids() const { return conversations_->bound_session_ids(); }

void PlatformRuntime::publish_state() {
    nlohmann::json state;
    {
        // 快照在 log_mu_ 下取:并发的两次发布按先后比较,日志不会把状态写倒。
        // 调用方不持 mu_ 与传输层的锁,这里再取 mu_ 不会互锁。
        std::lock_guard<std::mutex> lock(log_mu_);
        state = snapshot();
        for (const auto& line : describe_state_changes(logged_state_, state))
            Logger::instance().log(line.level, __FILE__, __LINE__, "[channels/" + platform_ + "] " + line.text);
        logged_state_ = state;
    }
    if (services_.broadcast) services_.broadcast("channels_state", state);
}

nlohmann::json PlatformRuntime::snapshot() const {
    const auto config = store_.config();
    const bool ready = configured(config);
    nlohmann::json out{{"platform", platform_}, {"configured", ready}, {"enabled", config.enabled}};
    std::shared_ptr<im::Transport> transport;
    std::string load_error;
    bool standby;
    std::int64_t holder_pid;
    {
        std::lock_guard<std::mutex> lock(mu_);
        transport = transport_;
        load_error = load_error_;
        standby = standby_;
        holder_pid = holder_pid_;
    }
    // 传输层在自己的锁外回调 on_status,这里在本对象锁外读取它的状态,不会互锁。
    const auto status = transport ? transport->status() : im::TransportStatus{};
    std::string state;
    if (!load_error.empty()) state = "error";
    else if (!config.enabled) state = ready ? "disabled" : "unconfigured";
    else if (standby) state = "standby";
    else if (!transport) state = ready ? "stopped" : "unconfigured";
    else if (status.state == im::LinkState::Stopped) state = "connecting";
    else state = im::link_state_name(status.state);
    out["state"] = state;
    out["detail"] = load_error.empty() ? status.detail : load_error;
    out["retry_stopped"] = status.retry_stopped;
    out["account"] = status.account;
    out["display_name"] = status.display_name;
    out["extra"] = status.extra.is_object() ? status.extra : nlohmann::json::object();
    if (standby && holder_pid > 0) out["hosted_by_pid"] = holder_pid;
    // 凭据:非密钥字段原样给出(AppID、机器人 id 等),密钥只给尾号;credential_hint 是第一个密钥的尾号。
    nlohmann::json public_fields = nlohmann::json::object();
    if (const auto* spec = find_platform_spec(platform_)) {
        for (const auto& field : spec->fields) {
            const auto value = config.credentials.is_object() ? config.credentials.value(field.key, std::string{})
                                                              : std::string{};
            if (value.empty()) continue;
            if (!field.secret) {
                public_fields[field.key] = value;
                continue;
            }
            const auto masked = im::mask_secret(value);
            public_fields[field.key] = masked;
            if (!out.contains("credential_hint")) out["credential_hint"] = masked;
        }
    }
    out["credentials_public"] = public_fields;
    if (platform_ == "qq") out["app_id"] = config.credentials.value("app_id", std::string{});
    nlohmann::json contacts = nlohmann::json::array();
    for (const auto& entry : config.access) {
        contacts.push_back({{"principal", entry.principal},
                            {"kind", principal_kind(entry.principal)},
                            {"name", entry.name},
                            {"owner", entry.principal == config.owner},
                            {"approved_at_ms", entry.approved_at_ms}});
    }
    out["contacts"] = contacts;
    out["owner"] = config.owner.empty() ? nlohmann::json(nullptr) : nlohmann::json(config.owner);
    nlohmann::json pending = nlohmann::json::array();
    const auto now = AccessControl::Clock::now();
    for (const auto& request : access_.pending(now)) {
        pending.push_back(
            {{"id", request.id},
             {"kind", request.kind},
             {"name", request.name},
             {"principal", request.principal},
             {"label", Conversations::label_for(request.address)},
             {"expires_in_s", std::chrono::duration_cast<std::chrono::seconds>(request.expires - now).count()}});
    }
    out["pending"] = pending;
    out["owner_window"] = access_.owner_window_open(now);
    out["bindings"] = conversations_->bindings_json();
    return out;
}

} // namespace acecode::channels::core
