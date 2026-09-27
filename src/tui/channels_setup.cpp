#include "channels_setup.hpp"
#include "theme_palette.hpp"
#include "terminal_key_event.hpp"
#include "config/config.hpp"
#include "desktop/locale.hpp"
#include "utils/open_url.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace acecode::tui {
using namespace ftxui;
using channels::SetupPhase;

struct ChannelsSetup::Impl {
    enum Page { Mode, Contacts, Review, Progress, Result };
    ChannelsSetupDependencies deps;
    Component root, pages, mode, contacts, review, progress, result;
    Component next_mode, next_contacts, connect, retry, finish;
    std::vector<std::string> modes;
    int page = Mode, selected_mode = 0;
    std::string numbers, validation;
    std::vector<std::string> peers;
    std::thread worker;
    std::atomic<bool> cancelled{false}, done{true};
    std::mutex mu;
    channels::SetupUpdate update;
    bool closing = false, closed = false;
    Box body_box;

    const char* tr(const char* zh, const char* en) const { return deps.english ? en : zh; }
    Component button(const char* zh, const char* en, std::function<void()> action) {
        return Button(tr(zh, en), std::move(action), ButtonOption::Animated());
    }
    Component cancel_button() {
        return button(u8"取消", "Cancel", [this] { cancel(); });
    }
    channels::SetupUpdate snapshot() {
        std::lock_guard<std::mutex> lock(mu);
        return update;
    }
    void set_page(Page value) {
        page = value;
        validation.clear();
        if (page == Mode) mode->ChildAt(0)->TakeFocus();
        else if (page == Contacts) contacts->ChildAt(0)->TakeFocus();
        else pages->ActiveChild()->TakeFocus();
    }
    void focus_controls(const Component& component, Components& controls) {
        if (!component->Focusable()) return;
        if (!component->ChildCount()) { controls.push_back(component); return; }
        for (size_t i = 0; i < component->ChildCount(); ++i) focus_controls(component->ChildAt(i), controls);
    }
    void tab(bool reverse) {
        Components controls;
        focus_controls(pages->ActiveChild(), controls);
        if (controls.empty()) return;
        int current = 0;
        for (size_t i = 0; i < controls.size(); ++i) if (controls[i]->Focused()) current = static_cast<int>(i);
        const int count = static_cast<int>(controls.size());
        controls[(current + count + (reverse ? -1 : 1)) % count]->TakeFocus();
    }
    void cancel() {
        if (!done) { closing = true; cancelled = true; return; }
        if (worker.joinable()) worker.join();
        if (closed) return;
        closed = true;
        if (deps.request_close) deps.request_close();
    }
    void sync() {
        if (page != Progress || !done) return;
        if (worker.joinable()) worker.join();
        if (closing || snapshot().phase == SetupPhase::Cancelled) {
            cancel();
        } else {
            set_page(Result);
            (snapshot().phase == SetupPhase::Complete ? finish : retry)->TakeFocus();
        }
    }
    void start() {
        if (!done) return;
        if (worker.joinable()) worker.join();
        cancelled = false; closing = false; done = false;
        { std::lock_guard<std::mutex> lock(mu); update = {}; }
        set_page(Progress);
        // The worker publishes plain data only; FTXUI components stay on the UI thread.
        worker = std::thread([this, allowed = peers] {
            channels::run_setup(allowed, deps.setup, cancelled, [this](const channels::SetupUpdate& value) {
                { std::lock_guard<std::mutex> lock(mu); update = value; }
                if (deps.post_event) deps.post_event();
            });
            done = true;
            if (deps.post_event) deps.post_event();
        });
    }
    Element line(const std::string& value) { return paragraph(value) | color(theme().ui.text_muted); }
    Element qr(const std::string& value) {
        Elements rows;
        std::istringstream input(value);
        std::string row;
        int width = 0;
        std::vector<std::string> lines;
        while (std::getline(input, row)) {
            if (!row.empty() && row.back() == '\r') row.pop_back();
            width = std::max(width, string_width(row));
            lines.push_back(row);
        }
        const int required_width = width + 4;
        const int required_height = static_cast<int>(lines.size()) + 2;
        auto available = Dimensions{body_box.x_max - body_box.x_min + 1, body_box.y_max - body_box.y_min + 1};
        if (deps.terminal_dimensions) {
            available = deps.terminal_dimensions();
            available.dimy -= 7; // Header, separators, buttons, and the next shell line.
        }
        if (required_width > available.dimx || required_height + 3 > available.dimy) {
            return line(std::string(tr(u8"请放大终端以显示完整二维码，至少需要 ",
                                       "Enlarge the terminal to show the full QR code: at least ")) +
                        std::to_string(required_width + 4) + " x " + std::to_string(required_height + 11));
        }
        const std::string white = u8"\u2588";
        std::string border_row;
        for (int i = 0; i < required_width; ++i) border_row += white;
        rows.push_back(text(border_row));
        for (auto& item : lines) {
            const int padding = required_width - 2 - string_width(item);
            item = white + white + item;
            for (int i = 0; i < padding; ++i) item += white;
            rows.push_back(text(item));
        }
        rows.push_back(text(border_row));
        return vbox(std::move(rows)) | color(Color::White) | bgcolor(Color::Black);
    }
    Element body() {
        if (page == Mode) return vbox({
            text(tr(u8"1. 选择使用方式", "1. Choose how to use WhatsApp")) | bold,
            text(""), mode->ChildAt(0)->Render(), text(""),
            line(tr(u8"默认使用个人号的“给自己发消息”。其他联系人需要明确授权。",
                    "Use your personal account's Message yourself chat. Other contacts need explicit access."))});
        if (page == Contacts) return vbox({
            text(tr(u8"2. 允许哪些联系人？", "2. Which contacts are allowed?")) | bold,
            text(""), line(tr(u8"输入带国家代码的手机号，多个号码用逗号分隔。", "Phone numbers with country codes, separated by commas.")),
            contacts->ChildAt(0)->Render() | border, text(""),
            line(tr(u8"自己的账号会自动授权；已有授权和会话会保留。", "Your own account is allowed automatically. Existing access and conversations are retained.")),
            paragraph(validation) | color(theme().semantic.error)});
        if (page == Review) return vbox({
            text(tr(u8"3. 确认配置", "3. Review configuration")) | bold, text(""),
            line(tr(u8"账号：个人 WhatsApp，扫码关联", "Account: personal WhatsApp, linked by QR code")),
            line(std::string(tr(u8"访问：自己", "Access: yourself")) + (peers.empty() ? "" :
                std::string(tr(u8"，新增联系人 ", ", additional contacts: ")) + std::to_string(peers.size()))),
            text(""), line(tr(u8"已关联的账号直接复用。首次关联需要 Node.js 22+，桥接依赖会自动安装。",
                              "Reuses the saved account. First-time linking requires Node.js 22+ and installs bridge dependencies automatically.")),
            text(""),
            line(tr(u8"此方式使用非官方 WhatsApp Web 协议，可能失效或受账号限制。", "This uses the unofficial WhatsApp Web protocol and may break or be subject to account restrictions."))});
        const auto current = snapshot();
        if (page == Progress) {
            if (closing) return line(tr(u8"正在取消并清理连接...", "Cancelling and cleaning up the connection..."));
            if (current.phase == SetupPhase::Pairing && !current.qr_text.empty()) return vbox({
                text(tr(u8"4. 扫码关联", "4. Scan to link")) | bold,
                line(tr(u8"WhatsApp 设置 > 已关联的设备 > 关联设备", "WhatsApp Settings > Linked devices > Link a device")),
                text(""), qr(current.qr_text)});
            const char* status = current.phase == SetupPhase::Installing ? tr(u8"正在安装桥接依赖...", "Installing bridge dependencies...") :
                current.phase == SetupPhase::Preparing ? tr(u8"正在读取配置...", "Reading configuration...") :
                tr(u8"正在准备账号...", "Preparing the account...");
            return vbox({text(tr(u8"4. 关联 WhatsApp", "4. Link WhatsApp")) | bold, text(""), line(status)});
        }
        if (current.phase == SetupPhase::Complete) {
            const auto phone = current.account.substr(0, current.account.find('@'));
            return vbox({text(tr(u8"5. 配置完成", "5. Setup complete")) | bold | color(theme().semantic.success),
                text(""), line("WhatsApp: +" + phone),
                line(tr(u8"账号和授权已保存。", "Account and access saved."))});
        }
        return vbox({text(tr(u8"配置未完成", "Setup not completed")) | bold | color(theme().semantic.error),
            text(""), paragraph(current.detail), text(""),
            line(tr(u8"原有授权和会话会保留。解决上方问题后可以重试。", "Existing access and conversations are retained. Resolve the issue above, then retry.")),
            paragraph(validation) | color(theme().semantic.error)});
    }
    Component footer() {
        if (page == Mode) return mode->ChildAt(1);
        if (page == Contacts) return contacts->ChildAt(1);
        return pages->ActiveChild();
    }
    explicit Impl(ChannelsSetupDependencies value) : deps(std::move(value)) {
        if (!deps.setup.begin) deps.setup = channels::default_setup_dependencies();
        modes = {tr(u8"仅自己使用（推荐）", "Just myself (recommended)"), tr(u8"自己和指定联系人", "Myself and selected contacts")};
        RadioboxOption options;
        options.focused_entry = &selected_mode;
        auto choices = Radiobox(&modes, &selected_mode, options);
        next_mode = button(u8"继续", "Continue", [this] {
            peers.clear(); set_page(selected_mode == 0 ? Review : Contacts);
            if (selected_mode == 0) connect->TakeFocus();
        });
        mode = Container::Vertical({choices, Container::Horizontal({next_mode, cancel_button()})});
        next_contacts = button(u8"继续", "Continue", [this] {
            try {
                peers = channels::setup_contacts(numbers);
                if (peers.empty()) {
                    validation = tr(u8"请输入至少一个手机号，或返回选择仅自己使用。", "Enter a phone number, or go back and choose Just myself.");
                    return;
                }
                set_page(Review); connect->TakeFocus();
            } catch (const std::exception&) {
                validation = tr(u8"号码格式不正确，请包含国家代码并用逗号分隔。", "Invalid phone numbers. Include country codes and separate numbers with commas.");
            }
        });
        InputOption input;
        input.multiline = false;
        input.on_enter = [this] { next_contacts->OnEvent(Event::Return); };
        contacts = Container::Vertical({Input(&numbers, "+886912345678, +12025550123", input),
            Container::Horizontal({next_contacts, button(u8"上一步", "Back", [this] { set_page(Mode); }), cancel_button()})});
        connect = button(u8"保存配置", "Save configuration", [this] { start(); });
        review = Container::Horizontal({connect, button(u8"上一步", "Back", [this] {
            set_page(selected_mode == 0 ? Mode : Contacts);
        }), cancel_button()});
        progress = Container::Horizontal({cancel_button()});
        retry = button(u8"重试", "Retry", [this] { start(); });
        finish = button(u8"完成", "Finish", [this] { cancel(); });
        result = Container::Horizontal({
            Maybe(finish, [this] { return snapshot().phase == SetupPhase::Complete; }),
            Maybe(retry, [this] { return snapshot().phase != SetupPhase::Complete; }),
            Maybe(button(u8"下载 Node.js", "Download Node.js", [this] {
                const auto opened = open_url_in_browser("https://nodejs.org/en/download");
                if (!opened.ok) validation = opened.error;
            }), [this] { return snapshot().detail.find("Node.js") != std::string::npos; }),
            Maybe(button(u8"上一步", "Back", [this] { set_page(Review); connect->TakeFocus(); }),
                  [this] { return snapshot().phase != SetupPhase::Complete; }),
            Maybe(cancel_button(), [this] { return snapshot().phase != SetupPhase::Complete; })});
        pages = Container::Tab({mode, contacts, review, progress, result}, &page);
        auto rendered = Renderer(pages, [this] {
            auto document = vbox({text(tr(u8"WhatsApp 配置", "WhatsApp Setup")) | bold | color(theme().ui.accent),
                separator(), body() | yframe | flex | reflect(body_box), separator(), footer()->Render()}) |
                color(theme().ui.text_primary);
            if (!deps.terminal_dimensions) return document;
            const auto terminal = deps.terminal_dimensions();
            const int rows = std::max(1, terminal.dimy - 1);
            // Resolve wrapped paragraphs before TerminalOutput chooses its height.
            Screen measure(std::max(1, terminal.dimx), rows);
            Render(measure, document);
            document->ComputeRequirement();
            return document | size(HEIGHT, EQUAL, std::clamp(document->requirement().min_y, 1, rows));
        });
        root = CatchEvent(rendered, [this](Event event) {
            sync();
            if (closed) return true;
            if (event == Event::Custom) return true;
            if (matches_terminal_key(event, TerminalKey::Escape)) { cancel(); return true; }
            if (closing) return true;
            if (event == Event::Tab || event == Event::TabReverse) { tab(event == Event::TabReverse); return true; }
            if (page == Mode && mode->ChildAt(0)->Focused() && event == Event::Return)
                return next_mode->OnEvent(event);
            return false;
        });
    }
    void shutdown() {
        cancelled = true;
        if (worker.joinable()) worker.join();
    }
};

