#include "memory_settings_section.hpp"

#include "config/settings_mutations.hpp"
#include "tui/theme_palette.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace acecode::tui::settings {

using namespace ftxui;

struct MemorySettingsSection::State {
    AppConfig* config = nullptr;
    std::function<void()> published;
    bool enabled = true;
    bool summary_enabled = false;
    std::vector<std::string> model_entries;  // [0] = 当前模型,其余为已保存的模型名
    int model_index = 0;
    std::string status;
    bool status_error = false;
    Component use_checkbox;
    Component summary_checkbox;
    Component model_radio;
    Component container;

    void sync() {
        if (!config) return;
        enabled = config->memory.enabled;
        summary_enabled = config->memory.summary.enabled;
        model_entries = {"Current model (the model each session last used)"};
        model_index = 0;
        for (const auto& profile : config->saved_models) {
            model_entries.push_back(profile.name);
            if (profile.name == config->memory.summary.model_name) {
                model_index = static_cast<int>(model_entries.size()) - 1;
            }
        }
    }

    // 立即保存;失败时控件回到磁盘上的值并显示原因。
    void save() {
        if (!config) return;
        MemoryConfig next = config->memory;
        next.enabled = enabled;
        next.summary.enabled = summary_enabled;
        next.summary.model_name =
            model_index > 0 && model_index < static_cast<int>(model_entries.size())
                ? model_entries[static_cast<std::size_t>(model_index)]
                : std::string{};
        SettingsMutationOptions options;
        options.live_config = config;
        options.on_live_config_published = published;
        options.restart_required_without_live_apply = false;
        const auto result = set_memory_settings(next, options);
        status_error = !result.ok;
        status = result.ok ? (result.changed ? "Memory settings saved." : "No changes to save.")
                           : result.error;
        if (!result.ok) sync();
    }
};

namespace {

Element radio_entry(const EntryState& entry) {
    Element line = text((entry.state ? "(*) " : "( ) ") + entry.label);
    return entry.focused ? line | bgcolor(theme().ui.selection_bg) | color(theme().ui.selection_fg)
                         : line | color(theme().ui.text_muted);
}

} // namespace

MemorySettingsSection::MemorySettingsSection(AppConfig* config, std::function<void()> published)
    : state_(std::make_shared<State>()) {
    state_->config = config;
    state_->published = std::move(published);
    state_->sync();

    CheckboxOption use_option;
    use_option.label = "Use memory (inject saved memory into new sessions and provide memory tools)";
    use_option.checked = &state_->enabled;
    use_option.on_change = [weak = std::weak_ptr<State>(state_)] {
        if (auto state = weak.lock()) state->save();
    };
    state_->use_checkbox = Checkbox(use_option);

    CheckboxOption summary_option;
    summary_option.label =
        "Memory summarization (extract memories from idle sessions; makes extra model calls)";
    summary_option.checked = &state_->summary_enabled;
    summary_option.on_change = [weak = std::weak_ptr<State>(state_)] {
        if (auto state = weak.lock()) state->save();
    };
    state_->summary_checkbox = Checkbox(summary_option);

    RadioboxOption model_option;
    model_option.entries = &state_->model_entries;
    model_option.selected = &state_->model_index;
    model_option.on_change = [weak = std::weak_ptr<State>(state_)] {
        if (auto state = weak.lock()) state->save();
    };
    model_option.transform = radio_entry;
    state_->model_radio = Radiobox(model_option);

    state_->container = Container::Vertical({
        state_->use_checkbox,
        state_->summary_checkbox,
        state_->model_radio,
    });
}

MemorySettingsSection::~MemorySettingsSection() = default;

Component MemorySettingsSection::component() const { return state_->container; }

Element MemorySettingsSection::render() const {
    const auto& s = *state_;
    Elements rows = {
        separator() | color(theme().ui.text_dim),
        text("Memory") | bold | color(theme().ui.text_primary),
        paragraph("Global memory holds your preferences across projects; workspace memory "
                  "belongs to this workspace. Use /memory to list, view or forget entries.") |
            color(theme().ui.text_secondary),
        s.use_checkbox->Render(),
        s.summary_checkbox->Render(),
        text("Summary model") | bold | color(theme().ui.text_primary),
        s.model_radio->Render(),
    };
    if (!s.enabled) {
        rows.push_back(paragraph("Memory is off: new sessions get no memory context or memory "
                                 "tools, and summarization does not run. Entries stay on disk.") |
                       color(theme().semantic.warning));
    }
    if (!s.status.empty()) {
        rows.push_back(text(s.status) |
                       color(s.status_error ? theme().semantic.error : theme().semantic.success));
    }
    return vbox(std::move(rows));
}

void MemorySettingsSection::sync_from_config() { state_->sync(); }

} // namespace acecode::tui::settings
