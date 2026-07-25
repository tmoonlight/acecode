#include "settings_state.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::tui::settings {
namespace {

template <typename Enum>
std::size_t index_of(Enum value) {
    return static_cast<std::size_t>(value);
}

std::string lower_ascii(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (unsigned char ch : value) {
        result.push_back(static_cast<char>(std::tolower(ch)));
    }
    return result;
}

std::string normalized_slug(std::string_view value) {
    std::string slug = lower_ascii(value);
    for (char& ch : slug) {
        if (ch == '_' || ch == ' ') ch = '-';
    }
    while (!slug.empty() && slug.front() == '/') slug.erase(slug.begin());
    return slug;
}

} // namespace

const std::array<std::string, static_cast<std::size_t>(SettingsTab::Count)>&
settings_tab_labels() {
    static const std::array<
        std::string,
        static_cast<std::size_t>(SettingsTab::Count)> labels = {
        "General",
        "Appearance",
        "Configuration",
        "Personalization",
        "Models",
        "Usage",
        "Archived",
        "About",
    };
    return labels;
}

const std::array<std::string, static_cast<std::size_t>(ManagementTab::Count)>&
management_tab_labels() {
    static const std::array<
        std::string,
        static_cast<std::size_t>(ManagementTab::Count)> labels = {
        "Skills",
        "MCP Servers",
        "Connectors",
        "Tools",
        "Hooks",
    };
    return labels;
}

std::optional<SettingsTab> parse_settings_tab(std::string_view value) {
    const std::string slug = normalized_slug(value);
    if (slug.empty() || slug == "config" || slug == "general") {
        return SettingsTab::General;
    }
    if (slug == "appearance" || slug == "theme") {
        return SettingsTab::Appearance;
    }
    if (slug == "configuration" || slug == "upgrade") {
        return SettingsTab::Configuration;
    }
    if (slug == "personalization" || slug == "instructions") {
        return SettingsTab::Personalization;
    }
    if (slug == "models" || slug == "model") return SettingsTab::Models;
    if (slug == "usage") return SettingsTab::Usage;
    if (slug == "archived" || slug == "archive") {
        return SettingsTab::Archived;
    }
    if (slug == "about") return SettingsTab::About;
    return std::nullopt;
}

std::optional<ManagementTab> parse_management_tab(std::string_view value) {
    const std::string slug = normalized_slug(value);
    if (slug == "skills" || slug == "skill") return ManagementTab::Skills;
    if (slug == "mcp" || slug == "mcp-servers" || slug == "mcpservers") {
        return ManagementTab::McpServers;
    }
    if (slug == "connectors" || slug == "connector") {
        return ManagementTab::Connectors;
    }
    if (slug == "tools" || slug == "tool") return ManagementTab::Tools;
    if (slug == "hooks" || slug == "hook") return ManagementTab::Hooks;
    return std::nullopt;
}

std::string settings_tab_slug(SettingsTab tab) {
    switch (tab) {
        case SettingsTab::General: return "general";
        case SettingsTab::Appearance: return "appearance";
        case SettingsTab::Configuration: return "configuration";
        case SettingsTab::Personalization: return "personalization";
        case SettingsTab::Models: return "models";
        case SettingsTab::Usage: return "usage";
        case SettingsTab::Archived: return "archived";
        case SettingsTab::About: return "about";
        case SettingsTab::Count: break;
    }
    return "general";
}

std::string management_tab_slug(ManagementTab tab) {
    switch (tab) {
        case ManagementTab::Skills: return "skills";
        case ManagementTab::McpServers: return "mcp";
        case ManagementTab::Connectors: return "connectors";
        case ManagementTab::Tools: return "tools";
        case ManagementTab::Hooks: return "hooks";
        case ManagementTab::Count: break;
    }
    return "skills";
}

std::string footer_action_label(FooterAction action) {
    switch (action) {
        case FooterAction::Filter: return "filter";
        case FooterAction::Save: return "save";
        case FooterAction::Add: return "add";
        case FooterAction::Edit: return "edit";
        case FooterAction::Delete: return "delete";
        case FooterAction::SetDefault: return "set default";
        case FooterAction::Toggle: return "enable/disable";
        case FooterAction::Refresh: return "refresh";
        case FooterAction::Reload: return "reload";
        case FooterAction::Reconnect: return "reconnect";
        case FooterAction::Details: return "details";
        case FooterAction::Restore: return "restore";
        case FooterAction::Purge: return "purge";
        case FooterAction::Copy: return "copy";
        case FooterAction::Open: return "open";
        case FooterAction::Trust: return "trust";
        case FooterAction::Close: return "close";
    }
    return {};
}

