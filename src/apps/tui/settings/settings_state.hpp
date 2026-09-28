#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acecode::tui::settings {

enum class RootSurface {
    Chat,
    Settings,
    Management,
};

enum class SettingsTab {
    General,
    Appearance,
    Configuration,
    Personalization,
    Models,
    Usage,
    Archived,
    About,
    Count,
};

enum class ManagementTab {
    Skills,
    McpServers,
    Connectors,
    Tools,
    Hooks,
    Count,
};

enum class FooterAction {
    Filter,
    Save,
    Add,
    Edit,
    Delete,
    SetDefault,
    Toggle,
    Refresh,
    Reload,
    Reconnect,
    Details,
    Restore,
    Purge,
    Copy,
    Open,
    Trust,
    Close,
};

enum class NavigationResult {
    Changed,
    Unchanged,
    NeedsDiscardConfirmation,
};

struct PageNavigationState {
    std::string filter;
    int selected = 0;
    int scroll_offset = 0;
    bool filter_focused = false;
};

struct ManagementRowCapabilities {
    bool toggle = false;
    bool edit = false;
    bool remove = false;
    bool reconnect = false;
    bool trust = false;
    bool open = false;
};

const std::array<std::string, static_cast<std::size_t>(SettingsTab::Count)>&
settings_tab_labels();

const std::array<std::string, static_cast<std::size_t>(ManagementTab::Count)>&
management_tab_labels();

std::optional<SettingsTab> parse_settings_tab(std::string_view value);
std::optional<ManagementTab> parse_management_tab(std::string_view value);

std::string settings_tab_slug(SettingsTab tab);
std::string management_tab_slug(ManagementTab tab);
std::string footer_action_label(FooterAction action);

bool search_matches(
    std::string_view query,
    const std::vector<std::string_view>& fields);

std::vector<FooterAction> settings_footer_actions(
    SettingsTab tab,
    bool dirty);

std::vector<FooterAction> management_footer_actions(
    ManagementTab tab,
    const ManagementRowCapabilities& capabilities);

class SettingsNavigationModel {
public:
    SettingsTab active_tab() const { return active_tab_; }
    void set_active_tab_immediately(SettingsTab tab);

    PageNavigationState& page(SettingsTab tab);
    const PageNavigationState& page(SettingsTab tab) const;

    bool dirty() const { return dirty_; }
    void set_dirty(bool dirty) { dirty_ = dirty; }

    NavigationResult request_tab(SettingsTab tab);
    NavigationResult request_close();
    bool discard_and_continue();
    bool save_and_continue();
    void cancel_pending_navigation();
    bool close_requested() const { return close_requested_; }

private:
    SettingsTab active_tab_ = SettingsTab::General;
    std::array<PageNavigationState, static_cast<std::size_t>(SettingsTab::Count)>
        pages_{};
    bool dirty_ = false;
    bool close_requested_ = false;
    std::optional<SettingsTab> pending_tab_;
};

class ManagementNavigationModel {
public:
    ManagementTab active_tab() const { return active_tab_; }
    void set_active_tab(ManagementTab tab) { active_tab_ = tab; }

    PageNavigationState& page(ManagementTab tab);
    const PageNavigationState& page(ManagementTab tab) const;

private:
    ManagementTab active_tab_ = ManagementTab::Skills;
    std::array<PageNavigationState, static_cast<std::size_t>(ManagementTab::Count)>
        pages_{};
};

} // namespace acecode::tui::settings
