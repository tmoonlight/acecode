#include "plugin_transport.hpp"

#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <stdexcept>
#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#endif

namespace acecode::im::plugin {
namespace {

// 一次子进程稳定运行这么久之后再退出,退避从头开始。
constexpr auto kStableRun = std::chrono::seconds(60);
// reader 每次最多等这么久就回头看一眼是否该退出;supervisor 按这个间隔检查插件进程是否已退出。
constexpr int kReadPollMs = 100;
constexpr auto kExitPoll = std::chrono::milliseconds(200);
// 初始化完成前最多先存这么多事件;正常插件此时只有一两条状态与入站。
constexpr std::size_t kMaxEarlyEvents = 256;

std::string str(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool flag(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return false;
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

AttachmentKind parse_kind(const std::string& kind) {
    if (kind == "image") return AttachmentKind::Image;
    if (kind == "file") return AttachmentKind::File;
    if (kind == "voice") return AttachmentKind::Voice;
    if (kind == "video") return AttachmentKind::Video;
    if (kind == "sticker") return AttachmentKind::Sticker;
    return AttachmentKind::Other;
}

const char* kind_name(AttachmentKind kind) {
    switch (kind) {
        case AttachmentKind::Image: return "image";
        case AttachmentKind::File: return "file";
        case AttachmentKind::Voice: return "voice";
        case AttachmentKind::Video: return "video";
        case AttachmentKind::Sticker: return "sticker";
        case AttachmentKind::Other: break;
    }
    return "other";
}

nlohmann::json address_json(const Address& address) {
    nlohmann::json value{{"kind", address.kind == ChatKind::Group ? "group" : "private"},
                         {"chat", address.chat},
                         {"sender", address.sender}};
    if (!address.thread.empty()) value["thread"] = address.thread;
    return value;
}

std::uint64_t positive_u64(const nlohmann::json& object, const char* key, std::uint64_t fallback) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_unsigned()) return fallback;
    return it->get<std::uint64_t>();
}

} // namespace

LinkState parse_plugin_link_state(const std::string& state) {
    if (state == "connected") return LinkState::Connected;
    if (state == "retrying") return LinkState::Retrying;
    if (state == "failed") return LinkState::Failed;
    if (state == "stopped") return LinkState::Stopped;
    return LinkState::Connecting;
}

bool parse_plugin_inbound(const nlohmann::json& params, const std::string& platform, const std::string& account,
                          Inbound& out) {
    if (!params.is_object()) return false;
    const auto address = params.find("address");
    if (address == params.end() || !address->is_object()) return false;
    Inbound inbound;
    inbound.address.platform = platform;
    inbound.address.account = account;
    const auto kind = str(*address, "kind");
    if (kind != "group" && kind != "private") return false;
    inbound.address.kind = kind == "group" ? ChatKind::Group : ChatKind::Private;
    inbound.address.chat = str(*address, "chat");
    inbound.address.sender = str(*address, "sender");
    inbound.address.thread = str(*address, "thread");
    if (!inbound.address.valid()) return false;
    inbound.message_id = str(params, "message_id");
    if (inbound.message_id.empty() || inbound.message_id.size() > 256) return false;
    inbound.text = str(params, "text");
    inbound.mentioned = inbound.address.kind == ChatKind::Private || flag(params, "mentioned");
    inbound.quote_text = str(params, "quote_text");
    inbound.sender_name = str(params, "sender_name");
    inbound.chat_name = str(params, "chat_name");
    inbound.from_self = flag(params, "from_self");
    if (const auto it = params.find("reply_context"); it != params.end() && it->is_object())
        inbound.reply_context = *it;
    if (const auto it = params.find("timestamp_ms"); it != params.end() && it->is_number_integer())
        inbound.timestamp_ms = it->get<std::int64_t>();
    if (const auto it = params.find("attachments"); it != params.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_object()) continue;
            Attachment attachment;
            attachment.kind = parse_kind(str(item, "kind"));
            attachment.name = str(item, "name");
            attachment.mime_type = str(item, "mime_type");
            attachment.size = positive_u64(item, "size", 0);
            attachment.remote_ref = str(item, "remote_ref");
            attachment.transcript = str(item, "transcript");
            inbound.attachments.push_back(std::move(attachment));
        }
    }
    out = std::move(inbound);
    return true;
}

