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

std::string permission_label(const std::string& mode) {
    if (mode == "default") return "默认权限 (default)";
    if (mode == "auto" || mode == "accept-edits") return "自动模式 (auto)";
    if (mode == "yolo") return "完全访问权限 (yolo)";
    if (mode == "plan") return "计划模式 (plan)";
    return mode;
}

std::string effort_status(const std::optional<SessionModelState>& state, bool list_options) {
    if (!state || state->deleted) return "思考深度:模型不可用";
    if (!state->reasoning || !state->reasoning->supported ||
        !(state->reasoning->mandatory || state->reasoning->enabled.value_or(state->reasoning->default_enabled)) ||
        state->reasoning->supported_efforts.empty() ||
        (state->provider != "openai" && state->provider != "anthropic"))
        return "思考深度:当前模型未启用或不支持 effort 档位";
    const auto& reasoning = *state->reasoning;
    std::string text = "思考深度:" + state->reasoning_effort.value_or(
        reasoning.effort.value_or(reasoning.default_effort.value_or("默认")));
    if (!state->reasoning_effort) text += " (模型预设)";
    if (list_options) {
        text += "\n可用档位:";
        for (const auto& effort : reasoning.supported_efforts) text += " " + effort;
        text += "\n用法:/effort <档位>,/effort default 恢复模型预设。";
    }
    return text;
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
    // 本人账号模型的群消息只进群聊代答缓冲:不解析命令、不走配对审批,不在群里回复。
    if (const auto* spec = find_platform_spec(platform_);
        spec && spec->owner_binding == OwnerBinding::Self && inbound.address.kind == im::ChatKind::Group) {
        ingest_group(inbound);
        return outcome;
    }
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
        case CommandKind::RefList: show_session_list(inbound, owner, command.argument, true); return;
        case CommandKind::More: more(inbound, command.argument); return;
        case CommandKind::Resume: resume(inbound, owner, command.argument); return;
        case CommandKind::New: new_session(inbound); return;
        case CommandKind::None: submit(inbound); return;
        default: handle_bound_command(inbound, command, owner); return;
    }
}

std::optional<SessionInfo> Conversations::session_info(const std::string& session_id) const {
    for (auto& info : deps_.sessions->list_sessions())
        if (info.id == session_id) return info;
    return std::nullopt;
}

