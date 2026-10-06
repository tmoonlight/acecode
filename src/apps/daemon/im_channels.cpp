#include "im_channels.hpp"

#include "config/config.hpp"
#include "platform/process/os_process.hpp"
#include "remote_control/session_channel_binder.hpp"
#include "session_host/session_registry.hpp"
#include "skills/skill_command_expander.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"
#include "web/handlers/models_handler.hpp"
#include "web/handlers/opencode_command_expander.hpp"

#include <mutex>

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
    services.broadcast = std::move(deps.broadcast);
    services.current_pid = [] { return static_cast<std::int64_t>(current_pid()); };
    services.endpoints = std::move(deps.endpoints);
    services.bind = std::move(deps.bind);
    services.weixin_login = std::move(deps.weixin_login);
    if (deps.tune_services) deps.tune_services(services);
    const auto root = deps.root.empty() ? im_channels_root() : deps.root;
    return std::make_unique<channels::core::ChannelHost>(root, std::move(services));
}

} // namespace acecode::daemon