nlohmann::json attachment_to_plugin_json(const Attachment& attachment) {
    return {{"kind", kind_name(attachment.kind)},
            {"name", attachment.name},
            {"mime_type", attachment.mime_type},
            {"size", attachment.size},
            {"remote_ref", attachment.remote_ref}};
}

PluginTransport::PluginTransport(PluginTransportOptions options)
    : options_(std::move(options)), settings_(options_.settings) {}

PluginTransport::~PluginTransport() { stop(); }

Capabilities PluginTransport::capabilities() const {
    std::lock_guard<std::mutex> lock(mu_);
    return capabilities_;
}

TransportStatus PluginTransport::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

void PluginTransport::start(TransportCallbacks callbacks) {
    if (running_.exchange(true)) return;
    callbacks_ = std::move(callbacks);
    stopping_ = false;
    set_local_status(LinkState::Connecting, "正在启动");
    supervisor_ = acecode::JoiningThread(&PluginTransport::supervise_loop, this);
}

void PluginTransport::stop() {
    if (!running_.load()) return;
    stopping_ = true;
    wake_.notify_all();
    {
        std::lock_guard<std::mutex> lock(io_mu_);
        io_cv_.notify_all();
    }
    if (supervisor_.joinable()) supervisor_.join();
    running_ = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_.state = LinkState::Stopped;
        status_.detail.clear();
    }
}

bool PluginTransport::wait_for(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(wake_mu_);
    wake_.wait_for(lock, duration, [this] { return stopping_.load(); });
    return !stopping_.load();
}

void PluginTransport::set_local_status(LinkState state, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_.state = state;
        status_.detail = detail;
        status_.retry_stopped = false;
    }
    emit_status();
}

void PluginTransport::emit_status() {
    if (!callbacks_.on_status) return;
    callbacks_.on_status(status());
}

void PluginTransport::supervise_loop() {
    auto backoff = options_.initial_backoff;
    while (!stopping_.load()) {
        const auto began = std::chrono::steady_clock::now();
        run_process_once();
        if (stopping_.load()) break;
        std::string reason;
        {
            std::lock_guard<std::mutex> lock(io_mu_);
            reason = failure_;
        }
        if (std::chrono::steady_clock::now() - began >= kStableRun) backoff = options_.initial_backoff;
        LOG_WARN("[channels/" + options_.platform + "] plugin stopped: " + reason + "; restarting in " +
                 std::to_string(backoff.count()) + " ms");
        set_local_status(LinkState::Retrying, reason.empty() ? std::string("插件进程已退出,正在重启") : reason);
        if (!wait_for(backoff)) break;
        backoff = std::min(backoff * 2, options_.max_backoff);
    }
}