void Conversations::handle_bound_command(const im::Inbound& inbound, const Command& command, bool owner) {
    const auto record = store_.binding(inbound.address.key());
    if (!record) {
        reply(inbound, texts::no_binding());
        return;
    }
    const auto projection = ensure_projection(*record);
    const auto& id = record->session_id;
    if (*builtin_command_name(command.kind)) {
        // 全局记忆属于机主:其他联系人看不到也改不了。
        if (command.kind == CommandKind::Memory && !owner) {
            reply(inbound, "只有机主可以管理记忆。");
            return;
        }
        run_builtin(inbound, id, projection, command);
        return;
    }
    switch (command.kind) {
        case CommandKind::Turn: steer(inbound, id, command.argument); return;
        case CommandKind::Side: ask_side(inbound, id, command.argument); return;
        case CommandKind::Skills: list_skills(inbound, id, command.argument); return;
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
            text += "\n权限模式:" + (info ? permission_label(info->permission_mode) : "不可用");
            text += "\n" + effort_status(deps_.model_state ? deps_.model_state(id) : std::nullopt, false);
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
        case CommandKind::Mode:
        case CommandKind::Yolo: {
            if (command.kind == CommandKind::Mode && command.argument.empty()) {
                const auto info = session_info(id);
                reply(inbound, "权限模式:" + (info ? permission_label(info->permission_mode) : "不可用") +
                               "\n用法:/mode default|auto|yolo,或 /yolo 直接切换到完全访问权限。");
                return;
            }
            const auto mode = command.kind == CommandKind::Yolo ? std::string("yolo") : command.argument;
            if (!deps_.set_permission_mode || !deps_.set_permission_mode(id, mode)) {
                reply(inbound, texts::turn_failed("切换权限模式失败"));
                return;
            }
            reply(inbound, "已切换权限模式为 " + permission_label(mode) + "。");
            return;
        }
        case CommandKind::Effort: {
            if (command.argument.empty()) {
                reply(inbound, effort_status(deps_.model_state ? deps_.model_state(id) : std::nullopt, true));
                return;
            }
            const auto effort = command.argument == "default" ? std::nullopt
                                                               : std::optional<std::string>(command.argument);
            std::string error;
            const auto updated = deps_.set_reasoning_effort ? deps_.set_reasoning_effort(id, effort, &error)
                                                           : std::nullopt;
            if (!updated) {
                reply(inbound, texts::turn_failed(error.empty() ? "切换思考深度失败" : error) + "\n" +
                               effort_status(deps_.model_state ? deps_.model_state(id) : std::nullopt, true));
                return;
            }
            reply(inbound, std::string(effort ? "已更新思考深度。" : "已恢复模型预设思考深度。") +
                           "从下一次模型请求起生效。\n" + effort_status(updated, false));
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
    show_session_list(inbound, owner, argument, false);
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
            const auto it = session_lists_.find(key);
            if (it != session_lists_.end()) snapshot = it->second.targets;
        }
        if (snapshot.empty()) snapshot = build_session_list(key, owner, std::nullopt, false).targets;
        target = rc::select_rc_session_snapshot(snapshot, std::stoul(argument));
    } else {
        for (const auto& candidate : visible_sessions(key, true, std::nullopt))
            if (candidate.session_id == argument) target = candidate;
    }
    if (!target) {
        reply(inbound, "没有找到这个会话。先发送 /s 查看可切换的会话。");
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
        if (record.delegate) {
            // 代答会话被别处接管:群里不发任何通知,下次汇总时为该群另建会话。
            drop_delegate_binding(record);
            released = true;
            continue;
        }
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
    std::vector<rc::RcSessionTarget> referenced;
    if (!resolve_session_mentions(inbound, session_id, text, referenced)) return;
    const auto display = text;  // @编号 已换成会话标题
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
            if (!referenced.empty()) {
                const auto expanded = with_references(inbound, session_id, referenced, text);
                if (!expanded) return;
                interjection.text = expanded->text;
                interjection.display_text = display;
                interjection.metadata["session_references"] = expanded->meta;
                interjection.metadata["display_text"] = display;
            }
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
    if (!referenced.empty()) {
        // 与 Web 相同:技能命令展开之后,再把被引用会话的上下文拼在前面。
        const auto expanded = with_references(inbound, session_id, referenced, input.text);
        if (!expanded) return;
        input.text = expanded->text;
        input.display_text = display;
        input.metadata["session_references"] = expanded->meta;
        input.metadata["display_text"] = display;
    }
    if (!files.empty()) {
        const auto transport = transport_->get();
        if (!transport) throw std::runtime_error("通道未连接");
        if (!input.text.empty()) input.content_parts.push_back({{"type", "text"}, {"text", input.text}});
        nlohmann::json saved = nlohmann::json::array();
        std::filesystem::create_directories(deps_.media_dir);
        for (const auto& file : files) {
            const auto temp = deps_.media_dir / ("in_" + platform::secure_random_token(16));
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
        if (record.delegate) continue;  // 代答会话没有出站投影:回复正文绝不发到群里
        try {
            ensure_projection(record);
        } catch (const SessionMissing&) {
        } catch (const std::exception& e) {
            LOG_WARN("[channels/" + platform_ + "] restore binding failed: " + e.what());
        }
    }
}

void Conversations::shutdown() {
    stop_tasks();
    std::map<std::string, std::shared_ptr<Projection>> projections;
    {
        std::lock_guard<std::mutex> lock(mu_);
        projections.swap(projections_);
        session_lists_.clear();
        ref_lists_.clear();
        pagers_.clear();
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
        if (record.delegate) {
            item["delegate"] = true;
            item["chat_name"] = record.chat_name;
        }
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

// ---- 群聊代答(D5) ----

std::chrono::steady_clock::time_point Conversations::now() const {
    return deps_.clock ? deps_.clock() : std::chrono::steady_clock::now();
}

void Conversations::ingest_group(const im::Inbound& inbound) {
    const auto config = store_.config();
    const auto& chat = inbound.address.chat;
    if (!config.group_watch.watches(chat)) {
        LOG_DEBUG("[channels/" + platform_ + "] group " + chat + " is not watched, ignored");
        return;
    }
    GroupMessage message;
    message.message_id = inbound.message_id;
    message.at_ms = inbound.timestamp_ms > 0 ? inbound.timestamp_ms : now_wall_ms();
    message.sender = inbound.address.sender;
    message.sender_name = inbound.sender_name;
    message.text = inbound.text;
    for (const auto& attachment : inbound.attachments) {
        const char* label = attachment.kind == im::AttachmentKind::Image ? "[图片]"
                            : attachment.kind == im::AttachmentKind::Voice
                                ? "[语音]"
                                : (attachment.kind == im::AttachmentKind::Video ? "[视频]" : "[文件]");
        message.text += (message.text.empty() ? "" : " ") + std::string(label);
        if (!attachment.transcript.empty()) message.text += " " + attachment.transcript;
    }
    if (!inbound.quote_text.empty()) message.text = "[引用:" + inbound.quote_text + "] " + message.text;
    if (message.text.empty()) return;
    message.from_self = inbound.from_self;
    message.mentioned = inbound.mentioned && !inbound.from_self;
    std::lock_guard<std::mutex> lock(groups_mu_);
    auto [it, created] = groups_.try_emplace(chat);
    if (created) it->second.last_flush = now();
    if (!inbound.chat_name.empty()) it->second.chat_name = inbound.chat_name;
    it->second.add(std::move(message));
}

ChannelDelegate Conversations::delegate_for(const BindingRecord& record, const GroupWatchSettings& settings) const {
    ChannelDelegate delegate;
    delegate.platform = platform_;
    delegate.chat = record.address.chat;
    delegate.chat_name = record.chat_name;
    delegate.prompt = settings.prompt;
    if (const auto transport = transport_->get()) delegate.user_name = transport->status().display_name;
    return delegate;
}

std::optional<BindingRecord> Conversations::delegate_binding_for_session(const std::string& session_id) const {
    for (const auto& record : store_.bindings())
        if (record.delegate && record.session_id == session_id) return record;
    return std::nullopt;
}

void Conversations::drop_delegate_binding(const BindingRecord& record) {
    store_.erase_binding(record.address.key());
    // 先确保会话已载入,meta 里的代答身份才能清掉;否则以后打开它仍按代答会话收窄工具。
    rc::RcSessionTarget target;
    target.session_id = record.session_id;
    target.cwd = record.cwd;
    target.workspace_hash = record.workspace_hash;
    target.no_workspace = record.no_workspace;
    if (deps_.set_delegate && deps_.resume_target && deps_.resume_target(target))
        deps_.set_delegate(record.session_id, ChannelDelegate{});
    {
        std::lock_guard<std::mutex> lock(groups_mu_);
        group_sends_.erase(record.session_id);
    }
    if (deps_.on_bindings_changed) deps_.on_bindings_changed();
}

BindingRecord Conversations::ensure_delegate_binding(const std::string& account, const std::string& chat,
                                                     const std::string& chat_name) {
    im::Address address;
    address.platform = platform_;
    address.account = account;
    address.kind = im::ChatKind::Group;
    address.chat = chat;
    address.sender = account;  // 群级会话键:与普通群聊的“群 + 发言人”键不冲突
    if (auto existing = store_.binding(address.key()); existing && existing->delegate) {
        rc::RcSessionTarget target;
        target.session_id = existing->session_id;
        target.cwd = existing->cwd;
        target.workspace_hash = existing->workspace_hash;
        target.no_workspace = existing->no_workspace;
        if (deps_.resume_target && deps_.resume_target(target)) {
            if (!chat_name.empty() && existing->chat_name != chat_name) {
                existing->chat_name = chat_name;
                store_.put_binding(*existing);
            }
            return *existing;
        }
        LOG_INFO("[channels/" + platform_ + "] delegate session of group " + chat + " is gone, creating a new one");
        drop_delegate_binding(*existing);
    }
    auto record = create_binding(address, std::nullopt);
    record.delegate = true;
    record.chat_name = chat_name;
    store_.put_binding(record);
    LOG_INFO("[channels/" + platform_ + "] group " + chat + " bound to delegate session " + record.session_id);
    if (deps_.on_bindings_changed) deps_.on_bindings_changed();
    return record;
}

void Conversations::poll_groups() {
    const auto current = now();
    {
        std::lock_guard<std::mutex> lock(groups_mu_);
        if (current < next_poll_) return;
        next_poll_ = current + std::chrono::seconds(5);
    }
    const auto transport = transport_->get();
    if (!transport) return;
    const auto status = transport->status();
    if (status.state != im::LinkState::Connected || status.account.empty()) return;
    const auto config = store_.config();
    const auto interval = std::chrono::seconds(config.group_watch.poll_interval_s);

    for (const auto& chat : config.group_watch.groups) {
        std::deque<GroupMessage> messages;
        std::size_t omitted = 0;
        std::string chat_name;
        {
            std::lock_guard<std::mutex> lock(groups_mu_);
            auto [it, created] = groups_.try_emplace(chat);
            auto& buffer = it->second;
            if (created) buffer.last_flush = current;
            if (!buffer.has_others() || current - buffer.last_flush < interval) continue;
            messages = buffer.messages();
            omitted = buffer.omitted();
            chat_name = buffer.chat_name;
        }
        try {
            const auto record = ensure_delegate_binding(status.account, chat, chat_name);
            const auto info = session_info(record.session_id);
            if (info && info->busy) continue;  // 会话还在处理上一批:等下一次
            if (deps_.set_delegate) deps_.set_delegate(record.session_id, delegate_for(record, config.group_watch));
            const int offset = deps_.utc_offset_minutes ? deps_.utc_offset_minutes() : local_utc_offset_minutes();
            UserInput input;
            input.text = build_group_digest(record.chat_name, messages, omitted, offset);
            input.display_text = group_digest_display(record.chat_name, messages.size());
            input.metadata["channel"] = {{"platform", platform_},
                                         {"address", record.address.to_json()},
                                         {"group_watch", true}};
            {
                std::lock_guard<std::mutex> lock(groups_mu_);
                group_sends_[record.session_id] = 0;
            }
            if (!deps_.sessions->send_input(record.session_id, input)) {
                LOG_WARN("[channels/" + platform_ + "] group digest rejected by session " + record.session_id);
                continue;
            }
            LOG_INFO("[channels/" + platform_ + "] group " + chat + " digest submitted (" +
                     std::to_string(messages.size()) + " message(s))");
            std::lock_guard<std::mutex> lock(groups_mu_);
            auto& buffer = groups_[chat];
            buffer.consume(messages.size());
            buffer.last_flush = current;
            buffer.flushed_once = true;
        } catch (const std::exception& e) {
            LOG_WARN("[channels/" + platform_ + "] group " + chat + " digest failed: " + e.what());
        }
    }
}

void Conversations::apply_group_settings(const GroupWatchSettings& previous, const GroupWatchSettings& next) {
    for (const auto& record : store_.bindings()) {
        if (!record.delegate) continue;
        if (!next.watches(record.address.chat)) {
            LOG_INFO("[channels/" + platform_ + "] group " + record.address.chat + " no longer watched");
            drop_delegate_binding(record);
            continue;
        }
        if (previous.prompt != next.prompt && deps_.set_delegate) {
            rc::RcSessionTarget target;
            target.session_id = record.session_id;
            target.cwd = record.cwd;
            target.workspace_hash = record.workspace_hash;
            target.no_workspace = record.no_workspace;
            if (deps_.resume_target && deps_.resume_target(target))
                deps_.set_delegate(record.session_id, delegate_for(record, next));
        }
    }
    std::lock_guard<std::mutex> lock(groups_mu_);
    for (auto it = groups_.begin(); it != groups_.end();) {
        if (next.watches(it->first)) ++it;
        else it = groups_.erase(it);
    }
}

std::optional<GroupSendResult> Conversations::send_to_group(const std::string& session_id, const std::string& text) {
    const auto record = delegate_binding_for_session(session_id);
    if (!record) return std::nullopt;
    GroupSendResult result;
    std::size_t chars = 0;
    for (const unsigned char c : text)
        if ((c & 0xC0) != 0x80) ++chars;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        result.error = "消息内容为空";
        return result;
    }
    if (chars > kMaxGroupSendChars) {
        result.error = "单条消息最多 2000 字,请精简后再发";
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(groups_mu_);
        if (group_sends_[session_id] >= kMaxGroupSendsPerDigest) {
            result.error = "本次汇总已经发了 5 条消息,等下一次群消息汇总再发";
            return result;
        }
    }
    const auto transport = transport_->get();
    if (!transport || transport->status().state != im::LinkState::Connected) {
        result.error = "消息通道未连接";
        return result;
    }
    const auto prefix = store_.config().group_watch.prefix;
    const auto sent = transport->send_text(record->address, prefix + text, nlohmann::json::object());
    if (!sent.ok()) {
        result.error = sent.error.empty() ? std::string("发送失败") : sent.error;
        LOG_WARN("[channels/" + platform_ + "] group message to " + record->address.chat + " failed: " + result.error);
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(groups_mu_);
        ++group_sends_[session_id];
    }
    LOG_INFO("[channels/" + platform_ + "] group message sent to " + record->address.chat + " (" +
             std::to_string(text.size()) + " bytes)");
    result.ok = true;
    return result;
}

} // namespace acecode::channels::core
