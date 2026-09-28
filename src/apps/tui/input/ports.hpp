#pragma once
#include "tui/screen_port.hpp"
#include "tui/input/input_disposition.hpp"
#include "platform/clipboard.hpp"
#include <functional>
#include <string>
namespace acecode { struct UserInput; struct CommandContext; enum class PermissionResult; }

namespace acecode::tui {
class ITurnSubmitter {
public:
    virtual ~ITurnSubmitter() = default;
    virtual std::string cwd() const = 0;
    virtual void cancel() = 0;
    virtual void submit_shell(const std::string& command) = 0;
    virtual void before_first_turn() = 0;
    virtual void submit_input(const UserInput& input) = 0;
    virtual void submit_text(const std::string& text, const std::string& display_text = {}) = 0;
};
class ICommandContextFactory {
public:
    virtual ~ICommandContextFactory() = default;
    virtual CommandContext make(bool track_command_usage) = 0;
};
class IFullScreenSurfaces {
public:
    virtual ~IFullScreenSurfaces() = default;
    // Borrowed only during events; construction must not call these methods.
    virtual bool open_settings(const std::string& tab, std::string& error) = 0;
    virtual bool open_management(const std::string& tab, std::string& error) = 0;
};
class IClipboard {
public:
    virtual ~IClipboard() = default;
    virtual ClipboardTextReadResult read_text() = 0;
    virtual ClipboardImageReadResult read_image() = 0;
    virtual ClipboardTextWriteResult write_text(const std::string& text) = 0;
    virtual void write_osc52(const std::string& text) = 0;
};
// Borrowed for one handler call; never stored or captured by asynchronous work.
using PermissionResponder = std::function<void(const std::string&, const std::string&, PermissionResult)>;
}