bool PluginTransport::run_process_once() {
    platform::SpawnOptions spawn;
    spawn.argv.push_back(options_.command);
    for (const auto& arg : options_.args) spawn.argv.push_back(arg);
    spawn.cwd = options_.cwd;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(io_mu_);
        alive_ = false;
        initialized_ = false;
        failure_.clear();
        pending_.clear();
        writes_.clear();
        early_events_.clear();
    }
    if (!process_.start(spawn, &error)) {
        std::lock_guard<std::mutex> lock(io_mu_);
        failure_ = "无法启动插件:" + error;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(io_mu_);
        alive_ = true;
    }
    LOG_INFO("[channels/" + options_.platform + "] plugin process started");
    reader_ = acecode::JoiningThread(&PluginTransport::read_loop, this);
    writer_ = acecode::JoiningThread(&PluginTransport::write_loop, this);

    nlohmann::json settings;
    {
        std::lock_guard<std::mutex> lock(mu_);
        settings = settings_;
    }
    try {
        const auto result = request("initialize",
                                    {{"protocol_version", kProtocolVersion},
                                     {"platform", options_.platform},
                                     {"settings", settings}},
                                    options_.initialize_timeout);
        const auto version = result.is_object() ? result.value("protocol_version", 0) : 0;
        if (version != kProtocolVersion)
            throw std::runtime_error("插件协议版本不兼容:" + std::to_string(version));
        Capabilities caps;
        const auto c = result.value("capabilities", nlohmann::json::object());
        if (c.is_object()) {
            caps.max_text_units = static_cast<std::size_t>(
                std::clamp<std::uint64_t>(positive_u64(c, "max_text_units", caps.max_text_units), 64, 65536));
            caps.count_utf16 = flag(c, "count_utf16");
            caps.batch_turn_output = flag(c, "batch_turn_output");
            caps.supports_typing = flag(c, "supports_typing");
            caps.max_upload_bytes = positive_u64(c, "max_upload_bytes", 0);
            caps.max_download_bytes = positive_u64(c, "max_download_bytes", 0);
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            capabilities_ = caps;
        }
        {
            std::lock_guard<std::mutex> lock(io_mu_);
            initialized_ = alive_;
        }
        flush_early_events();
    } catch (const std::exception& e) {
        fail_process(std::string("插件初始化失败:") + e.what());
    }

    // 插件拉起的常驻子进程会继承 stdout 写端,插件退出后管道不一定 EOF;
    // 所以这里周期性地看插件进程本身是否已退出。
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(io_mu_);
            if (io_cv_.wait_for(lock, kExitPoll, [this] { return !alive_ || stopping_.load(); })) break;
        }
        if (process_.wait_exit(0)) {
            fail_process("插件进程已退出");
            break;
        }
    }
    if (stopping_.load()) {
        // 正常关闭:先请插件自己收尾,再关 stdin,限时等它退出。
        try {
            request("shutdown", nlohmann::json::object(), options_.shutdown_timeout);
        } catch (...) {
        }
        process_.close_stdin();
        process_.wait_exit(static_cast<int>(options_.shutdown_timeout.count()));
        fail_process("插件已停止");
    }
    // 先杀进程让阻塞的读写返回,再 join 读写线程,最后关句柄;不与在途读写竞争句柄。
    process_.kill_child();
    if (writer_.joinable()) writer_.join();
    if (reader_.joinable()) reader_.join();
    process_.terminate();
    LOG_INFO("[channels/" + options_.platform + "] plugin process exited");
    return true;
}

void PluginTransport::fail_process(const std::string& reason) {
    std::lock_guard<std::mutex> lock(io_mu_);
    if (alive_) {
        alive_ = false;
        failure_ = reason;
    }
    initialized_ = false;
    for (const auto& [id, pending] : pending_) {
        if (pending->done) continue;
        pending->done = true;
        pending->error = reason;
    }
    writes_.clear();
    io_cv_.notify_all();
}

void PluginTransport::read_loop() {
    std::string buffer;
    char bytes[8192];
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(io_mu_);
            if (!alive_) break;
        }
        bool timed_out = false;
        const auto count = process_.read_stdout_for(bytes, sizeof(bytes), kReadPollMs, &timed_out);
        if (timed_out) continue;  // 回到循环顶部检查进程是否已被判定退出
        if (count <= 0) {
            fail_process("插件进程已退出");
            break;
        }
        buffer.append(bytes, static_cast<std::size_t>(count));
        for (;;) {
            const auto newline = buffer.find('\n');
            if (newline == std::string::npos) break;
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
            const auto frame = nlohmann::json::parse(line, nullptr, false);
            if (frame.is_discarded() || !frame.is_object()) {
                fail_process("插件输出了无法解析的协议帧");
                return;
            }
            try {
                handle_frame(frame);
            } catch (const std::exception& e) {
                LOG_WARN("[channels/" + options_.platform + "] plugin frame rejected: " + e.what());
            }
        }
        if (buffer.size() > kMaxFrameBytes) {
            fail_process("插件协议帧超过 1 MiB");
            break;
        }
    }
}

void PluginTransport::write_loop() {
#ifndef _WIN32
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
#endif
    for (;;) {
        std::string frame;
        {
            std::unique_lock<std::mutex> lock(io_mu_);
            io_cv_.wait(lock, [this] { return !alive_ || !writes_.empty(); });
            if (!alive_ && writes_.empty()) break;
            if (writes_.empty()) continue;
            frame = std::move(writes_.front());
            writes_.pop_front();
        }
        std::string error;
        if (!process_.write_stdin(frame.data(), frame.size(), &error)) {
            fail_process("插件输入管道已断开");
            break;
        }
    }
}