bool search_matches(
    std::string_view query,
    const std::vector<std::string_view>& fields) {
    const std::string needle = lower_ascii(query);
    if (needle.empty()) return true;
    for (std::string_view field : fields) {
        if (lower_ascii(field).find(needle) != std::string::npos) return true;
    }
    return false;
}

std::vector<FooterAction> settings_footer_actions(
    SettingsTab tab,
    bool dirty) {
    std::vector<FooterAction> actions;
    switch (tab) {
        case SettingsTab::General:
        case SettingsTab::Appearance:
            break;
        case SettingsTab::Configuration:
        case SettingsTab::Personalization:
            if (dirty) actions.push_back(FooterAction::Save);
            break;
        case SettingsTab::Models:
            actions = {
                FooterAction::Filter,
                FooterAction::Add,
                FooterAction::Edit,
                FooterAction::SetDefault,
                FooterAction::Delete,
            };
            break;
        case SettingsTab::Usage:
            actions = {FooterAction::Refresh};
            break;
        case SettingsTab::Archived:
            actions = {
                FooterAction::Filter,
                FooterAction::Restore,
                FooterAction::Purge,
                FooterAction::Refresh,
            };
            break;
        case SettingsTab::About:
            actions = {FooterAction::Copy, FooterAction::Open};
            break;
        case SettingsTab::Count:
            break;
    }
    actions.push_back(FooterAction::Close);
    return actions;
}

std::vector<FooterAction> management_footer_actions(
    ManagementTab tab,
    const ManagementRowCapabilities& capabilities) {
    std::vector<FooterAction> actions = {FooterAction::Filter};
    if (capabilities.toggle) actions.push_back(FooterAction::Toggle);
    if (capabilities.edit) actions.push_back(FooterAction::Edit);
    if (capabilities.remove) actions.push_back(FooterAction::Delete);
    if (capabilities.reconnect) actions.push_back(FooterAction::Reconnect);
    if (capabilities.trust) actions.push_back(FooterAction::Trust);
    if (capabilities.open) actions.push_back(FooterAction::Open);
    actions.push_back(FooterAction::Details);
    if (tab == ManagementTab::Skills ||
        tab == ManagementTab::McpServers ||
        tab == ManagementTab::Hooks) {
        actions.push_back(FooterAction::Reload);
    } else if (tab == ManagementTab::Connectors) {
        actions.push_back(FooterAction::Refresh);
    }
    actions.push_back(FooterAction::Close);
    return actions;
}

void SettingsNavigationModel::set_active_tab_immediately(SettingsTab tab) {
    active_tab_ = tab;
    pending_tab_.reset();
    close_requested_ = false;
}

PageNavigationState& SettingsNavigationModel::page(SettingsTab tab) {
    return pages_[index_of(tab)];
}

const PageNavigationState& SettingsNavigationModel::page(SettingsTab tab) const {
    return pages_[index_of(tab)];
}

NavigationResult SettingsNavigationModel::request_tab(SettingsTab tab) {
    close_requested_ = false;
    if (tab == active_tab_) return NavigationResult::Unchanged;
    if (dirty_) {
        pending_tab_ = tab;
        return NavigationResult::NeedsDiscardConfirmation;
    }
    active_tab_ = tab;
    return NavigationResult::Changed;
}

NavigationResult SettingsNavigationModel::request_close() {
    if (dirty_) {
        close_requested_ = true;
        pending_tab_.reset();
        return NavigationResult::NeedsDiscardConfirmation;
    }
    close_requested_ = true;
    return NavigationResult::Changed;
}

bool SettingsNavigationModel::discard_and_continue() {
    if (!dirty_) return false;
    dirty_ = false;
    if (pending_tab_) {
        active_tab_ = *pending_tab_;
        pending_tab_.reset();
    }
    return true;
}

bool SettingsNavigationModel::save_and_continue() {
    dirty_ = false;
    if (pending_tab_) {
        active_tab_ = *pending_tab_;
        pending_tab_.reset();
    }
    return true;
}

void SettingsNavigationModel::cancel_pending_navigation() {
    pending_tab_.reset();
    close_requested_ = false;
}

PageNavigationState& ManagementNavigationModel::page(ManagementTab tab) {
    return pages_[index_of(tab)];
}

const PageNavigationState& ManagementNavigationModel::page(
    ManagementTab tab) const {
    return pages_[index_of(tab)];
}

} // namespace acecode::tui::settings
