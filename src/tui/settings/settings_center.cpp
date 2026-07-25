#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include "settings_center.hpp"

#include "../../config/request_headers.hpp"
#include "../../config/settings_mutations.hpp"
#include "../../desktop/workspace_registry.hpp"
#include "../../network/proxy_resolver.hpp"
#include "../../provider/auth/github_auth.hpp"
#include "../../session/session_storage.hpp"
#include "../../session/session_usage_ledger.hpp"
#include "../../utils/clipboard.hpp"
#include "../../utils/utf8_path.hpp"
#include "../theme_palette.hpp"

#include <cpr/cpr.h>
#include <cpr/ssl_options.h>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/canvas.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>
#  ifdef RGB
#    undef RGB
#  endif
#else
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace acecode::tui::settings {
namespace {

namespace fs = std::filesystem;
using namespace ftxui;

constexpr const char* kProjectUrl =
    "https://github.com/shaohaozhi286/acecode";
constexpr const char* kFtxuiVersion = "6.1.9";

std::string trim_ascii(std::string value) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(
        value.begin(),
        std::find_if(value.begin(), value.end(),
                     [&](unsigned char ch) { return !is_space(ch); }));
    value.erase(
        std::find_if(value.rbegin(), value.rend(),
                     [&](unsigned char ch) { return !is_space(ch); })
            .base(),
        value.end());
    return value;
}

std::string lower_ascii(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string truncate_middle(const std::string& value, std::size_t limit) {
    if (value.size() <= limit || limit < 7) return value;
    const std::size_t head = (limit - 3) / 2;
    const std::size_t tail = limit - 3 - head;
    return value.substr(0, head) + "..." + value.substr(value.size() - tail);
}

std::string format_tokens(std::int64_t value) {
    std::ostringstream out;
    if (value >= 1000000) {
        out << std::fixed << std::setprecision(1)
            << static_cast<double>(value) / 1000000.0 << "M";
    } else if (value >= 1000) {
        out << std::fixed << std::setprecision(1)
            << static_cast<double>(value) / 1000.0 << "K";
    } else {
        out << value;
    }
    return out.str();
}

std::string join_strings(const std::vector<std::string>& values,
                         const std::string& separator) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << separator;
        out << values[i];
    }
    return out.str();
}

