#include "im_channels.hpp"

#include "channels/core/plugin_platforms.hpp"
#include "config/config.hpp"
#include "platform/process/os_process.hpp"
#include "utils/logger.hpp"
#include "remote_control/session_channel_binder.hpp"
#include "session/session_history_page.hpp"
#include "session/session_storage.hpp"
#include "session_host/session_registry.hpp"
#include "skills/opencode_command.hpp"
#include "skills/skill_command_expander.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"
#include "web/handlers/models_handler.hpp"
#include "web/handlers/opencode_command_expander.hpp"
#include "web/session_reference_context.hpp"

#include <mutex>
#include <set>

namespace acecode::daemon {

std::filesystem::path im_channels_root() { return path_from_utf8(get_acecode_dir()) / "channels"; }

std::unique_ptr<channels::core::ChannelHost> make_im_channel_host(ImChannelDeps deps) {
    channels::core::HostServices services;
    auto& conversation = services.conversation;
    auto* registry = deps.registry;
    auto* client = deps.client;
    const auto* config = deps.config;
    auto* config_mu = deps.config_mu;

    conversation.sessions = client;
    conversation.pending_permissions = [registry](const std::string& id) {
        auto entry = registry->acquire(id);
        return entry && entry->prompter ? entry->prompter->snapshot_pending_requests()
                                        : std::vector<nlohmann::json>{};
    };
    conversation.session_cwd = [registry](const std::string& id) {
        auto entry = registry->acquire(id);
        return entry ? entry->cwd : std::string{};
    };
    conversation.catalog = std::move(deps.catalog);
    conversation.resume_target = [client](const rc::RcSessionTarget& target) {
        return rc::resume_session_target_exact(*client, target);
    };
    conversation.model_names = [config, config_mu] {
        std::vector<std::string> names;
        std::shared_lock<std::shared_mutex> lock(*config_mu);
        for (const auto& model : config->saved_models) names.push_back(model.name);
        return names;
    };
    conversation.switch_model = [config, config_mu, registry](const std::string& id, const std::string& name,
                                                              std::string* error) {
        std::optional<ModelProfile> profile;
        {
            std::shared_lock<std::shared_mutex> lock(*config_mu);
            profile = web::find_model_by_name(*config, name);
        }
        if (!profile) {
            if (error) *error = "没有名为 " + name + " 的模型";
            return false;
        }
        return registry->switch_model(id, *profile, nullptr, error);
    };
    conversation.set_permission_mode = [registry](const std::string& id, const std::string& mode) {
        const auto parsed = PermissionManager::parse_mode_name(mode);
        return parsed && registry->set_permission_mode(id, *parsed);
    };
    conversation.model_state = [registry](const std::string& id) { return registry->current_model_state(id); };
    conversation.set_reasoning_effort = [registry](const std::string& id,
                                                  const std::optional<std::string>& effort,
                                                  std::string* error) -> std::optional<SessionModelState> {
        const auto result = registry->set_reasoning_effort(id, effort);
        if (result.status == SessionReasoningStatus::Updated) return result.state;
        if (error) {
            switch (result.status) {
                case SessionReasoningStatus::InvalidEffort: *error = "当前模型不支持该 effort 档位"; break;
                case SessionReasoningStatus::UnknownSession: *error = "会话不存在"; break;
                case SessionReasoningStatus::Unavailable: *error = "当前会话模型不可用"; break;
                default: *error = "更新思考深度失败,原设置已保留"; break;
            }
        }
        return std::nullopt;
    };
    // 与 Web 输入相同的展开顺序:先 opencode 命令,再技能命令。
    conversation.expand_skill = [config, config_mu, registry](const std::string& id, const std::string& text) {
        if (auto entry = registry->acquire(id); entry && !entry->cwd.empty()) {
            std::shared_lock<std::shared_mutex> lock(*config_mu);
            auto command = web::try_expand_opencode_command(text, *config, entry->cwd);
            if (command.expanded) return command.text;
        }
        if (auto skills = registry->skill_registry_snapshot(id)) {
            auto skill = web::try_expand_skill_command(text, *skills);
            if (skill.expanded) return skill.text;
        }
        return text;
    };
    conversation.side_question = [registry](const std::string& id, const std::string& question,
                                            SideChatCancellation& cancellation) {
        // 与 Desktop 侧边对话同一个只读工具循环;IM 不需要流式进度。
        return registry->stream_side_chat(id, question, {}, cancellation, {}, {}).response;
    };
    conversation.list_skills = [config, config_mu, registry](const std::string& id) {
        std::vector<channels::core::SkillListing> listing;
        std::set<std::string> seen;
        if (auto skills = registry->skill_registry_snapshot(id)) {
            for (const auto& skill : skills->list())
                if (seen.insert(skill.name).second) listing.push_back({skill.name, skill.description});
        }
        // 项目命令与技能一样以 /名称 调用(Desktop 下拉里也列在一起)。
        if (auto entry = registry->acquire(id); entry && !entry->no_workspace && !entry->cwd.empty()) {
            AppConfig snapshot;
            {
                std::shared_lock<std::shared_mutex> lock(*config_mu);
                snapshot = *config;
            }
            for (const auto& command : load_opencode_commands(snapshot, entry->cwd))
                if (!is_web_reserved_builtin_command(command.name) && seen.insert(command.name).second)
                    listing.push_back({command.name, command.description});
        }
        return listing;
    };
    conversation.reference_sessions =
        [](const std::string& current_id, const std::vector<rc::RcSessionTarget>& targets, const std::string& text,
           std::string* error) -> std::optional<channels::core::SessionReferenceExpansion> {
        std::vector<web::ResolvedSessionReference> resolved;
        for (const auto& target : targets) {
            if (target.session_id == current_id) continue;
            web::ResolvedSessionReference item;
            item.descriptor.session_id = target.session_id;
            item.descriptor.no_workspace = target.no_workspace;
            item.descriptor.workspace_hash = target.no_workspace ? std::string{} : target.workspace_hash;
            item.descriptor.title = target.title.empty() ? target.summary : target.title;
            item.descriptor.workspace_name = target.no_workspace ? std::string("Task") : target.workspace_label;
            try {
                // 只读最近一页:拼进提示词的部分本来就有上限(每个会话 24 KiB),不必整份读入大会话。
                const auto files = SessionStorage::find_session_files(SessionStorage::get_project_dir(target.cwd),
                                                                      target.session_id);
                if (!files.empty()) {
                    SessionHistoryRequest request;
                    request.limit = kHistoryDefaultLimit;
                    for (auto& row : load_session_history_page(files.front().jsonl_path, request).messages)
                        item.messages.push_back(std::move(row.message));
                }
            } catch (const std::exception& e) {
                LOG_WARN("[channels] cannot read referenced session " + target.session_id + ": " + e.what());
                if (error) *error = "读取会话「" + item.descriptor.title + "」失败";
                return std::nullopt;
            }
            resolved.push_back(std::move(item));
        }
        const auto context = web::build_session_reference_prompt_context(resolved);
        channels::core::SessionReferenceExpansion expansion;
        expansion.text = web::build_session_reference_augmented_prompt(context, text);
        expansion.meta = context.meta;
        return expansion;
    };
    conversation.set_delegate = [registry](const std::string& id, const ChannelDelegate& delegate) {
        if (auto entry = registry->acquire(id); entry && entry->sm) entry->sm->set_channel_delegate(delegate);
    };
    services.broadcast = std::move(deps.broadcast);
    services.current_pid = [] { return static_cast<std::int64_t>(current_pid()); };
    services.endpoints = std::move(deps.endpoints);
    services.bind = std::move(deps.bind);
    services.weixin_login = std::move(deps.weixin_login);
    if (deps.tune_services) deps.tune_services(services);
    const auto root = deps.root.empty() ? im_channels_root() : deps.root;
    return std::make_unique<channels::core::ChannelHost>(root, std::move(services));
}

std::filesystem::path im_channel_plugins_root() { return path_from_utf8(get_acecode_dir()) / "connectors"; }

std::vector<std::string> register_im_channel_plugins(const std::filesystem::path& connectors_root) {
    auto scanned = channels::core::scan_plugin_platforms(connectors_root);
    for (const auto& error : scanned.errors) LOG_WARN("[channels] plugin manifest skipped: " + error);
    const auto names = channels::core::register_plugin_platforms(std::move(scanned.platforms));
    for (const auto& name : names) LOG_INFO("[channels] plugin platform registered: " + name);
    return names;
}

} // namespace acecode::daemon