void PluginTransport::handle_frame(const nlohmann::json& frame) {
    if (const auto id = frame.find("id"); id != frame.end()) {
        if (!id->is_number_unsigned()) return;
        std::lock_guard<std::mutex> lock(io_mu_);
        const auto it = pending_.find(id->get<std::uint64_t>());
        if (it == pending_.end()) return;  // 已超时的请求
        it->second->done = true;
        if (frame.value("ok", false)) {
            it->second->result = frame.value("result", nlohmann::json::object());
        } else {
            const auto error = str(frame, "error");
            it->second->error = error.empty() ? std::string("插件请求失败") : error;
        }
        io_cv_.notify_all();
        return;
    }
    const auto event = str(frame, "event");
    if (event.empty()) throw std::runtime_error("unrecognized frame");
    dispatch_event(event, frame.value("params", nlohmann::json::object()));
}

void PluginTransport::dispatch_event(const std::string& event, const nlohmann::json& params) {
    // 插件回复 initialize 后紧接着就报状态、推入站;supervisor 那时可能还没处理完 initialize
    // 结果(能力、就绪标记)。初始化完成前的事件按到达顺序先存着,完成后再依次处理,外部看到
    // 「已连接」时能力与就绪状态一定已经就位。event_mu_ 让补发与后来的事件不会交错。
    std::lock_guard<std::mutex> order(event_mu_);
    std::vector<std::pair<std::string, nlohmann::json>> backlog;
    {
        std::lock_guard<std::mutex> lock(io_mu_);
        if (!initialized_) {
            if (early_events_.size() < kMaxEarlyEvents) early_events_.emplace_back(event, params);
            return;
        }
        backlog.swap(early_events_);
    }
    for (const auto& [name, early] : backlog) handle_event_logged(name, early);
    handle_event(event, params);
}

void PluginTransport::flush_early_events() {
    std::lock_guard<std::mutex> order(event_mu_);
    std::vector<std::pair<std::string, nlohmann::json>> backlog;
    {
        std::lock_guard<std::mutex> lock(io_mu_);
        if (!initialized_) return;
        backlog.swap(early_events_);
    }
    for (const auto& [name, early] : backlog) handle_event_logged(name, early);
}

void PluginTransport::handle_event_logged(const std::string& event, const nlohmann::json& params) {
    try {
        handle_event(event, params);
    } catch (const std::exception& e) {
        LOG_WARN("[channels/" + options_.platform + "] plugin event rejected: " + e.what());
    }
}

void PluginTransport::handle_event(const std::string& event, const nlohmann::json& params) {
    if (event == "status") {
        {
            std::lock_guard<std::mutex> lock(mu_);
            status_.state = parse_plugin_link_state(str(params, "state"));
            status_.detail = str(params, "detail");
            status_.retry_stopped = flag(params, "retry_stopped");
            const auto account = str(params, "account");
            if (!account.empty()) status_.account = account;
            if (params.contains("display_name")) status_.display_name = str(params, "display_name");
            const auto extra = params.find("extra");
            status_.extra = extra != params.end() && extra->is_object() ? *extra : nlohmann::json::object();
        }
        emit_status();
        return;
    }
    if (event == "inbound") {
        std::string account;
        {
            std::lock_guard<std::mutex> lock(mu_);
            account = status_.account;
        }
        Inbound inbound;
        if (account.empty() || !parse_plugin_inbound(params, options_.platform, account, inbound)) {
            LOG_WARN("[channels/" + options_.platform + "] plugin inbound dropped: invalid address or no account yet");
            return;
        }
        if (callbacks_.on_inbound) callbacks_.on_inbound(std::move(inbound));
        return;
    }
    // 未知事件忽略,便于插件先于 ACECode 扩展协议。
}

