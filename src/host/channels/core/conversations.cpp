#include "conversations.hpp"

#include "channels/core/platforms.hpp"
#include "platform/crypto/secure_random.hpp"
#include "session/attachment_store.hpp"
#include "session/output_attachments.hpp"
#include "session/session_storage.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace acecode::channels::core {
namespace {

struct SessionMissing : std::runtime_error {
    SessionMissing() : std::runtime_error("session missing") {}
};

bool all_digits(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string location_label(const BindingRecord& record) {
    if (record.no_workspace || record.cwd.empty()) return "无项目";
    return path_to_utf8(path_from_utf8(record.cwd).filename());
}

std::string media_kind_label(im::AttachmentKind kind) {
    switch (kind) {
        case im::AttachmentKind::Voice: return "语音";
        case im::AttachmentKind::Video: return "视频";
        case im::AttachmentKind::Sticker: return "贴纸";
        default: return "此类";
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const char* command_name(CommandKind kind) {
    switch (kind) {
        case CommandKind::Help: return "/help";
        case CommandKind::Status: return "/status";
        case CommandKind::Stop: return "/stop";
        case CommandKind::New: return "/new";
        case CommandKind::Sessions: return "/sessions";
        case CommandKind::Resume: return "/resume";
        case CommandKind::Model: return "/model";
        case CommandKind::Approve: return "/approve";
        case CommandKind::Deny: return "/deny";
        case CommandKind::Question: return "/aq";
        case CommandKind::None: break;
    }
    return "(text)";
}

} // namespace

Conversations::Conversations(std::string platform, ChannelStore& store, AccessControl& access,
                             std::shared_ptr<TransportSlot> transport, ConversationDeps deps)
    : platform_(std::move(platform)), store_(store), access_(access), transport_(std::move(transport)),
      deps_(std::move(deps)) {}

Conversations::~Conversations() { shutdown(); }

std::string Conversations::label_for(const im::Address& address) {
    std::string label = platform_label(address.platform);
    label += address.kind == im::ChatKind::Group ? " 群聊" : " 私聊";
    return label;
}

void Conversations::reply(const im::Inbound& inbound, const std::string& text) {
    if (auto projection = projection_for(inbound.address.key())) {
        projection->say(text, inbound.reply_context);
        return;
    }
    const auto transport = transport_->get();
    if (!transport) return;
    const auto result = transport->send_text(inbound.address, text, inbound.reply_context);
    if (!result.ok()) LOG_WARN("[channels/" + platform_ + "] reply failed: " + result.error);
}

std::shared_ptr<Projection> Conversations::projection_for(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = projections_.find(key);
    return it == projections_.end() ? nullptr : it->second;
}

HandleOutcome Conversations::handle(const im::Inbound& inbound) {
    HandleOutcome outcome;
    const auto config = store_.config();
    const auto access = access_.evaluate(config, inbound, AccessControl::Clock::now());
    outcome.pending_created = access.created;
    {
        // 只记来源、长度和判定原因,不记消息内容。
        const auto line = "[channels/" + platform_ + "] inbound " + label_for(inbound.address) + " chat " +
                          inbound.address.chat + " from " + access.principal + " (" +
                          std::to_string(inbound.text.size()) + " bytes, " +
                          std::to_string(inbound.attachments.size()) + " attachment(s)): " + access.reason;
        if (access.created || access.claimed_owner) LOG_INFO(line);
        else LOG_DEBUG(line);
    }
    if (access.claimed_owner) {
        store_.update_config([&access, &inbound](PlatformConfig& c) {
            c.owner = access.principal;
            if (!c.has_access(access.principal))
                c.access.push_back({access.principal, inbound.sender_name, now_wall_ms()});
        });
        outcome.owner_claimed = true;
        reply(inbound, texts::owner_bound());
        if (access.consumed) return outcome;  // 绑定码(/start <码> 或 6 位数字)本身不交给会话
    }
    if (access.decision == AccessDecision::NotifyPending) {
        reply(inbound, texts::pairing_notice());
        return outcome;
    }
    if (access.decision != AccessDecision::Allow) return outcome;

    const auto key = inbound.address.key();
    if (store_.has_receipt(key, inbound.message_id)) return outcome;
    // 先记回执再处理:崩溃时宁可让对方重发,也不重复执行工具。
    store_.remember_receipt(key, inbound.message_id);
    try {
        dispatch(inbound, access.owner);
    } catch (const SessionMissing&) {
        reply(inbound, texts::session_missing());
    } catch (const std::exception& e) {
        store_.forget_receipt(key, inbound.message_id);
        reply(inbound, texts::turn_failed(e.what()));
    }
    return outcome;
}

void Conversations::dispatch(const im::Inbound& inbound, bool owner) {
    const auto command = parse_command(inbound.text);
    if (!command.error.empty()) {
        reply(inbound, command.error);
        return;
    }
    if (command.kind != CommandKind::None) {
        LOG_INFO("[channels/" + platform_ + "] command " + command_name(command.kind) + " in " +
                 label_for(inbound.address) + " chat " + inbound.address.chat);
    }
    switch (command.kind) {
        case CommandKind::Help: reply(inbound, texts::help(owner)); return;
        case CommandKind::Sessions: list_sessions(inbound, owner, command.argument); return;
        case CommandKind::Resume: resume(inbound, owner, command.argument); return;
        case CommandKind::New: new_session(inbound); return;
        case CommandKind::None: submit(inbound); return;
        default: handle_bound_command(inbound, command); return;
    }
}

std::optional<SessionInfo> Conversations::session_info(const std::string& session_id) const {
    for (auto& info : deps_.sessions->list_sessions())
        if (info.id == session_id) return info;
    return std::nullopt;
}

void Conversations::handle_bound_command(const im::Inbound& inbound, const Command& command) {
    const auto record = store_.binding(inbound.address.key());
    if (!record) {
        reply(inbound, texts::no_binding());
        return;
    }
    const auto projection = ensure_projection(*record);
    const auto& id = record->session_id;
    switch (command.kind) {
        case CommandKind::Stop:
            deps_.sessions->abort(id);
            reply(inbound, texts::stop_requested());
            return;
        case CommandKind::Status: {
            const auto info = session_info(id);
            std::string text = "平台:" + platform_label(platform_) + "\n会话:" +
                               (info && !info->title.empty() ? info->title : id) + "\n位置:" +
                               location_label(*record) + "\n模型:" +
                               (info && !info->model_name.empty() ? info->model_name : std::string("默认")) +
                               "\n状态:" + (info && info->busy ? "执行中" : "空闲");
            const auto permissions = projection->pending_permission_ids();
            for (const auto& request : permissions) text += "\n待确认权限:" + request;
            for (const auto& line : projection->pending_question_texts()) text += "\n" + line;
            reply(inbound, text);
            return;
        }
        case CommandKind::Model: {
            const auto names = deps_.model_names ? deps_.model_names() : std::vector<std::string>{};
            if (command.argument.empty()) {
                const auto info = session_info(id);
                std::string text = "可用模型(* 为当前):";
                for (const auto& name : names)
                    text += "\n" + std::string(info && info->model_name == name ? "* " : "  ") + name;
                reply(inbound, text);
                return;
            }
            if (std::find(names.begin(), names.end(), command.argument) == names.end()) {
                reply(inbound, texts::model_unknown(command.argument, names));
                return;
            }
            std::string error;
            if (!deps_.switch_model || !deps_.switch_model(id, command.argument, &error)) {
                reply(inbound, texts::turn_failed(error.empty() ? "切换模型失败" : error));
                return;
            }
            reply(inbound, texts::model_switched(command.argument));
            return;
        }
        case CommandKind::Approve:
        case CommandKind::Deny: {
            if (!projection->take_permission(command.argument)) {
                reply(inbound, texts::permission_unknown());
                return;
            }
            const bool approve = command.kind == CommandKind::Approve;
            deps_.sessions->respond_permission(
                id, {command.argument, approve ? PermissionDecisionChoice::Allow : PermissionDecisionChoice::Deny});
            reply(inbound, texts::permission_submitted(command.argument, approve));
            return;
        }
        case CommandKind::Question: {
            const auto action = projection->with_questions(
                [&inbound](rc::ChannelQuestionBridge& questions) { return questions.handle_input(inbound.text); });
            projection->emit_question_action(action);
            if (!action.submission) return;
            // respond_question 可能同步触发 QuestionClosed,必须在问题状态机锁外调用。
            const auto status = deps_.sessions->respond_question(id, action.submission->request_id,
                                                                 action.submission->response);
            projection->emit_question_action(
                projection->with_questions([&action, status](rc::ChannelQuestionBridge& questions) {
                    return questions.complete_submission(action.submission->request_id, status);
                }));
            return;
        }
        default:
            return;
    }
}

std::vector<rc::RcSessionTarget> Conversations::visible_sessions(const std::string& key, bool owner,
                                                                   const std::optional<std::string>& query) const {
    auto targets = deps_.catalog ? deps_.catalog(query) : std::vector<rc::RcSessionTarget>{};
    if (owner) return targets;
    const auto created = store_.created_sessions(key);
    targets.erase(std::remove_if(targets.begin(), targets.end(),
                                 [&](const rc::RcSessionTarget& target) {
                                     return std::find(created.begin(), created.end(), target.session_id) ==
                                            created.end();
                                 }),
                  targets.end());
    return targets;
}

void Conversations::list_sessions(const im::Inbound& inbound, bool owner, const std::string& argument) {
    std::optional<std::string> query;
    std::size_t limit = rc::kRcSessionRecentLimit;
    std::string lowered = argument;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered.rfind("search ", 0) == 0) {
        query = argument.substr(7);
        limit = rc::kRcSessionSearchLimit;
    } else if (lowered == "more" || lowered == "all") {
        limit = 50;
    }
    auto targets = visible_sessions(inbound.address.key(), owner, query);
    if (targets.size() > limit) targets.resize(limit);
    {
        std::lock_guard<std::mutex> lock(mu_);
        snapshots_[inbound.address.key()] = targets;
    }
    if (targets.empty()) {
        reply(inbound, owner ? "没有可切换的会话。" : "你还没有通过这里创建过会话。");
        return;
    }
    reply(inbound, rc::format_rc_session_listing(targets, "可切换的会话(发送 /resume <编号> 切换):"));
}

void Conversations::resume(const im::Inbound& inbound, bool owner, const std::string& argument) {
    const auto key = inbound.address.key();
    const auto current = store_.binding(key);
    if (current) {
        const auto info = session_info(current->session_id);
        if (info && info->busy) {
            reply(inbound, texts::busy_reject());
            return;
        }
    }
    std::optional<rc::RcSessionTarget> target;
    if (all_digits(argument) && argument.size() <= 6) {
        std::vector<rc::RcSessionTarget> snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const auto it = snapshots_.find(key);
            if (it != snapshots_.end()) snapshot = it->second;
        }
        if (snapshot.empty()) {
            snapshot = visible_sessions(key, owner, std::nullopt);
            if (snapshot.size() > rc::kRcSessionRecentLimit) snapshot.resize(rc::kRcSessionRecentLimit);
        }
        target = rc::select_rc_session_snapshot(snapshot, std::stoul(argument));
    } else {
        for (const auto& candidate : visible_sessions(key, true, std::nullopt))
            if (candidate.session_id == argument) target = candidate;
    }
    if (!target) {
        reply(inbound, "没有找到这个会话。先发送 /sessions 查看可切换的会话。");
        return;
    }
    if (!owner) {
        const auto created = store_.created_sessions(key);
        if (std::find(created.begin(), created.end(), target->session_id) == created.end()) {
            reply(inbound, texts::resume_denied());
            return;
        }
    }
    if (current && current->session_id == target->session_id) {
        reply(inbound, "已经在这个会话里了。");
        return;
    }
    if (!deps_.resume_target || !deps_.resume_target(*target)) {
        reply(inbound, texts::session_missing());
        return;
    }
    BindingRecord record;
    record.address = inbound.address;
    record.session_id = target->session_id;
    record.cwd = target->cwd;
    record.workspace_hash = target->workspace_hash;
    record.no_workspace = target->no_workspace;
    record.updated_at_ms = now_wall_ms();
    switch_binding(record);
    reply(inbound, texts::transferred_here(target->title.empty() ? target->session_id : target->title));
}

BindingRecord Conversations::create_binding(const im::Address& address, const std::optional<BindingRecord>& near) {
    SessionOptions options;
    options.permission_mode = "default";
    options.inherit_dangerous_mode = false;
    if (near && !near->no_workspace) {
        options.cwd = near->cwd;
        options.workspace_hash = near->workspace_hash;
    } else {
        options.no_workspace = true;
    }
    const auto id = deps_.sessions->create_session(options);
    if (id.empty()) throw std::runtime_error("无法创建会话");
    BindingRecord record;
    record.address = address;
    record.session_id = id;
    record.cwd = deps_.session_cwd ? deps_.session_cwd(id) : std::string{};
    record.no_workspace = options.no_workspace;
    record.workspace_hash = options.no_workspace ? std::string{} : options.workspace_hash;
    record.updated_at_ms = now_wall_ms();
    store_.remember_created(address.key(), id);
    return record;
}

void Conversations::new_session(const im::Inbound& inbound) {
    const auto current = store_.binding(inbound.address.key());
    if (current) {
        const auto info = session_info(current->session_id);
        if (info && info->busy) {
            reply(inbound, texts::busy_reject());
            return;
        }
    }
    const auto record = create_binding(inbound.address, current);
    switch_binding(record);
    reply(inbound, texts::new_session(location_label(record)));
}

std::shared_ptr<Projection> Conversations::ensure_projection(const BindingRecord& record) {
    const auto key = record.address.key();
    if (auto existing = projection_for(key); existing && existing->record().session_id == record.session_id)
        return existing;
    rc::RcSessionTarget target;
    target.session_id = record.session_id;
    target.cwd = record.cwd;
    target.workspace_hash = record.workspace_hash;
    target.no_workspace = record.no_workspace;
    if (!deps_.resume_target || !deps_.resume_target(target)) throw SessionMissing();
    auto projection = std::make_shared<Projection>(
        record, ProjectionDeps{deps_.sessions, transport_, deps_.pending_permissions});
    projection->start();
    std::shared_ptr<Projection> previous;
    {
        std::lock_guard<std::mutex> lock(mu_);
        previous = std::move(projections_[key]);
        projections_[key] = projection;
    }
    if (previous) previous->stop();
    return projection;
}

void Conversations::switch_binding(const BindingRecord& record) {
    const auto key = record.address.key();
    if (deps_.release_session) deps_.release_session(record.session_id, key, label_for(record.address));
    store_.put_binding(record);
    ensure_projection(record);
    if (deps_.on_bindings_changed) deps_.on_bindings_changed();
}

bool Conversations::release_session(const std::string& session_id, const std::string& except_key,
                                    const std::string& taker_label) {
    bool released = false;
    for (const auto& record : store_.bindings()) {
        const auto key = record.address.key();
        if (record.session_id != session_id || key == except_key) continue;
        std::shared_ptr<Projection> projection;
        {
            std::lock_guard<std::mutex> lock(mu_);
            const auto it = projections_.find(key);
            if (it != projections_.end()) {
                projection = it->second;
                projections_.erase(it);
            }
        }
        const auto notice = texts::bound_elsewhere(taker_label);
        if (projection) {
            projection->say(notice, nlohmann::json::object());
            projection->stop();
        } else if (const auto transport = transport_->get()) {
            transport->send_text(record.address, notice, nlohmann::json::object());
        }
        store_.erase_binding(key);
        released = true;
    }
    return released;
}

void Conversations::submit(const im::Inbound& inbound) {
    const auto key = inbound.address.key();
    auto record = store_.binding(key);
    if (!record) {
        record = create_binding(inbound.address, std::nullopt);
        store_.put_binding(*record);
        if (deps_.on_bindings_changed) deps_.on_bindings_changed();
    }
    const auto projection = ensure_projection(*record);
    const auto& session_id = record->session_id;

    std::string text = inbound.text;
    std::vector<im::Attachment> files;
    for (const auto& attachment : inbound.attachments) {
        if (attachment.kind == im::AttachmentKind::Voice && !attachment.transcript.empty()) {
            text += (text.empty() ? "" : "\n") + std::string("[语音转写] ") + attachment.transcript;
        } else if (attachment.kind == im::AttachmentKind::Image || attachment.kind == im::AttachmentKind::File) {
            files.push_back(attachment);
        } else {
            reply(inbound, texts::unsupported_media(media_kind_label(attachment.kind)));
        }
    }
    if (text.empty() && files.empty()) return;
    if (!inbound.quote_text.empty())
        text = "[引用的消息]\n" + inbound.quote_text + "\n[引用结束]\n" + text;

    nlohmann::json channel{{"platform", platform_},
                           {"address", inbound.address.to_json()},
                           {"message_id", inbound.message_id},
                           {"reply_context", inbound.reply_context}};
    if (!inbound.sender_name.empty()) channel["sender_name"] = inbound.sender_name;

    // 提问挂起时的普通文字视为插话,同一回合紧跟在工具结果后交给模型。
    if (files.empty() && !text.empty()) {
        const auto request_id = projection->with_questions(
            [](rc::ChannelQuestionBridge& questions) { return questions.begin_interjection(); });
        if (request_id) {
            UserInput interjection;
            interjection.text = text;
            interjection.metadata["channel"] = channel;
            const auto result = deps_.sessions->interject_question(session_id, *request_id, interjection);
            const auto outcome = result.accepted() ? QuestionResponseStatus::Accepted : QuestionResponseStatus::Closed;
            projection->emit_question_action(
                projection->with_questions([&request_id, outcome](rc::ChannelQuestionBridge& questions) {
                    return questions.complete_submission(*request_id, outcome);
                }));
            if (result.accepted()) return;
        }
    }

    UserInput input;
    input.text = deps_.expand_skill && !text.empty() && text[0] == '/' ? deps_.expand_skill(session_id, text) : text;
    input.display_text = inbound.text.empty() ? text : inbound.text;
    input.metadata["channel"] = channel;
    if (!files.empty()) {
        const auto transport = transport_->get();
        if (!transport) throw std::runtime_error("通道未连接");
        if (!input.text.empty()) input.content_parts.push_back({{"type", "text"}, {"text", input.text}});
        nlohmann::json saved = nlohmann::json::array();
        std::filesystem::create_directories(deps_.media_dir);
        for (const auto& file : files) {
            const auto temp = deps_.media_dir / ("in-" + platform::secure_random_token(16));
            std::string error;
            if (!transport->download(file, temp, &error)) {
                std::error_code ec;
                std::filesystem::remove(temp, ec);
                reply(inbound, texts::turn_failed(error));
                continue;
            }
            const auto bytes = read_file(temp);
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            const auto name = file.name.empty() ? std::string(file.kind == im::AttachmentKind::Image ? "image.jpg" : "file")
                                                : file.name;
            const auto stored = save_attachment(SessionStorage::get_project_dir(record->cwd), session_id, name,
                                                file.mime_type, bytes, &error);
            if (!stored) {
                reply(inbound, texts::turn_failed(error));
                continue;
            }
            input.content_parts.push_back(attachment_content_part(*stored));
            saved.push_back(attachment_to_json(*stored));
        }
        if (!saved.empty()) input.metadata["attachments"] = saved;
        if (saved.empty() && text.empty()) return;
    }
    if (input.empty()) return;
    if (!deps_.sessions->send_input(session_id, input)) reply(inbound, texts::input_rejected());
}

void Conversations::restore() {
    for (const auto& record : store_.bindings()) {
        if (!transport_->get()) return;
        try {
            ensure_projection(record);
        } catch (const SessionMissing&) {
        } catch (const std::exception& e) {
            LOG_WARN("[channels/" + platform_ + "] restore binding failed: " + e.what());
        }
    }
}

void Conversations::shutdown() {
    std::map<std::string, std::shared_ptr<Projection>> projections;
    {
        std::lock_guard<std::mutex> lock(mu_);
        projections.swap(projections_);
        snapshots_.clear();
    }
    for (auto& [key, projection] : projections) projection->stop();
}

void Conversations::abort_principal(const std::string& principal) {
    for (const auto& record : store_.bindings()) {
        const bool match = principal_for(record.address) == principal ||
                           (principal_kind(principal) == "group" && group_principal(record.address) == principal);
        if (match) deps_.sessions->abort(record.session_id);
    }
}

nlohmann::json Conversations::bindings_json() const {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& record : store_.bindings()) {
        nlohmann::json item{{"key", record.address.key()},
                            {"label", label_for(record.address)},
                            {"chat", record.address.chat},
                            {"sender", record.address.sender},
                            {"session_id", record.session_id},
                            {"no_workspace", record.no_workspace},
                            {"cwd", record.cwd},
                            {"workspace_hash", record.workspace_hash}};
        if (const auto projection = projection_for(record.address.key())) {
            const auto stats = projection->stats();
            item["sent"] = stats.outbound_sent;
            item["failed"] = stats.outbound_failed;
            item["dropped"] = stats.outbound_dropped;
        }
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<std::string> Conversations::bound_session_ids() const {
    std::vector<std::string> ids;
    for (const auto& record : store_.bindings()) ids.push_back(record.session_id);
    return ids;
}

} // namespace acecode::channels::core