ChannelsSetup::ChannelsSetup(ChannelsSetupDependencies dependencies) : impl_(std::make_unique<Impl>(std::move(dependencies))) {}
ChannelsSetup::~ChannelsSetup() { shutdown(); }
Component ChannelsSetup::component() const { return impl_->root; }
void ChannelsSetup::open() {
    impl_->shutdown(); impl_->closing = false; impl_->closed = false; impl_->cancelled = false;
    impl_->selected_mode = 0; impl_->numbers.clear(); impl_->peers.clear();
    impl_->set_page(Impl::Mode);
    impl_->mode->ChildAt(0)->OnEvent(Event::Home);
}
void ChannelsSetup::shutdown() { impl_->shutdown(); }

ScreenInteractive make_channels_setup_terminal() {
    return ScreenInteractive::TerminalOutput();
}

int run_channels_setup() {
#ifdef _WIN32
    const bool interactive = _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    const bool interactive = isatty(fileno(stdin)) && isatty(fileno(stdout));
#endif
    if (!interactive) throw std::runtime_error("WhatsApp setup needs an interactive terminal. Run acecode channels directly in a terminal.");
    const auto config = load_config();
    init_theme_palette(config.tui.theme == "light" ? "light" : "dark");
    auto screen = make_channels_setup_terminal();
    ChannelsSetup wizard({screen.ExitLoopClosure(), [&screen] { screen.PostEvent(Event::Custom); },
        channels::default_setup_dependencies(), desktop::resolve_ui_locale(config.ui.locale,
            desktop::detect_system_locale_tag()) == "en-US", [] { return Terminal::Size(); }});
    wizard.open();
    screen.Loop(wizard.component());
    wizard.shutdown();
    return 0;
}
} // namespace acecode::tui
