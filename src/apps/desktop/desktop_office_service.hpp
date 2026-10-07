#pragma once

#include "utils/atomic_file.hpp"
#include "utils/semver.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>

namespace acecode::desktop {

// GUI-thread owner. Bridge lifetime is independent of disposable native windows.
// Host/Window are injected so lifecycle and persistence can be tested headlessly.
template <typename Host, typename Window>
class DesktopOfficeService : public std::enable_shared_from_this<DesktopOfficeService<Host, Window>> {
public:
    DesktopOfficeService(Host& host, std::filesystem::path path, const std::string& version,
                         bool available, std::function<std::string()> preview)
        : host_(host), path_(std::move(path)), available_(available), preview_(std::move(preview)) {
        const auto current = upgrade::parse_sem_version(version);
        const auto minimum = upgrade::parse_sem_version("0.9.37");
        eligible_ = current && minimum && upgrade::compare_sem_version(*current, *minimum) >= 0;
        std::ifstream in(path_);
        if (in) {
            const auto data = nlohmann::json::parse(in, nullptr, false);
            if (data.is_object()) {
                enabled_ = data.value("enabled", nlohmann::json()) == true;
                welcome_seen_ = data.value("welcome_seen", nlohmann::json()) == true;
            }
        }
        enabled_ = enabled_ && available_;
    }

    ~DesktopOfficeService() { destroy_window(); }

    void bind_bridge() {
        const auto weak = this->weak_from_this();
        host_.bind("aceDesktop_getOfficePreferences", [weak](const std::string&) {
            const auto self = weak.lock();
            return self ? self->state().dump() : std::string("{}");
        });
        host_.bind("aceDesktop_setOfficeEnabled", [weak](const std::string& request) {
            const auto self = weak.lock();
            const auto args = nlohmann::json::parse(request, nullptr, false);
            if (!self || !args.is_array() || args.size() != 1 || !args[0].is_boolean())
                return std::string("{\"ok\":false}");
            return self->set_enabled(args[0].get<bool>()).dump();
        });
        host_.bind("aceDesktop_claimOfficeWelcome", [weak](const std::string&) {
            const auto self = weak.lock();
            if (!self) return std::string("{\"ok\":false}");
            const bool show = self->state()["welcomePending"];
            if (show && !self->save(self->enabled_, true)) return self->failure("save_failed").dump();
            if (show) self->welcome_seen_ = true;
            auto result = self->state();
            result["show"] = show;
            return result.dump();
        });
        host_.bind("aceDesktop_getOfficePreview", [weak](const std::string&) {
            const auto self = weak.lock();
            return self && self->available_ ? nlohmann::json(self->preview_()).dump() : std::string("null");
        });
        host_.bind("aceDesktop_getOfficeState", [weak](const std::string&) {
            const auto self = weak.lock();
            return self ? self->snapshot_.dump() : std::string("{}");
        });
        host_.bind("aceDesktop_updateOffice", [weak](const std::string& request) {
            const auto self = weak.lock();
            if (!self || !self->enabled_ || request.size() > 512 * 1024) return std::string("false");
            try {
                const auto args = nlohmann::json::parse(request);
                if (!args.is_array() || args.size() != 1 || !args[0].is_string()) return std::string("false");
                const auto value = nlohmann::json::parse(args[0].get<std::string>());
                if (!value.is_object() || value.value("version", 0) != 1 ||
                    !value.contains("agents") || !value["agents"].is_array() ||
                    !value.contains("offices") || !value["offices"].is_array() || value["offices"].size() > 5)
                    return std::string("false");
                self->snapshot_ = value;
                if (self->window_) self->window_->update_snapshot(value);
                return std::string("true");
            } catch (...) { return std::string("false"); }
        });
        if (enabled_ && !open_window()) enabled_ = false;
    }

    nlohmann::json state() const {
        return {{"ok", true}, {"available", available_}, {"enabled", enabled_},
                {"welcomePending", available_ && eligible_ && !welcome_seen_}};
    }

    nlohmann::json set_enabled(bool enabled) {
        if (!available_) return failure("unavailable");
        if (!save(enabled, true)) return failure("save_failed");
        welcome_seen_ = true;
        enabled_ = enabled;
        if (enabled && (!window_ || window_closed_)) {
            destroy_window();
            if (!open_window()) {
                enabled_ = false;
                (void)save(false, true);
                notify("open_failed");
                return failure("open_failed");
            }
        }
        if (!enabled) destroy_window();
        notify();
        return state();
    }

private:
    bool save(bool enabled, bool welcome_seen) const {
        return atomic_write_file(path_to_utf8(path_),
            nlohmann::json{{"enabled", enabled}, {"welcome_seen", welcome_seen}}.dump());
    }

    nlohmann::json failure(const std::string& error) const {
        auto result = state();
        result["ok"] = false;
        result["error"] = error;
        return result;
    }

    void notify(const std::string& error = {}) {
        host_.eval("window.dispatchEvent(new CustomEvent('ace-desktop-office-preferences',{detail:" +
                   (error.empty() ? state() : failure(error)).dump() + "}));");
    }

    bool open_window() {
        auto window = std::make_shared<Window>(host_);
        const auto weak = this->weak_from_this();
        const std::weak_ptr<Window> weak_window = window;
        window->on_closed = [weak, weak_window] {
            const auto self = weak.lock();
            const auto closed = weak_window.lock();
            if (!self || !closed || self->window_ != closed) return;
            self->enabled_ = false;
            const bool saved = self->save(false, self->welcome_seen_);
            // Keep the controller alive until the next toggle, outside its callback.
            self->window_closed_ = true;
            self->notify(saved ? "" : "save_failed");
        };
        window_ = window;
        window_closed_ = false;
        window->update_snapshot(snapshot_);
        if (window->start()) return true;
        destroy_window();
        return false;
    }

    void destroy_window() {
        if (!window_) return;
        auto window = std::move(window_);
        window->on_closed = {};
        window->close();
    }

    Host& host_; // Borrowed; service is destroyed before the GUI host.
    std::filesystem::path path_;
    bool available_ = false;
    bool eligible_ = false;
    bool enabled_ = false;
    bool welcome_seen_ = false;
    bool window_closed_ = false;
    std::function<std::string()> preview_;
    nlohmann::json snapshot_ = {{"follow", true}};
    // Service owns the window; in-flight native callbacks briefly share it.
    std::shared_ptr<Window> window_;
};

} // namespace acecode::desktop
