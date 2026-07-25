#pragma once

#include "settings_state.hpp"

#include "../../config/config.hpp"

#include <ftxui/component/component_base.hpp>

#include <functional>
#include <memory>
#include <string>

namespace acecode::tui::settings {

struct SettingsCenterDependencies {
    AppConfig* config = nullptr;
    std::string cwd;
    std::string acecode_version;
    std::string acecode_dir_override;
    std::function<void()> request_close;
    std::function<void()> post_event;
    std::function<void(std::function<void()>)> post_to_ui;
    std::function<bool(const std::string& model_name)>
        model_profile_used_by_busy_session;
    std::function<bool(const std::string& session_id)>
        session_is_busy;
    std::function<bool(const std::string& path)>
        reveal_in_file_manager;
};

class SettingsCenter {
public:
    explicit SettingsCenter(SettingsCenterDependencies dependencies);
    ~SettingsCenter();

    SettingsCenter(const SettingsCenter&) = delete;
    SettingsCenter& operator=(const SettingsCenter&) = delete;

    ftxui::Component component() const;
    void open();
    void open(SettingsTab tab);
    SettingsTab active_tab() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode::tui::settings