nlohmann::json PluginTransport::request(const std::string& method, const nlohmann::json& params,
                                        std::chrono::milliseconds timeout) {
    auto pending = std::make_shared<Pending>();
    std::unique_lock<std::mutex> lock(io_mu_);
    if (!alive_) throw std::runtime_error(failure_.empty() ? std::string("插件未运行") : failure_);
    if (!initialized_ && method != "initialize" && method != "shutdown") throw std::runtime_error("插件尚未就绪");
    if (pending_.size() >= kMaxPending || writes_.size() >= kMaxPending)
        throw std::runtime_error("插件请求队列已满");
    const auto id = next_id_++;
    auto frame = nlohmann::json{{"id", id}, {"method", method}, {"params", params}}.dump(
                     -1, ' ', false, nlohmann::json::error_handler_t::replace) +
                 '\n';
    if (frame.size() > kMaxFrameBytes) throw std::runtime_error("请求超过 1 MiB");
    pending_[id] = pending;
    writes_.push_back(std::move(frame));
    io_cv_.notify_all();
    const bool ready = io_cv_.wait_for(lock, timeout, [&pending] { return pending->done; });
    pending_.erase(id);
    // 超时的投递结果未知:不重发,让调用方按失败处理。
    if (!ready) throw std::runtime_error("插件响应超时");
    if (!pending->error.empty()) throw std::runtime_error(pending->error);
    return pending->result;
}

void PluginTransport::notify(const std::string& method, const nlohmann::json& params) {
    std::lock_guard<std::mutex> lock(io_mu_);
    if (!alive_ || !initialized_ || writes_.size() >= kMaxPending) return;
    auto frame = nlohmann::json{{"method", method}, {"params", params}}.dump(
                     -1, ' ', false, nlohmann::json::error_handler_t::replace) +
                 '\n';
    if (frame.size() > kMaxFrameBytes) return;
    writes_.push_back(std::move(frame));
    io_cv_.notify_all();
}

SendResult PluginTransport::send_result(const nlohmann::json& result) const {
    SendResult out;
    const auto outcome = str(result, "outcome");
    if (outcome == "sent") out.outcome = SendOutcome::Sent;
    else if (outcome == "held") out.outcome = SendOutcome::Held;
    else out.outcome = SendOutcome::Failed;
    out.error = str(result, "error");
    if (out.outcome == SendOutcome::Failed && out.error.empty()) out.error = "插件未能发送";
    return out;
}

SendResult PluginTransport::send_text(const Address& to, const std::string& text, const nlohmann::json& reply_context) {
    try {
        return send_result(request("send_text",
                                   {{"address", address_json(to)},
                                    {"text", text},
                                    {"reply_context", reply_context.is_object() ? reply_context
                                                                                : nlohmann::json::object()}},
                                   options_.send_timeout));
    } catch (const std::exception& e) {
        return {SendOutcome::Failed, e.what()};
    }
}

SendResult PluginTransport::send_file(const Address& to, const std::filesystem::path& path, const std::string& name,
                                      const std::string& mime_type, const nlohmann::json& reply_context) {
    try {
        return send_result(request("send_file",
                                   {{"address", address_json(to)},
                                    {"path", path_to_utf8(path)},
                                    {"name", name},
                                    {"mime_type", mime_type},
                                    {"reply_context", reply_context.is_object() ? reply_context
                                                                                : nlohmann::json::object()}},
                                   options_.file_timeout));
    } catch (const std::exception& e) {
        return {SendOutcome::Failed, e.what()};
    }
}

void PluginTransport::set_typing(const Address& to, bool on) {
    notify("set_typing", {{"address", address_json(to)}, {"on", on}});
}

bool PluginTransport::download(const Attachment& attachment, const std::filesystem::path& dest, std::string* error) {
    try {
        const auto result = request("download",
                                    {{"attachment", attachment_to_plugin_json(attachment)}, {"dest", path_to_utf8(dest)}},
                                    options_.file_timeout);
        if (flag(result, "ok")) return true;
        if (error) *error = str(result, "error").empty() ? std::string("插件未能下载附件") : str(result, "error");
        return false;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

nlohmann::json PluginTransport::action(const std::string& name, const nlohmann::json& args) {
    if (name == "configure") {
        {
            std::lock_guard<std::mutex> lock(mu_);
            settings_ = args.is_object() ? args : nlohmann::json::object();
        }
        return request("configure", {{"settings", args}}, options_.typing_timeout);
    }
    return request("action", {{"name", name}, {"args", args.is_object() ? args : nlohmann::json::object()}},
                   options_.action_timeout);
}

} // namespace acecode::im::plugin