std::vector<std::string> split_capabilities(const std::string& value) {
    std::vector<std::string> result;
    std::set<std::string> seen;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        std::string item = trim_ascii(value.substr(
            start,
            comma == std::string::npos ? std::string::npos : comma - start));
        if (!item.empty() && seen.insert(item).second) {
            result.push_back(std::move(item));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return result;
}

bool parse_optional_positive_int(const std::string& text,
                                 std::optional<int>& out) {
    const std::string value = trim_ascii(text);
    if (value.empty()) {
        out.reset();
        return true;
    }
    try {
        std::size_t consumed = 0;
        const long parsed = std::stol(value, &consumed);
        if (consumed != value.size() || parsed <= 0 ||
            parsed > static_cast<long>((std::numeric_limits<int>::max)())) {
            return false;
        }
        out = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

std::map<std::string, std::string> parse_headers_json(
    const std::string& text,
    std::string& error) {
    std::map<std::string, std::string> result;
    const std::string value = trim_ascii(text);
    if (value.empty()) return result;
    try {
        const auto json = nlohmann::json::parse(value);
        auto parsed = parse_request_headers_json(json, "TUI model editor", error);
        if (!parsed.has_value()) return {};
        return std::move(*parsed);
    } catch (const std::exception& e) {
        error = std::string("request headers must be a JSON object: ") + e.what();
        return {};
    }
}

std::string headers_to_json(
    const std::map<std::string, std::string>& headers) {
    if (headers.empty()) return "{}";
    return nlohmann::json(headers).dump(2);
}

std::string permission_description(int index) {
    static const std::array<const char*, 4> descriptions = {
        "Ask before tools that can change files or run commands.",
        "Approve file edits while keeping command permissions interactive.",
        "Restrict the next session to planning and read-only exploration.",
        "Allow all tools without confirmation. Use only in trusted workspaces.",
    };
    index = std::clamp(index, 0, 3);
    return descriptions[static_cast<std::size_t>(index)];
}

Element page_heading(const std::string& title, const std::string& description) {
    return vbox({
        text(title) | bold | color(theme().ui.text_primary),
        paragraph(description) | color(theme().ui.text_secondary),
        separator() | color(theme().ui.text_dim),
    });
}

Element field_row(const std::string& label,
                  const Component& component,
                  const std::string& help = {}) {
    Elements rows = {
        text(label) | bold | color(theme().ui.text_primary),
        component->Render() | border | color(theme().ui.border),
    };
    if (!help.empty()) {
        rows.push_back(paragraph(help) | color(theme().ui.text_secondary));
    }
    return vbox(std::move(rows));
}

Element badge(const std::string& value, Color foreground, Color background) {
    return text(" " + value + " ") | color(foreground) | bgcolor(background);
}

Element status_line(const std::string& value, bool error) {
    if (value.empty()) return text("");
    return hbox({
        text(error ? "Error: " : "Status: ") | bold,
        paragraph(value),
    }) | color(error ? theme().semantic.error : theme().semantic.success);
}

std::string mcp_like_url(std::string base_url) {
    while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
    return base_url + "/models";
}

bool open_external_url(const std::string& url) {
#ifdef _WIN32
    const std::wstring wide_url = utf8_to_wide(url);
    const auto result = reinterpret_cast<std::intptr_t>(
        ::ShellExecuteW(
            nullptr,
            L"open",
            wide_url.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL));
    return result > 32;
#else
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
#  ifdef __APPLE__
        ::execlp("open", "open", url.c_str(), static_cast<char*>(nullptr));
#  else
        ::execlp("xdg-open", "xdg-open", url.c_str(),
                 static_cast<char*>(nullptr));
#  endif
        _exit(127);
    }
    return true;
#endif
}

struct WorkspaceScope {
    std::string project_dir;
    std::string hash;
    std::string name;
    std::string cwd;
};

std::vector<WorkspaceScope> discover_workspace_scopes(
    const std::string& current_cwd,
    const std::string& acecode_dir_override = {}) {
    std::vector<WorkspaceScope> scopes;
    std::unordered_set<std::string> seen;

    auto add = [&](WorkspaceScope scope) {
        if (scope.project_dir.empty() || !seen.insert(scope.project_dir).second) {
            return;
        }
        if (scope.name.empty()) {
            scope.name = desktop::default_workspace_name(scope.cwd);
        }
        scopes.push_back(std::move(scope));
    };

    if (!current_cwd.empty()) {
        const std::string project_dir =
            SessionStorage::get_project_dir(current_cwd);
        add({
            project_dir,
            path_to_utf8(path_from_utf8(project_dir).filename()),
            desktop::default_workspace_name(current_cwd),
            current_cwd,
        });
    }

    const fs::path projects_dir =
        path_from_utf8(
            acecode_dir_override.empty()
                ? get_acecode_dir()
                : acecode_dir_override) /
        "projects";
    std::error_code ec;
    for (fs::directory_iterator it(projects_dir, ec), end;
         !ec && it != end;
         it.increment(ec)) {
        if (ec || !it->is_directory(ec)) continue;
        WorkspaceScope scope;
        scope.project_dir = path_to_utf8(it->path());
        scope.hash = path_to_utf8(it->path().filename());
        const fs::path meta_path = it->path() / "workspace.json";
        std::ifstream input(meta_path);
        if (input) {
            try {
                nlohmann::json json;
                input >> json;
                scope.cwd = json.value("cwd", std::string{});
                scope.name = json.value("name", std::string{});
            } catch (...) {
            }
        }
        if (scope.name.empty()) {
            scope.name = scope.cwd.empty()
                ? scope.hash
                : desktop::default_workspace_name(scope.cwd);
        }
        add(std::move(scope));
    }
    std::sort(
        scopes.begin(), scopes.end(),
        [](const WorkspaceScope& lhs, const WorkspaceScope& rhs) {
            return lower_ascii(lhs.name) < lower_ascii(rhs.name);
        });
    return scopes;
}

struct ArchivedRow {
    std::string project_dir;
    std::string workspace_name;
    SessionMeta meta;
};

std::string archived_row_key(const ArchivedRow& row) {
    return row.project_dir + "\n" + row.meta.id;
}

} // namespace

struct SettingsCenter::Impl {
    explicit Impl(SettingsCenterDependencies dependencies)
        : deps(std::move(dependencies)),
          tabs(settings_tab_labels().begin(), settings_tab_labels().end()) {
        build_components();
        sync_from_config();
    }

    ~Impl() {
        shutting_down.store(true);
        ++async_generation;
        for (auto& thread : async_threads) {
            if (thread.joinable()) thread.join();
        }
    }

    SettingsCenterDependencies deps;
    SettingsNavigationModel navigation;
    std::vector<std::string> tabs;
    int tab_index = 0;
    int previous_tab_index = 0;
    std::string page_status;
    bool page_status_error = false;

    int permission_index = 0;
    std::vector<std::string> permission_entries = {
        "Default - ask before sensitive tools",
        "Accept edits - approve file changes",
        "Plan - planning and read-only exploration",
        "Yolo - run without confirmations",
    };
    bool notifications_enabled = true;

    int theme_index = 0;
    std::vector<std::string> theme_entries = {
        "Auto", "Dark", "Light",
    };

    std::string upgrade_url;
    std::string custom_instructions;

    std::string model_filter;
    std::vector<std::string> model_entries;
    std::vector<std::size_t> visible_model_indexes;
    int model_selected = 0;
    bool model_form_open = false;
    bool model_form_edit = false;
    bool model_form_dirty = false;
    std::string model_original_name;
    std::string form_name;
    int form_provider_index = 0;
    std::vector<std::string> provider_entries = {
        "openai", "anthropic", "copilot",
    };
    std::string form_model;
    std::string form_base_url;
    std::string form_api_key;
    bool form_reveal_api_key = false;
    bool form_mask_api_key = true;
    std::string form_context_window;
    std::string form_stream_timeout;
    std::string form_capabilities;
    std::string form_headers;
    std::string form_status;
    bool form_status_error = false;
    std::string probe_status;
    std::vector<std::string> probed_models;
    std::string auth_status;
    std::string auth_code;
    std::string auth_url;

    UsageAggregate usage;
    std::string usage_error;
    bool usage_loading = false;

    std::string archived_filter;
    std::vector<ArchivedRow> archived_rows;
    std::vector<std::size_t> visible_archived_indexes;
    std::vector<std::string> archived_entries;
    std::set<std::string> archived_selected_ids;
    int archived_selected = 0;
    bool archived_loading = false;

    bool discard_modal_open = false;
    bool model_discard_modal_open = false;
    bool confirm_modal_open = false;
    std::string confirm_title;
    std::string confirm_message;
    std::function<void()> confirm_action;

    std::atomic<bool> shutting_down{false};
    std::atomic<std::uint64_t> async_generation{0};
    std::mutex async_mutex;
    std::vector<std::thread> async_threads;

    Component tab_menu;
    Component tab_content;
    Component root;

    Component permission_radio;
    Component notification_checkbox;
    Component general_page;
    Component theme_radio;
    Component appearance_page;
    Component upgrade_input;
    Component upgrade_save_button;
    Component configuration_page;
    Component instructions_input;
    Component instructions_save_button;
    Component personalization_page;
    Component model_filter_input;
    Component model_menu;
    Component model_add_button;
    Component model_edit_button;
    Component model_default_button;
    Component model_delete_button;
    Component models_page;
    Component usage_refresh_button;
    Component usage_page;
    Component archived_filter_input;
    Component archived_menu;
    Component archived_restore_button;
    Component archived_purge_button;
    Component archived_refresh_button;
    Component archived_page;
    Component about_copy_button;
    Component about_open_button;
    Component about_page;

    Component form_name_input;
    Component form_provider_radio;
    Component form_model_input;
    Component form_base_url_input;
    Component form_api_key_input;
    Component form_reveal_checkbox;
    Component form_context_input;
    Component form_timeout_input;
    Component form_capabilities_input;
    Component form_headers_input;
    Component form_probe_button;
    Component form_auth_button;
    Component form_save_button;
    Component form_save_add_button;
    Component form_cancel_button;
    Component model_form_component;
    Component discard_modal_component;
    Component model_discard_modal_component;
    Component confirm_modal_component;

    SettingsMutationOptions mutation_options() {
        SettingsMutationOptions options;
        options.live_config = deps.config;
        options.restart_required_without_live_apply = false;
        return options;
    }

    void post_event() {
        if (deps.post_event) deps.post_event();
    }

    void post_to_ui(std::function<void()> task) {
        if (deps.post_to_ui) {
            deps.post_to_ui(std::move(task));
        } else {
            task();
        }
    }

    void set_status(std::string value, bool error = false) {
        page_status = std::move(value);
        page_status_error = error;
        post_event();
    }

    void set_form_status(std::string value, bool error = false) {
        form_status = std::move(value);
        form_status_error = error;
        post_event();
    }

    void close_surface() {
        ++async_generation;
        usage_loading = false;
        archived_loading = false;
        if (deps.request_close) deps.request_close();
    }

    void sync_from_config() {
        if (!deps.config) return;
        const std::string permission = deps.config->default_permission_mode;
        permission_index =
            permission == "accept-edits" ? 1 :
            permission == "plan" ? 2 :
            permission == "yolo" ? 3 : 0;
        notifications_enabled = deps.config->desktop.notifications.enabled;
        theme_index =
            deps.config->tui.theme == "dark" ? 1 :
            deps.config->tui.theme == "light" ? 2 : 0;
        upgrade_url = deps.config->upgrade.base_url;
        custom_instructions =
            deps.config->custom_instructions.text_snapshot();
        rebuild_model_entries();
    }

    void persist_permission() {
        static const std::array<const char*, 4> values = {
            "default", "accept-edits", "plan", "yolo",
        };
        const auto result = set_default_permission_mode(
            values[static_cast<std::size_t>(
                std::clamp(permission_index, 0, 3))],
            mutation_options());
        if (!result.ok) {
            sync_from_config();
            set_status(result.error, true);
            return;
        }
        set_status(
            result.changed
                ? "Default permission saved for new sessions."
                : "Default permission is already selected.");
    }

    void persist_notifications() {
        const auto result = set_native_notifications_enabled(
            notifications_enabled, mutation_options());
        if (!result.ok) {
            sync_from_config();
            set_status(result.error, true);
            return;
        }
        set_status(
            result.changed
                ? "Native notification preference saved."
                : "Native notification preference is unchanged.");
    }

    void persist_theme() {
        static const std::array<const char*, 3> values = {
            "auto", "dark", "light",
        };
        const std::string value =
            values[static_cast<std::size_t>(std::clamp(theme_index, 0, 2))];
        SettingsMutationOptions options = mutation_options();
        options.apply_live = [value](const AppConfig&, std::string&) {
            if (value != "auto") swap_theme_palette(value);
            return true;
        };
        const auto result = set_tui_theme(value, options);
        if (!result.ok) {
            sync_from_config();
            set_status(result.error, true);
            return;
        }
        if (value == "auto") {
            set_status(
                "Auto saved. Terminal background detection runs at next launch; "
                "the current palette stays active.");
        } else {
            set_status("Theme switched to " + value + " and saved.");
        }
    }

    bool save_current_editor() {
        const SettingsTab tab = navigation.active_tab();
        SettingsMutationResult result;
        if (tab == SettingsTab::Configuration) {
            result = set_upgrade_base_url(upgrade_url, mutation_options());
        } else if (tab == SettingsTab::Personalization) {
            result = set_custom_instructions(
                custom_instructions, mutation_options());
        } else {
            return false;
        }
        if (!result.ok) {
            set_status(result.error, true);
            return false;
        }
        if (tab == SettingsTab::Configuration && deps.config) {
            upgrade_url = deps.config->upgrade.base_url;
        }
        navigation.set_dirty(false);
        set_status(result.changed ? "Saved." : "No changes to save.");
        return true;
    }

    const ModelProfile* selected_model() const {
        if (!deps.config || visible_model_indexes.empty()) return nullptr;
        const int selected = std::clamp(
            model_selected, 0,
            static_cast<int>(visible_model_indexes.size()) - 1);
        const std::size_t index =
            visible_model_indexes[static_cast<std::size_t>(selected)];
        if (index >= deps.config->saved_models.size()) return nullptr;
        return &deps.config->saved_models[index];
    }

    void rebuild_model_entries() {
        model_entries.clear();
        visible_model_indexes.clear();
        if (!deps.config) return;
        for (std::size_t i = 0; i < deps.config->saved_models.size(); ++i) {
            const auto& model = deps.config->saved_models[i];
            if (!search_matches(
                    model_filter,
                    {model.name, model.provider, model.model,
                     model.base_url})) {
                continue;
            }
            const bool is_default =
                deps.config->default_model_name == model.name;
            model_entries.push_back(
                std::string(is_default ? "* " : "  ") +
                model.name + "  " + model.provider + "/" + model.model);
            visible_model_indexes.push_back(i);
        }
        if (model_entries.empty()) {
            model_entries.push_back("  No matching model profiles");
        }
        const int max_selected = visible_model_indexes.empty()
            ? 0
            : static_cast<int>(visible_model_indexes.size()) - 1;
        model_selected = std::clamp(model_selected, 0, max_selected);
        navigation.page(SettingsTab::Models).filter = model_filter;
        navigation.page(SettingsTab::Models).selected = model_selected;
    }

    void clear_model_form() {
        model_form_edit = false;
        model_form_dirty = false;
        model_original_name.clear();
        form_name.clear();
        form_provider_index = 0;
        form_model.clear();
        form_base_url.clear();
        form_api_key.clear();
        form_context_window.clear();
        form_stream_timeout.clear();
        form_capabilities.clear();
        form_headers = "{}";
        form_reveal_api_key = false;
        form_mask_api_key = true;
        form_status.clear();
        probe_status.clear();
        probed_models.clear();
        auth_status.clear();
        auth_code.clear();
        auth_url.clear();
    }

    void open_model_add() {
        clear_model_form();
        model_form_open = true;
    }

    void open_model_edit() {
        const ModelProfile* profile = selected_model();
        if (!profile) {
            set_status("Select a model profile first.", true);
            return;
        }
        clear_model_form();
        model_form_edit = true;
        model_original_name = profile->name;
        form_name = profile->name;
        form_provider_index =
            profile->provider == "anthropic" ? 1 :
            profile->provider == "copilot" ? 2 : 0;
        form_model = profile->model;
        form_base_url = profile->base_url;
        form_api_key = profile->api_key;
        if (profile->context_window.has_value()) {
            form_context_window = std::to_string(*profile->context_window);
        }
        if (profile->stream_timeout_ms.has_value()) {
            form_stream_timeout =
                std::to_string(*profile->stream_timeout_ms);
        }
        form_capabilities = join_strings(profile->capabilities, ", ");
        form_headers = headers_to_json(profile->request_headers);
        model_form_dirty = false;
        model_form_open = true;
    }

    void close_model_form_now() {
        ++async_generation;
        model_form_dirty = false;
        model_discard_modal_open = false;
        model_form_open = false;
    }

    void request_close_model_form() {
        if (!model_form_dirty) {
            close_model_form_now();
            return;
        }
        model_discard_modal_open = true;
    }

    std::optional<SavedModelDraft> model_draft_from_form() {
        SavedModelDraft draft;
        draft.name = trim_ascii(form_name);
        draft.provider = provider_entries[static_cast<std::size_t>(
            std::clamp(form_provider_index, 0, 2))];
        draft.model = trim_ascii(form_model);
        draft.base_url = trim_ascii(form_base_url);
        draft.api_key = form_api_key;
        if (!parse_optional_positive_int(
                form_context_window, draft.context_window)) {
            set_form_status(
                "Context window must be a positive integer.", true);
            return std::nullopt;
        }
        if (!parse_optional_positive_int(
                form_stream_timeout, draft.stream_timeout_ms)) {
            set_form_status(
                "Stream timeout must be a positive integer in milliseconds.",
                true);
            return std::nullopt;
        }
        draft.capabilities = split_capabilities(form_capabilities);
        std::string headers_error;
        draft.request_headers =
            parse_headers_json(form_headers, headers_error);
        if (!headers_error.empty()) {
            set_form_status(headers_error, true);
            return std::nullopt;
        }
        return draft;
    }

    bool save_model_form(bool add_another) {
        const auto draft = model_draft_from_form();
        if (!draft.has_value()) return false;
        const auto result = model_form_edit
            ? update_saved_model_setting(
                  model_original_name, *draft, mutation_options())
            : add_saved_model_setting(*draft, mutation_options());
        if (!result.ok) {
            set_form_status(result.error, true);
            return false;
        }
        rebuild_model_entries();
        model_form_dirty = false;
        set_status(
            std::string(model_form_edit ? "Updated model profile: "
                                        : "Added model profile: ") +
            draft->name);
        if (add_another) {
            clear_model_form();
            model_form_open = true;
        } else {
            model_form_open = false;
        }
        return true;
    }

    void set_default_model() {
        const ModelProfile* profile = selected_model();
        if (!profile) {
            set_status("Select a model profile first.", true);
            return;
        }
        const std::string name = profile->name;
        const auto result =
            set_default_model_setting(name, mutation_options());
        if (!result.ok) {
            set_status(result.error, true);
            return;
        }
        rebuild_model_entries();
        set_status(
            result.changed
                ? "Default model saved for new sessions: " + name
                : name + " is already the default model.");
    }

    void request_delete_model() {
        const ModelProfile* profile = selected_model();
        if (!profile) {
            set_status("Select a model profile first.", true);
            return;
        }
        const std::string name = profile->name;
        confirm_title = "Delete model profile";
        confirm_message =
            "Delete '" + name +
            "'? Existing session metadata remains readable, but this action "
            "cannot be undone.";
        confirm_action = [this, name]() {
            const auto result = remove_saved_model_setting(
                name,
                deps.model_profile_used_by_busy_session,
                mutation_options());
            if (!result.ok) {
                set_status(result.error, true);
                return;
            }
            rebuild_model_entries();
            set_status("Deleted model profile: " + name);
        };
        confirm_modal_open = true;
    }

    void start_model_probe() {
        const auto draft = model_draft_from_form();
        if (!draft.has_value()) return;
        const std::uint64_t generation = ++async_generation;
        {
            std::lock_guard<std::mutex> lock(async_mutex);
            probe_status = "Probing provider...";
            probed_models.clear();
        }
        post_event();
        async_threads.emplace_back([this, generation, draft = *draft]() {
            std::vector<std::string> models;
            std::string status;
            bool error = false;
            if (draft.provider == "copilot") {
                const auto result =
                    fetch_copilot_model_ids(load_github_token());
                models = result.models;
                if (!result.error.empty()) {
                    error = true;
                    status = result.message.empty()
                        ? result.error
                        : result.message;
                } else {
                    status = "Connected. Found " +
                        std::to_string(models.size()) + " models.";
                }
            } else if (
                draft.provider == "openai" ||
                draft.provider == "anthropic") {
                if (draft.provider == "anthropic") {
                    error = true;
                    status =
                        "Anthropic model discovery is unavailable; enter the "
                        "model identifier manually.";
                } else {
                    const std::string url = mcp_like_url(draft.base_url);
                    cpr::Header headers{
                        {"Accept", "application/json"},
                    };
                    if (!draft.api_key.empty()) {
                        headers["Authorization"] =
                            "Bearer " + draft.api_key;
                    }
                    for (const auto& [name, value] : draft.request_headers) {
                        headers[name] = value;
                    }
                    const auto proxy = network::proxy_options_for(url);
                    const auto response = cpr::Get(
                        cpr::Url{url},
                        headers,
                        network::build_ssl_options(proxy),
                        proxy.proxies,
                        proxy.auth,
                        cpr::Timeout{10000});
                    if (response.status_code < 200 ||
                        response.status_code >= 300) {
                        error = true;
                        status = response.status_code == 0
                            ? "Connection failed: " + response.error.message
                            : "Provider returned HTTP " +
                                  std::to_string(response.status_code);
                    } else {
                        try {
                            const auto body =
                                nlohmann::json::parse(response.text);
                            const nlohmann::json* list = nullptr;
                            if (body.is_object() &&
                                body.contains("data") &&
                                body["data"].is_array()) {
                                list = &body["data"];
                            } else if (
                                body.is_object() &&
                                body.contains("models") &&
                                body["models"].is_array()) {
                                list = &body["models"];
                            } else if (body.is_array()) {
                                list = &body;
                            }
                            if (list) {
                                std::set<std::string> unique;
                                for (const auto& item : *list) {
                                    if (item.is_string()) {
                                        unique.insert(
                                            item.get<std::string>());
                                    } else if (item.is_object()) {
                                        for (const char* key :
                                             {"id", "model", "name"}) {
                                            if (item.contains(key) &&
                                                item[key].is_string()) {
                                                unique.insert(
                                                    item[key]
                                                        .get<std::string>());
                                                break;
                                            }
                                        }
                                    }
                                }
                                models.assign(
                                    unique.begin(), unique.end());
                            }
                            status = "Connected. Found " +
                                std::to_string(models.size()) + " models.";
                        } catch (const std::exception& e) {
                            error = true;
                            status =
                                std::string("Invalid provider response: ") +
                                e.what();
                        }
                    }
                }
            }
            if (shutting_down.load() ||
                generation != async_generation.load()) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(async_mutex);
                probe_status =
                    std::string(error ? "Error: " : "") + status;
                probed_models = std::move(models);
            }
            post_event();
        });
    }

    void start_copilot_auth() {
        const std::uint64_t generation = ++async_generation;
        {
            std::lock_guard<std::mutex> lock(async_mutex);
            auth_status = "Requesting a GitHub device code...";
            auth_code.clear();
            auth_url.clear();
        }
        post_event();
        async_threads.emplace_back([this, generation]() {
            const DeviceCodeResponse device = request_device_code();
            if (shutting_down.load() ||
                generation != async_generation.load()) {
                return;
            }
            if (device.device_code.empty()) {
                {
                    std::lock_guard<std::mutex> lock(async_mutex);
                    auth_status = "Could not request a GitHub device code.";
                }
                post_event();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(async_mutex);
                auth_code = device.user_code;
                auth_url = device.verification_uri;
                auth_status =
                    "Open the URL, enter the code, and keep this window open.";
            }
            post_event();

            const auto start = std::chrono::steady_clock::now();
            int interval = std::max(1, device.interval);
            while (!shutting_down.load() &&
                   generation == async_generation.load()) {
                for (int second = 0;
                     second < interval &&
                     !shutting_down.load() &&
                     generation == async_generation.load();
                     ++second) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
                if (elapsed >= device.expires_in) {
                    std::lock_guard<std::mutex> lock(async_mutex);
                    auth_status = "Device code expired.";
                    post_event();
                    return;
                }
                const DevicePollResult poll =
                    poll_for_access_token_once(device.device_code);
                if (poll.status == "pending") {
                    continue;
                }
                if (poll.status == "slow_down") {
                    interval += std::max(5, poll.interval_delta_seconds);
                    continue;
                }
                if (poll.status == "authorized") {
                    save_github_token(poll.access_token);
                    const CopilotToken token =
                        exchange_copilot_token(poll.access_token);
                    std::lock_guard<std::mutex> lock(async_mutex);
                    auth_status = token.token.empty()
                        ? "GitHub login succeeded, but Copilot token exchange failed."
                        : "GitHub Copilot authentication completed.";
                    post_event();
                    return;
                }
                std::lock_guard<std::mutex> lock(async_mutex);
                auth_status = poll.message.empty()
                    ? "GitHub authentication failed."
                    : poll.message;
                post_event();
                return;
            }
        });
    }

    void refresh_usage() {
        const std::uint64_t generation = ++async_generation;
        usage_loading = true;
        usage_error.clear();
        set_status("Loading usage from all known workspaces...");
        const std::string cwd = deps.cwd;
        const std::string acecode_dir = deps.acecode_dir_override;
        async_threads.emplace_back(
            [this, generation, cwd, acecode_dir]() {
                UsageAggregate loaded;
                std::string error;
                try {
                    std::vector<UsageLedgerScope> scopes;
                    for (const auto& scope :
                         discover_workspace_scopes(cwd, acecode_dir)) {
                        scopes.push_back({
                            scope.project_dir,
                            scope.hash,
                            scope.name,
                            scope.cwd,
                        });
                    }
                    UsageLedgerQuery query;
                    query.days = 30;
                    loaded = aggregate_usage_ledgers(scopes, query);
                } catch (const std::exception& e) {
                    error = e.what();
                }
                post_to_ui(
                    [this,
                     generation,
                     loaded = std::move(loaded),
                     error = std::move(error)]() mutable {
                        if (shutting_down.load() ||
                            generation != async_generation.load()) {
                            return;
                        }
                        usage_loading = false;
                        usage_error = std::move(error);
                        if (usage_error.empty()) {
                            usage = std::move(loaded);
                            set_status(
                                "Usage refreshed across all known "
                                "workspaces.");
                        } else {
                            set_status(
                                "Usage refresh failed: " + usage_error,
                                true);
                        }
                    });
            });
    }

    void refresh_archived(bool announce = true) {
        const std::uint64_t generation = ++async_generation;
        archived_loading = true;
        archived_rows.clear();
        rebuild_archived_entries();
        if (announce) {
            set_status("Loading archived sessions...");
        }
        const std::string cwd = deps.cwd;
        const std::string acecode_dir = deps.acecode_dir_override;
        async_threads.emplace_back(
            [this, generation, announce, cwd, acecode_dir]() {
                std::vector<ArchivedRow> loaded;
                std::string error;
                try {
                    for (const auto& scope :
                         discover_workspace_scopes(cwd, acecode_dir)) {
                        for (const auto& meta :
                             SessionStorage::list_sessions(
                                 scope.project_dir)) {
                            if (!meta.archived ||
                                !meta.parent_session_id.empty()) {
                                continue;
                            }
                            loaded.push_back({
                                scope.project_dir,
                                scope.name,
                                meta,
                            });
                        }
                    }
                    std::sort(
                        loaded.begin(), loaded.end(),
                        [](const ArchivedRow& lhs,
                           const ArchivedRow& rhs) {
                            return lhs.meta.updated_at >
                                rhs.meta.updated_at;
                        });
                } catch (const std::exception& e) {
                    error = e.what();
                }
                post_to_ui(
                    [this,
                     generation,
                     announce,
                     loaded = std::move(loaded),
                     error = std::move(error)]() mutable {
                        if (shutting_down.load() ||
                            generation != async_generation.load()) {
                            return;
                        }
                        archived_loading = false;
                        if (!error.empty()) {
                            set_status(
                                "Archive refresh failed: " + error,
                                true);
                            return;
                        }
                        archived_rows = std::move(loaded);
                        rebuild_archived_entries();
                        if (announce) {
                            set_status("Archived sessions refreshed.");
                        } else {
                            post_event();
                        }
                    });
            });
    }

    void rebuild_archived_entries() {
        visible_archived_indexes.clear();
        archived_entries.clear();
        for (std::size_t i = 0; i < archived_rows.size(); ++i) {
            const auto& row = archived_rows[i];
            if (!search_matches(
                    archived_filter,
                    {row.meta.id, row.meta.title, row.meta.summary,
                     row.workspace_name, row.meta.cwd})) {
                continue;
            }
            const bool selected =
                archived_selected_ids.count(archived_row_key(row)) != 0;
            std::string title = row.meta.title.empty()
                ? (row.meta.summary.empty() ? row.meta.id : row.meta.summary)
                : row.meta.title;
            archived_entries.push_back(
                std::string(selected ? "[x] " : "[ ] ") +
                truncate_middle(title, 48) + "  " +
                row.workspace_name + "  " + row.meta.updated_at);
            visible_archived_indexes.push_back(i);
        }
        if (archived_entries.empty()) {
            archived_entries.push_back("    No archived sessions");
        }
        const int max_selected = visible_archived_indexes.empty()
            ? 0
            : static_cast<int>(visible_archived_indexes.size()) - 1;
        archived_selected =
            std::clamp(archived_selected, 0, max_selected);
        navigation.page(SettingsTab::Archived).filter = archived_filter;
        navigation.page(SettingsTab::Archived).selected = archived_selected;
    }

    void toggle_archived_selection() {
        if (visible_archived_indexes.empty()) return;
        const std::size_t row_index = visible_archived_indexes[
            static_cast<std::size_t>(archived_selected)];
        const std::string key = archived_row_key(archived_rows[row_index]);
        if (!archived_selected_ids.erase(key)) {
            archived_selected_ids.insert(key);
        }
        rebuild_archived_entries();
    }

    std::vector<std::size_t> selected_archived_rows() const {
        std::vector<std::size_t> result;
        if (!archived_selected_ids.empty()) {
            for (std::size_t i = 0; i < archived_rows.size(); ++i) {
                if (archived_selected_ids.count(
                        archived_row_key(archived_rows[i]))) {
                    result.push_back(i);
                }
            }
            return result;
        }
        if (!visible_archived_indexes.empty()) {
            result.push_back(visible_archived_indexes[
                static_cast<std::size_t>(archived_selected)]);
        }
        return result;
    }

    void restore_archived() {
        const auto selected = selected_archived_rows();
        if (selected.empty()) {
            set_status("Select an archived session first.", true);
            return;
        }
        int restored = 0;
        std::vector<std::string> failures;
        for (const std::size_t index : selected) {
            ArchivedRow& row = archived_rows[index];
            try {
                row.meta.archived = false;
                SessionStorage::write_meta(
                    SessionStorage::meta_path(
                        row.project_dir, row.meta.id),
                    row.meta);
                ++restored;
                archived_selected_ids.erase(archived_row_key(row));
            } catch (const std::exception& e) {
                failures.push_back(row.meta.id + ": " + e.what());
            }
        }
        refresh_archived(false);
        std::string message =
            "Restored " + std::to_string(restored) + " session(s).";
        if (!failures.empty()) {
            message += " Failed: " + join_strings(failures, "; ");
        }
        set_status(message, !failures.empty());
    }

    void request_purge_archived() {
        const auto selected = selected_archived_rows();
        if (selected.empty()) {
            set_status("Select an archived session first.", true);
            return;
        }
        confirm_title = "Permanently purge sessions";
        confirm_message =
            "Permanently delete " + std::to_string(selected.size()) +
            " archived session(s), including transcripts and stored tool "
            "results? This cannot be undone.";
        confirm_action = [this, selected]() {
            int purged = 0;
            std::vector<std::string> failures;
            for (const std::size_t index : selected) {
                if (index >= archived_rows.size()) continue;
                const auto& row = archived_rows[index];
                if (deps.session_is_busy &&
                    deps.session_is_busy(row.meta.id)) {
                    failures.push_back(
                        row.meta.id + ": session is busy");
                    continue;
                }
                std::string error;
                if (SessionStorage::purge_session_files(
                        row.project_dir, row.meta.id, &error)) {
                    ++purged;
                    archived_selected_ids.erase(archived_row_key(row));
                } else {
                    failures.push_back(
                        row.meta.id + ": " +
                        (error.empty() ? "purge failed" : error));
                }
            }
            refresh_archived(false);
            std::string message =
                "Purged " + std::to_string(purged) + " session(s).";
            if (!failures.empty()) {
                message += " Failed: " + join_strings(failures, "; ");
            }
            set_status(message, !failures.empty());
        };
        confirm_modal_open = true;
    }

    Element render_usage_chart() const {
        constexpr int width = 120;
        constexpr int height = 32;
        Canvas chart(width, height);
        chart.DrawBlockLine(
            2, height - 4, width - 2, height - 4,
            theme().ui.text_dim);
        if (!usage.daily.empty()) {
            std::int64_t max_total = 1;
            for (const auto& day : usage.daily) {
                max_total =
                    std::max(max_total, day.totals.total_tokens);
            }
            int previous_x = 2;
            int previous_y = height - 4;
            for (std::size_t i = 0; i < usage.daily.size(); ++i) {
                const int x = 2 + static_cast<int>(
                    (width - 4) * i /
                    std::max<std::size_t>(1, usage.daily.size() - 1));
                const int y = height - 4 - static_cast<int>(
                    (height - 8) *
                    usage.daily[i].totals.total_tokens / max_total);
                if (i) {
                    chart.DrawBlockLine(
                        previous_x, previous_y, x, y,
                        theme().ui.accent);
                }
                chart.DrawBlockCircleFilled(
                    x, y, 1, theme().ui.accent_alt);
                previous_x = x;
                previous_y = y;
            }
        }
        return canvas(std::move(chart)) |
            size(HEIGHT, EQUAL, 8) |
            flex;
    }

    Element render_footer() const {
        Elements actions;
        const auto values = settings_footer_actions(
            navigation.active_tab(), navigation.dirty());
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) {
                actions.push_back(
                    text("  |  ") | color(theme().ui.text_dim));
            }
            std::string key;
            switch (values[i]) {
                case FooterAction::Filter: key = "/"; break;
                case FooterAction::Save: key = "Ctrl+S"; break;
                case FooterAction::Add: key = "a"; break;
                case FooterAction::Edit: key = "e"; break;
                case FooterAction::Delete: key = "d"; break;
                case FooterAction::SetDefault: key = "s"; break;
                case FooterAction::Refresh: key = "r"; break;
                case FooterAction::Restore: key = "Enter"; break;
                case FooterAction::Purge: key = "d"; break;
                case FooterAction::Copy: key = "c"; break;
                case FooterAction::Open: key = "o"; break;
                case FooterAction::Close: key = "Esc"; break;
                default: key = ""; break;
            }
            actions.push_back(hbox({
                text(key) | bold | color(theme().ui.text_primary),
                text(" " + footer_action_label(values[i])) |
                    color(theme().ui.text_secondary),
            }));
        }
        return hbox(std::move(actions)) | hcenter;
    }

    Element render_model_details() const {
        const ModelProfile* profile = selected_model();
        if (!profile) {
            return vbox({
                text("No model selected") | bold,
                paragraph(
                    "Add a saved model profile or change the current filter."),
            }) | color(theme().ui.text_secondary) | border;
        }
        Elements values = {
            hbox({
                text(profile->name) | bold |
                    color(theme().ui.text_primary),
                filler(),
                deps.config &&
                        deps.config->default_model_name == profile->name
                    ? badge(
                          "DEFAULT",
                          theme().ui.badge_fg,
                          theme().ui.badge_bg)
                    : text(""),
            }),
            separator() | color(theme().ui.text_dim),
            text("Provider       " + profile->provider),
            text("Model          " + profile->model),
        };
        if (!profile->base_url.empty()) {
            values.push_back(
                text("Base URL       " +
                     truncate_middle(profile->base_url, 62)));
        }
        values.push_back(text(
            std::string("API key        ") +
            (profile->api_key.empty() ? "(not used)" : "configured")));
        values.push_back(text(
            "Context window  " +
            (profile->context_window.has_value()
                 ? std::to_string(*profile->context_window)
                 : "automatic")));
        values.push_back(text(
            "Capabilities    " +
            (profile->capabilities.empty()
                 ? "(none declared)"
                 : join_strings(profile->capabilities, ", "))));
        values.push_back(text(
            "Headers         " +
            std::to_string(profile->request_headers.size()) +
            " configured"));
        return vbox(std::move(values)) |
            color(theme().ui.text_muted) | border;
    }

    Element render_archive_details() const {
        if (visible_archived_indexes.empty()) {
            return vbox({
                text("Archive is empty") | bold,
                paragraph(
                    "Archived sessions from all known workspaces appear here."),
            }) | color(theme().ui.text_secondary) | border;
        }
        const auto& row = archived_rows[visible_archived_indexes[
            static_cast<std::size_t>(archived_selected)]];
        return vbox({
            text(
                row.meta.title.empty() ? row.meta.id : row.meta.title) |
                bold | color(theme().ui.text_primary),
            separator() | color(theme().ui.text_dim),
            text("Workspace  " + row.workspace_name),
            text("Session    " + row.meta.id),
            text("Updated    " + row.meta.updated_at),
            text("Messages   " + std::to_string(row.meta.message_count)),
            paragraph(
                row.meta.summary.empty()
                    ? "(No summary)"
                    : row.meta.summary),
            text(
                archived_selected_ids.count(archived_row_key(row))
                    ? "Selected for batch action"
                    : "Press Space to add to batch selection") |
                color(theme().ui.accent_alt),
        }) | color(theme().ui.text_muted) | border;
    }

    void build_components() {
        auto tab_option = MenuOption::HorizontalAnimated();
        tab_option.underline.color_active = theme().ui.accent;
        tab_option.underline.color_inactive = theme().ui.text_dim;
        tab_option.entries_option.transform = [](const EntryState& state) {
            Element value = text(" " + state.label + " ");
            if (state.active) {
                value = value | bold | color(theme().ui.text_primary);
            } else {
                value = value | color(theme().ui.text_secondary);
            }
            return value;
        };
        tab_option.on_change = [this]() {
            const SettingsTab requested =
                static_cast<SettingsTab>(tab_index);
            const NavigationResult result =
                navigation.request_tab(requested);
            if (result == NavigationResult::NeedsDiscardConfirmation) {
                tab_index = previous_tab_index;
                discard_modal_open = true;
                return;
            }
            ++async_generation;
            usage_loading = false;
            archived_loading = false;
            previous_tab_index = tab_index;
            page_status.clear();
            if (requested == SettingsTab::Usage) refresh_usage();
            if (requested == SettingsTab::Archived) refresh_archived();
        };
        tab_menu = Menu(&tabs, &tab_index, tab_option);

        RadioboxOption permission_option;
        permission_option.entries = &permission_entries;
        permission_option.selected = &permission_index;
        permission_option.on_change = [this]() { persist_permission(); };
        permission_option.transform = [](const EntryState& state) {
            const std::string prefix = state.state ? "(*) " : "( ) ";
            Element line = text(prefix + state.label);
            if (state.focused) {
                line = line | bgcolor(theme().ui.selection_bg) |
                    color(theme().ui.selection_fg);
            } else {
                line = line | color(theme().ui.text_muted);
            }
            return line;
        };
        permission_radio = Radiobox(permission_option);

        CheckboxOption notification_option;
        notification_option.label =
            "Enable native notifications for permission, question, and completion events";
        notification_option.checked = &notifications_enabled;
        notification_option.on_change = [this]() {
            persist_notifications();
        };
        notification_checkbox = Checkbox(notification_option);

        auto general_container = Container::Vertical({
            permission_radio,
            notification_checkbox,
        });
        general_page = Renderer(general_container, [this]() {
            return vbox({
                page_heading(
                    "General",
                    "Defaults used when a new ACECode session is created."),
                text("Default permission mode") | bold,
                paragraph(permission_description(permission_index)) |
                    color(theme().ui.text_secondary),
                permission_radio->Render() | border |
                    color(theme().ui.border),
                separatorEmpty(),
                text("Native notifications") | bold,
                notification_checkbox->Render() | border |
                    color(theme().ui.border),
                paragraph(
                    "This is a single master switch. Existing notification "
                    "event preferences remain intact.") |
                    color(theme().ui.text_secondary),
            }) | yframe | vscroll_indicator | flex;
        });

        RadioboxOption theme_option;
        theme_option.entries = &theme_entries;
        theme_option.selected = &theme_index;
        theme_option.on_change = [this]() { persist_theme(); };
        theme_option.transform = [](const EntryState& state) {
            Element line = text(
                std::string(state.state ? "(*) " : "( ) ") + state.label);
            if (state.focused) {
                return line | bgcolor(theme().ui.selection_bg) |
                    color(theme().ui.selection_fg);
            }
            return line | color(theme().ui.text_muted);
        };
        theme_radio = Radiobox(theme_option);
        appearance_page = Renderer(theme_radio, [this]() {
            auto preview = [](const std::string& title,
                              Color bg,
                              Color fg,
                              Color accent) {
                return vbox({
                    text(" " + title + " ") | bold,
                    separator(),
                    text(" ACECode settings "),
                    hbox({
                        text(" active ") | color(Color::Black) |
                            bgcolor(accent),
                        text("  secondary "),
                    }),
                    text(" model/profile-name "),
                }) | color(fg) | bgcolor(bg) |
                    size(WIDTH, EQUAL, 28) |
                    size(HEIGHT, EQUAL, 7) | border;
            };
            return vbox({
                page_heading(
                    "Appearance",
                    "Choose the TUI color theme. Desktop and Web appearance "
                    "settings are intentionally not mirrored here."),
                hbox({
                    vbox({
                        text("TUI theme") | bold,
                        theme_radio->Render() | border |
                            color(theme().ui.border),
                        paragraph(
                            "Auto detects the terminal background at launch. "
                            "Dark and Light apply immediately.") |
                            color(theme().ui.text_secondary),
                    }) | size(WIDTH, EQUAL, 36),
                    separator(),
                    vbox({
                        text("Preview") | bold,
                        hbox({
                            preview(
                                "Dark",
                                Color::RGB(15, 17, 20),
                                Color::GrayLight,
                                Color::CyanLight),
                            preview(
                                "Light",
                                Color::RGB(245, 245, 245),
                                Color::Black,
                                Color::RGB(0, 120, 160)),
                        }),
                    }) | flex,
                }),
            }) | yframe | vscroll_indicator | flex;
        });

        InputOption upgrade_option = InputOption::Spacious();
        upgrade_option.content = &upgrade_url;
        upgrade_option.multiline = false;
        upgrade_option.on_change = [this]() {
            navigation.set_dirty(
                deps.config &&
                upgrade_url != deps.config->upgrade.base_url);
        };
        upgrade_input = Input(upgrade_option);
        upgrade_save_button = Button(
            " Save ",
            [this]() { save_current_editor(); },
            ButtonOption::Animated());
        auto configuration_container = Container::Vertical({
            upgrade_input,
            upgrade_save_button,
        });
        configuration_page = Renderer(
            configuration_container, [this]() {
                return vbox({
                    page_heading(
                        "Configuration",
                        "Service endpoints shared by the TUI, Desktop, and "
                        "daemon."),
                    field_row(
                        "Upgrade service URL",
                        upgrade_input,
                        "Must be an HTTP or HTTPS base URL. The trailing slash "
                        "is normalized when saved."),
                    hbox({
                        upgrade_save_button->Render(),
                        text(
                            navigation.dirty()
                                ? "  Unsaved changes"
                                : "  Saved") |
                            color(
                                navigation.dirty()
                                    ? theme().semantic.warning
                                    : theme().semantic.success),
                    }),
                }) | yframe | vscroll_indicator | flex;
            });

        InputOption instructions_option = InputOption::Spacious();
        instructions_option.content = &custom_instructions;
        instructions_option.placeholder =
            "Describe preferences ACECode should follow in every session...";
        instructions_option.multiline = true;
        instructions_option.on_change = [this]() {
            navigation.set_dirty(
                deps.config &&
                custom_instructions !=
                    deps.config->custom_instructions.text_snapshot());
        };
        instructions_input = Input(instructions_option);
        instructions_save_button = Button(
            " Save ",
            [this]() { save_current_editor(); },
            ButtonOption::Animated());
        auto personalization_container = Container::Vertical({
            instructions_input,
            instructions_save_button,
        });
        personalization_page = Renderer(
            personalization_container, [this]() {
                const std::size_t bytes = custom_instructions.size();
                return vbox({
                    page_heading(
                        "Personalization",
                        "Custom instructions are appended to new model "
                        "contexts on every application surface."),
                    text("Custom instructions") | bold,
                    instructions_input->Render() |
                        size(HEIGHT, EQUAL, 13) | border |
                        color(theme().ui.border),
                    hbox({
                        text(
                            std::to_string(bytes) + " / " +
                            std::to_string(kCustomInstructionsMaxBytes) +
                            " bytes") |
                            color(
                                bytes > kCustomInstructionsMaxBytes
                                    ? theme().semantic.error
                                    : theme().ui.text_secondary),
                        filler(),
                        instructions_save_button->Render(),
                    }),
                }) | yframe | vscroll_indicator | flex;
            });

        InputOption model_filter_option = InputOption::Spacious();
        model_filter_option.content = &model_filter;
        model_filter_option.placeholder = "/ to search models";
        model_filter_option.multiline = false;
        model_filter_option.on_change =
            [this]() { rebuild_model_entries(); };
        model_filter_input = Input(model_filter_option);
        auto model_menu_option = MenuOption::VerticalAnimated();
        model_menu_option.entries_option.transform =
            [](const EntryState& state) {
                Element line = text(state.label);
                if (state.focused) {
                    return line | bgcolor(theme().ui.selection_bg) |
                        color(theme().ui.selection_fg);
                }
                return line | color(theme().ui.text_muted);
            };
        model_menu = Menu(
            &model_entries, &model_selected, model_menu_option);
        model_add_button = Button(
            " Add ", [this]() { open_model_add(); },
            ButtonOption::Animated());
        model_edit_button = Button(
            " Edit ", [this]() { open_model_edit(); },
            ButtonOption::Animated());
        model_default_button = Button(
            " Set default ", [this]() { set_default_model(); },
            ButtonOption::Animated());
        model_delete_button = Button(
            " Delete ", [this]() { request_delete_model(); },
            ButtonOption::Animated());
        auto models_container = Container::Vertical({
            model_filter_input,
            Container::Horizontal({
                model_menu,
                Container::Vertical({
                    model_add_button,
                    model_edit_button,
                    model_default_button,
                    model_delete_button,
                }),
            }),
        });
        models_page = Renderer(models_container, [this]() {
            return vbox({
                page_heading(
                    "Models",
                    "Manage saved profiles and the global default for future "
                    "sessions. Use /model to switch only the current session."),
                hbox({
                    text("Search ") | bold,
                    model_filter_input->Render() | flex | border |
                        color(theme().ui.border),
                    text("  " + std::to_string(
                        visible_model_indexes.size()) + " profiles") |
                        color(theme().ui.text_secondary),
                }),
                hbox({
                    model_menu->Render() | frame |
                        vscroll_indicator |
                        size(WIDTH, EQUAL, 56),
                    separator(),
                    vbox({
                        render_model_details() | flex,
                        hbox({
                            model_add_button->Render(),
                            model_edit_button->Render(),
                            model_default_button->Render(),
                            model_delete_button->Render(),
                        }),
                    }) | flex,
                }) | flex,
            });
        });

        usage_refresh_button = Button(
            " Refresh ",
            [this]() { refresh_usage(); },
            ButtonOption::Animated());
        usage_page = Renderer(usage_refresh_button, [this]() {
            if (usage_loading) {
                return vbox({
                    page_heading(
                        "Usage",
                        "Thirty-day token usage aggregated from every known "
                        "workspace ledger."),
                    hbox({
                        text("Loading usage ledgers...") |
                            color(theme().ui.accent_alt),
                        filler(),
                        usage_refresh_button->Render(),
                    }),
                }) | yframe | vscroll_indicator | flex;
            }
            Elements model_rows;
            for (std::size_t i = 0;
                 i < std::min<std::size_t>(5, usage.models.size());
                 ++i) {
                const auto& row = usage.models[i];
                model_rows.push_back(hbox({
                    text(truncate_middle(row.label, 36)) | flex,
                    text(format_tokens(row.totals.total_tokens)) |
                        color(theme().ui.accent_alt),
                }));
            }
            if (model_rows.empty()) {
                model_rows.push_back(
                    text("No model usage in this period.") |
                    color(theme().ui.text_secondary));
            }
            Elements workspace_rows;
            for (std::size_t i = 0;
                 i < std::min<std::size_t>(5, usage.workspaces.size());
                 ++i) {
                const auto& row = usage.workspaces[i];
                workspace_rows.push_back(hbox({
                    text(truncate_middle(row.workspace_name, 36)) | flex,
                    text(format_tokens(row.totals.total_tokens)) |
                        color(theme().ui.accent_alt),
                }));
            }
            if (workspace_rows.empty()) {
                workspace_rows.push_back(
                    text("No workspace usage in this period.") |
                    color(theme().ui.text_secondary));
            }
            return vbox({
                page_heading(
                    "Usage",
                    "Thirty-day token usage aggregated from every known "
                    "workspace ledger."),
                hbox({
                    vbox({
                        text("Total tokens") |
                            color(theme().ui.text_secondary),
                        text(format_tokens(usage.totals.total_tokens)) |
                            bold | color(theme().ui.accent),
                    }) | border | flex,
                    vbox({
                        text("Sessions") |
                            color(theme().ui.text_secondary),
                        text(std::to_string(usage.session_ids.size())) |
                            bold,
                    }) | border | flex,
                    vbox({
                        text("Prompt / completion") |
                            color(theme().ui.text_secondary),
                        text(
                            format_tokens(usage.totals.prompt_tokens) +
                            " / " +
                            format_tokens(usage.totals.completion_tokens)) |
                            bold,
                    }) | border | flex,
                    usage_refresh_button->Render(),
                }),
                text("Daily trend") | bold,
                render_usage_chart() | border |
                    color(theme().ui.border),
                hbox({
                    vbox({
                        text("By model") | bold,
                        vbox(std::move(model_rows)),
                    }) | border | flex,
                    vbox({
                        text("By workspace") | bold,
                        vbox(std::move(workspace_rows)),
                    }) | border | flex,
                    vbox({
                        text("Token categories") | bold,
                        text(
                            "Cache read   " +
                            format_tokens(
                                usage.totals.cache_read_tokens)),
                        text(
                            "Cache write  " +
                            format_tokens(
                                usage.totals.cache_write_tokens)),
                        text(
                            "Reasoning    " +
                            format_tokens(
                                usage.totals.reasoning_tokens)),
                    }) | border | flex,
                }),
                usage_error.empty()
                    ? text("")
                    : paragraph(usage_error) |
                          color(theme().semantic.error),
            }) | yframe | vscroll_indicator | flex;
        });

        InputOption archived_filter_option = InputOption::Spacious();
        archived_filter_option.content = &archived_filter;
        archived_filter_option.placeholder =
            "/ to search archived sessions";
        archived_filter_option.multiline = false;
        archived_filter_option.on_change =
            [this]() { rebuild_archived_entries(); };
        archived_filter_input = Input(archived_filter_option);
        auto archived_menu_option = MenuOption::VerticalAnimated();
        archived_menu_option.entries_option.transform =
            [](const EntryState& state) {
                Element line = text(state.label);
                if (state.focused) {
                    return line | bgcolor(theme().ui.selection_bg) |
                        color(theme().ui.selection_fg);
                }
                return line | color(theme().ui.text_muted);
            };
        archived_menu = Menu(
            &archived_entries,
            &archived_selected,
            archived_menu_option);
        archived_restore_button = Button(
            " Restore ",
            [this]() { restore_archived(); },
            ButtonOption::Animated());
        archived_purge_button = Button(
            " Purge ",
            [this]() { request_purge_archived(); },
            ButtonOption::Animated());
        archived_refresh_button = Button(
            " Refresh ",
            [this]() { refresh_archived(); },
            ButtonOption::Animated());
        auto archived_container = Container::Vertical({
            archived_filter_input,
            archived_menu,
            Container::Horizontal({
                archived_restore_button,
                archived_purge_button,
                archived_refresh_button,
            }),
        });
        archived_page = Renderer(archived_container, [this]() {
            return vbox({
                page_heading(
                    "Archived",
                    "Restore or permanently purge sessions from every known "
                    "workspace. Space toggles batch selection."),
                hbox({
                    text("Search ") | bold,
                    archived_filter_input->Render() | flex | border |
                        color(theme().ui.border),
                    text(
                        "  " +
                        std::to_string(archived_selected_ids.size()) +
                        " selected") |
                        color(theme().ui.text_secondary),
                }),
                archived_loading
                    ? paragraph("Loading archived sessions...") |
                          color(theme().ui.accent_alt)
                    : text(""),
                hbox({
                    archived_menu->Render() | frame |
                        vscroll_indicator |
                        size(WIDTH, EQUAL, 70),
                    separator(),
                    render_archive_details() | flex,
                }) | flex,
                hbox({
                    archived_restore_button->Render(),
                    archived_purge_button->Render(),
                    archived_refresh_button->Render(),
                }),
            });
        });

        about_copy_button = Button(
            " Copy config path ",
            [this]() {
                const std::string path = path_to_utf8(
                    path_from_utf8(
                        deps.acecode_dir_override.empty()
                            ? get_acecode_dir()
                            : deps.acecode_dir_override) /
                    "config.json");
                const auto result = write_system_clipboard_text(path);
                set_status(
                    result ? "Config path copied to the clipboard."
                           : "Clipboard is unavailable.",
                    !result);
            },
            ButtonOption::Animated());
        about_open_button = Button(
            " Open project ",
            [this]() {
                const bool opened = open_external_url(kProjectUrl);
                set_status(
                    opened ? "Project page opened in the default browser."
                           : "Could not open the default browser.",
                    !opened);
            },
            ButtonOption::Animated());
        auto about_container = Container::Horizontal({
            about_copy_button,
            about_open_button,
        });
        about_page = Renderer(about_container, [this]() {
            const std::string config_path = path_to_utf8(
                path_from_utf8(
                    deps.acecode_dir_override.empty()
                        ? get_acecode_dir()
                        : deps.acecode_dir_override) /
                "config.json");
            return vbox({
                page_heading(
                    "About",
                    "Build and installation information for this ACECode "
                    "terminal session."),
                vbox({
                    hbox({
                        text("ACECode") | bold | size(WIDTH, EQUAL, 20),
                        text(deps.acecode_version),
                    }),
                    separator() | color(theme().ui.text_dim),
                    hbox({
                        text("FTXUI") | bold | size(WIDTH, EQUAL, 20),
                        text(kFtxuiVersion),
                    }),
                    separator() | color(theme().ui.text_dim),
                    hbox({
                        text("Config path") | bold |
                            size(WIDTH, EQUAL, 20),
                        text(config_path),
                    }),
                    separator() | color(theme().ui.text_dim),
                    hbox({
                        text("Project") | bold | size(WIDTH, EQUAL, 20),
                        text(kProjectUrl) |
                            color(theme().markdown.link),
                    }),
                }) | border | color(theme().ui.border),
                hbox({
                    about_copy_button->Render(),
                    about_open_button->Render(),
                }),
            }) | yframe | vscroll_indicator | flex;
        });

        tab_content = Container::Tab(
            {
                general_page,
                appearance_page,
                configuration_page,
                personalization_page,
                models_page,
                usage_page,
                archived_page,
                about_page,
            },
            &tab_index);

        build_model_form();
        build_confirmation_modals();

        auto content_container = Container::Vertical({
            tab_menu,
            tab_content,
        });
        root = Renderer(content_container, [this]() {
            return vbox({
                hbox({
                    text(" ACECode Settings ") | bold |
                        color(theme().ui.text_primary),
                    filler(),
                    text("[Esc] Close ") |
                        color(theme().ui.text_secondary),
                }),
                separator() | color(theme().ui.border),
                tab_menu->Render() | xframe,
                separator() | color(theme().ui.text_dim),
                tab_content->Render() | flex,
                status_line(page_status, page_status_error),
                separator() | color(theme().ui.text_dim),
                render_footer(),
            }) | border | color(theme().ui.border);
        });

        root = CatchEvent(root, [this](Event event) {
            if (event == Event::CtrlS) {
                if (navigation.active_tab() ==
                        SettingsTab::Configuration ||
                    navigation.active_tab() ==
                        SettingsTab::Personalization) {
                    save_current_editor();
                    return true;
                }
            }
            if (event == Event::Escape) {
                if (model_form_open) {
                    request_close_model_form();
                    return true;
                }
                const NavigationResult result =
                    navigation.request_close();
                if (result ==
                    NavigationResult::NeedsDiscardConfirmation) {
                    discard_modal_open = true;
                } else {
                    close_surface();
                }
                return true;
            }
            if (navigation.active_tab() == SettingsTab::Models) {
                if (model_filter_input->Focused()) return false;
                const std::string character =
                    lower_ascii(event.character());
                if (character == "a") {
                    open_model_add();
                    return true;
                }
                if (character == "e") {
                    open_model_edit();
                    return true;
                }
                if (character == "s") {
                    set_default_model();
                    return true;
                }
                if (character == "d") {
                    request_delete_model();
                    return true;
                }
                if (character == "/") {
                    model_filter_input->TakeFocus();
                    return true;
                }
            }
            if (navigation.active_tab() == SettingsTab::Archived) {
                if (archived_filter_input->Focused()) return false;
                if (event == Event::Character(' ')) {
                    toggle_archived_selection();
                    return true;
                }
                const std::string character =
                    lower_ascii(event.character());
                if (character == "/") {
                    archived_filter_input->TakeFocus();
                    return true;
                }
                if (character == "r") {
                    refresh_archived();
                    return true;
                }
                if (character == "d") {
                    request_purge_archived();
                    return true;
                }
                if (event == Event::Return) {
                    restore_archived();
                    return true;
                }
            }
            if (navigation.active_tab() == SettingsTab::Usage &&
                lower_ascii(event.character()) == "r") {
                refresh_usage();
                return true;
            }
            if (navigation.active_tab() == SettingsTab::About) {
                const std::string character =
                    lower_ascii(event.character());
                if (character == "c") {
                    about_copy_button->OnEvent(Event::Return);
                    return true;
                }
                if (character == "o") {
                    about_open_button->OnEvent(Event::Return);
                    return true;
                }
            }
            return false;
        });
        root |= Modal(model_form_component, &model_form_open);
        root |= Modal(
            model_discard_modal_component,
            &model_discard_modal_open);
        root |= Modal(discard_modal_component, &discard_modal_open);
        root |= Modal(confirm_modal_component, &confirm_modal_open);
    }

    void build_model_form() {
        auto single_line_input = [this](
                                     std::string* value,
                                     const std::string& placeholder) {
            InputOption option = InputOption::Spacious();
            option.content = value;
            option.placeholder = placeholder;
            option.multiline = false;
            option.on_change = [this]() {
                if (model_form_open) model_form_dirty = true;
            };
            return Input(option);
        };
        form_name_input =
            single_line_input(&form_name, "Profile name");
        form_model_input =
            single_line_input(&form_model, "Model identifier");
        form_base_url_input =
            single_line_input(&form_base_url, "https://.../v1");
        InputOption key_option = InputOption::Spacious();
        key_option.content = &form_api_key;
        key_option.placeholder = "API key";
        key_option.multiline = false;
        key_option.password = &form_mask_api_key;
        key_option.on_change = [this]() {
            if (model_form_open) model_form_dirty = true;
        };
        form_api_key_input = Input(key_option);
        CheckboxOption reveal_option;
        reveal_option.label = "Reveal API key";
        reveal_option.checked = &form_reveal_api_key;
        reveal_option.on_change = [this]() {
            form_mask_api_key = !form_reveal_api_key;
        };
        form_reveal_checkbox = Checkbox(reveal_option);
        form_context_input =
            single_line_input(&form_context_window, "Automatic");
        form_timeout_input =
            single_line_input(&form_stream_timeout, "Provider default");
        form_capabilities_input =
            single_line_input(
                &form_capabilities,
                "vision, tool_use, web_search");
        InputOption headers_option = InputOption::Spacious();
        headers_option.content = &form_headers;
        headers_option.placeholder = "{}";
        headers_option.multiline = true;
        headers_option.on_change = [this]() {
            if (model_form_open) model_form_dirty = true;
        };
        form_headers_input = Input(headers_option);

        RadioboxOption provider_option;
        provider_option.entries = &provider_entries;
        provider_option.selected = &form_provider_index;
        provider_option.on_change = [this]() {
            if (model_form_open) model_form_dirty = true;
        };
        form_provider_radio = Radiobox(provider_option);

        form_probe_button = Button(
            " Probe ",
            [this]() { start_model_probe(); },
            ButtonOption::Animated());
        form_auth_button = Button(
            " Authenticate Copilot ",
            [this]() { start_copilot_auth(); },
            ButtonOption::Animated());
        form_save_button = Button(
            " Save ",
            [this]() { save_model_form(false); },
            ButtonOption::Animated());
        form_save_add_button = Button(
            " Save & add another ",
            [this]() { save_model_form(true); },
            ButtonOption::Animated());
        form_cancel_button = Button(
            " Cancel ",
            [this]() { request_close_model_form(); },
            ButtonOption::Animated());

        auto form_container = Container::Vertical({
            form_name_input,
            form_provider_radio,
            form_model_input,
            form_base_url_input,
            form_api_key_input,
            form_reveal_checkbox,
            form_context_input,
            form_timeout_input,
            form_capabilities_input,
            form_headers_input,
            Container::Horizontal({
                form_probe_button,
                form_auth_button,
            }),
            Container::Horizontal({
                form_save_button,
                form_save_add_button,
                form_cancel_button,
            }),
        });
        model_form_component = Renderer(form_container, [this]() {
            std::string probe_snapshot;
            std::vector<std::string> models_snapshot;
            std::string auth_snapshot;
            std::string code_snapshot;
            std::string url_snapshot;
            {
                std::lock_guard<std::mutex> lock(async_mutex);
                probe_snapshot = probe_status;
                models_snapshot = probed_models;
                auth_snapshot = auth_status;
                code_snapshot = auth_code;
                url_snapshot = auth_url;
            }
            const bool remote_provider = form_provider_index < 2;
            Elements form_rows = {
                text(
                    std::string(
                        model_form_edit ? " Edit model profile "
                                        : " Add model profile ") +
                    (model_form_dirty ? "* " : "")) |
                    bold | hcenter,
                separator() | color(theme().ui.text_dim),
                field_row("Name", form_name_input),
                text("Provider") | bold,
                form_provider_radio->Render() | border |
                    color(theme().ui.border),
                field_row("Model", form_model_input),
            };
            if (remote_provider) {
                form_rows.push_back(
                    field_row("Base URL", form_base_url_input));
                form_rows.push_back(
                    field_row("API key", form_api_key_input));
                form_rows.push_back(form_reveal_checkbox->Render());
            } else {
                form_rows.push_back(
                    paragraph(
                        "Copilot uses GitHub device authentication; no base "
                        "URL or API key is stored in this profile.") |
                    color(theme().ui.text_secondary));
            }
            form_rows.push_back(
                hbox({
                    field_row(
                        "Context window",
                        form_context_input) | flex,
                    field_row(
                        "Stream timeout (ms)",
                        form_timeout_input) | flex,
                }));
            form_rows.push_back(
                field_row(
                    "Capabilities",
                    form_capabilities_input,
                    "Comma-separated capability tags."));
            if (remote_provider) {
                form_rows.push_back(
                    field_row(
                        "Request headers (JSON)",
                        form_headers_input) |
                    size(HEIGHT, EQUAL, 7));
            }
            form_rows.push_back(hbox({
                form_probe_button->Render(),
                form_provider_index == 2
                    ? form_auth_button->Render()
                    : text(""),
            }));
            if (!probe_snapshot.empty()) {
                form_rows.push_back(
                    paragraph(probe_snapshot) |
                    color(
                        probe_snapshot.rfind("Error:", 0) == 0
                            ? theme().semantic.error
                            : theme().ui.accent_alt));
            }
            if (!models_snapshot.empty()) {
                form_rows.push_back(
                    paragraph(
                        "Discovered: " +
                        join_strings(
                            std::vector<std::string>(
                                models_snapshot.begin(),
                                models_snapshot.begin() +
                                    std::min<std::size_t>(
                                        8, models_snapshot.size())),
                            ", ")) |
                    color(theme().ui.text_secondary));
            }
            if (!auth_snapshot.empty()) {
                form_rows.push_back(
                    paragraph(auth_snapshot) |
                    color(theme().ui.accent_alt));
            }
            if (!code_snapshot.empty()) {
                form_rows.push_back(hbox({
                    text("Code: ") | bold,
                    text(code_snapshot) |
                        color(theme().ui.accent),
                    text("  " + url_snapshot) |
                        color(theme().markdown.link),
                }));
            }
            form_rows.push_back(
                status_line(form_status, form_status_error));
            form_rows.push_back(hbox({
                form_save_button->Render(),
                !model_form_edit
                    ? form_save_add_button->Render()
                    : text(""),
                form_cancel_button->Render(),
            }) | hcenter);
            return vbox(std::move(form_rows)) |
                size(WIDTH, EQUAL, 86) |
                size(HEIGHT, LESS_THAN, 34) |
                yframe | vscroll_indicator |
                border | color(theme().ui.border);
        });
        model_form_component =
            CatchEvent(model_form_component, [this](Event event) {
                if (event == Event::Escape) {
                    request_close_model_form();
                    return true;
                }
                if (event == Event::CtrlS) {
                    save_model_form(false);
                    return true;
                }
                return false;
            });
    }

    void build_confirmation_modals() {
        auto discard_keep = Button(
            " Cancel ",
            [this]() {
                discard_modal_open = false;
                navigation.cancel_pending_navigation();
                tab_index = previous_tab_index;
            },
            ButtonOption::Animated());
        auto save_changes = Button(
            " Save changes ",
            [this]() {
                const bool close_after_save =
                    navigation.close_requested();
                if (!save_current_editor()) return;
                navigation.save_and_continue();
                discard_modal_open = false;
                if (close_after_save) {
                    navigation.cancel_pending_navigation();
                    close_surface();
                } else {
                    tab_index = static_cast<int>(
                        navigation.active_tab());
                    previous_tab_index = tab_index;
                }
            },
            ButtonOption::Animated());
        auto discard_changes = Button(
            " Discard changes ",
            [this]() {
                const bool close_after_discard =
                    navigation.close_requested();
                navigation.discard_and_continue();
                sync_from_config();
                discard_modal_open = false;
                if (close_after_discard) {
                    navigation.cancel_pending_navigation();
                    close_surface();
                } else {
                    tab_index = static_cast<int>(
                        navigation.active_tab());
                    previous_tab_index = tab_index;
                }
            },
            ButtonOption::Animated());
        auto discard_container = Container::Horizontal({
            discard_keep,
            save_changes,
            discard_changes,
        });
        discard_modal_component = Renderer(
            discard_container,
            [discard_keep, save_changes, discard_changes]() {
                return vbox({
                    text(" Unsaved changes ") | bold | hcenter,
                    separator(),
                    paragraph(
                        "Save or discard the edits on this page before "
                        "leaving?"),
                    hbox({
                        discard_keep->Render(),
                        save_changes->Render(),
                        discard_changes->Render(),
                    }) | hcenter,
                }) | size(WIDTH, EQUAL, 66) | border;
            });
        discard_modal_component =
            CatchEvent(discard_modal_component, [this](Event event) {
                if (event != Event::Escape) return false;
                discard_modal_open = false;
                navigation.cancel_pending_navigation();
                tab_index = previous_tab_index;
                return true;
            });

        auto model_keep = Button(
            " Cancel ",
            [this]() { model_discard_modal_open = false; },
            ButtonOption::Animated());
        auto model_save = Button(
            " Save changes ",
            [this]() {
                if (save_model_form(false)) {
                    model_discard_modal_open = false;
                }
            },
            ButtonOption::Animated());
        auto model_discard = Button(
            " Discard changes ",
            [this]() { close_model_form_now(); },
            ButtonOption::Animated());
        auto model_discard_container = Container::Horizontal({
            model_keep,
            model_save,
            model_discard,
        });
        model_discard_modal_component = Renderer(
            model_discard_container,
            [model_keep, model_save, model_discard]() {
                return vbox({
                    text(" Unsaved model profile ") | bold | hcenter,
                    separator(),
                    paragraph(
                        "Save or discard this model profile before "
                        "closing the editor?"),
                    hbox({
                        model_keep->Render(),
                        model_save->Render(),
                        model_discard->Render(),
                    }) | hcenter,
                }) | size(WIDTH, EQUAL, 66) | border;
            });
        model_discard_modal_component =
            CatchEvent(
                model_discard_modal_component,
                [this](Event event) {
                    if (event != Event::Escape) return false;
                    model_discard_modal_open = false;
                    return true;
                });

        auto confirm_cancel = Button(
            " Cancel ",
            [this]() {
                confirm_modal_open = false;
                confirm_action = {};
            },
            ButtonOption::Animated());
        auto confirm_accept = Button(
            " Confirm ",
            [this]() {
                auto action = std::move(confirm_action);
                confirm_modal_open = false;
                if (action) action();
            },
            ButtonOption::Animated());
        auto confirm_container = Container::Horizontal({
            confirm_cancel,
            confirm_accept,
        });
        confirm_modal_component = Renderer(
            confirm_container,
            [this, confirm_cancel, confirm_accept]() {
                return vbox({
                    text(" " + confirm_title + " ") | bold | hcenter,
                    separator(),
                    paragraph(confirm_message),
                    hbox({
                        confirm_cancel->Render(),
                        confirm_accept->Render(),
                    }) | hcenter,
                }) | size(WIDTH, EQUAL, 62) | border |
                    color(theme().semantic.warning);
            });
        confirm_modal_component =
            CatchEvent(confirm_modal_component, [this](Event event) {
                if (event != Event::Escape) return false;
                confirm_modal_open = false;
                confirm_action = {};
                return true;
            });
    }
};

SettingsCenter::SettingsCenter(SettingsCenterDependencies dependencies)
    : impl_(std::make_unique<Impl>(std::move(dependencies))) {}

SettingsCenter::~SettingsCenter() = default;

ftxui::Component SettingsCenter::component() const {
    return impl_->root;
}

void SettingsCenter::open() {
    open(impl_->navigation.active_tab());
}

void SettingsCenter::open(SettingsTab tab) {
    ++impl_->async_generation;
    impl_->usage_loading = false;
    impl_->archived_loading = false;
    impl_->sync_from_config();
    impl_->navigation.set_dirty(false);
    impl_->navigation.set_active_tab_immediately(tab);
    impl_->tab_index = static_cast<int>(tab);
    impl_->previous_tab_index = impl_->tab_index;
    impl_->page_status.clear();
    impl_->page_status_error = false;
    if (tab == SettingsTab::Usage) impl_->refresh_usage();
    if (tab == SettingsTab::Archived) impl_->refresh_archived();
    impl_->post_event();
}

SettingsTab SettingsCenter::active_tab() const {
    return impl_->navigation.active_tab();
}

} // namespace acecode::tui::settings
