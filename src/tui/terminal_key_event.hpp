#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <ftxui/component/event.hpp>

namespace acecode { namespace tui {

enum class TerminalKey {
    Codepoint,
    Escape,
    Enter,
    Tab,
    Backspace,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
};

enum class TerminalKeyModifier : std::uint16_t {
    Shift = 1U << 0U,
    Alt = 1U << 1U,
    Ctrl = 1U << 2U,
    Super = 1U << 3U,
    Hyper = 1U << 4U,
    Meta = 1U << 5U,
    CapsLock = 1U << 6U,
    NumLock = 1U << 7U,
};

using TerminalKeyModifiers = std::uint16_t;

constexpr TerminalKeyModifiers terminal_modifier(TerminalKeyModifier modifier) {
    return static_cast<TerminalKeyModifiers>(modifier);
}

constexpr TerminalKeyModifiers operator|(TerminalKeyModifier lhs,
                                         TerminalKeyModifier rhs) {
    return terminal_modifier(lhs) | terminal_modifier(rhs);
}

constexpr TerminalKeyModifiers operator|(TerminalKeyModifiers lhs,
                                         TerminalKeyModifier rhs) {
    return lhs | terminal_modifier(rhs);
}

enum class TerminalKeyAction {
    Press,
    Repeat,
    Release,
};

struct TerminalKeyEvent {
    TerminalKey key = TerminalKey::Codepoint;
    std::uint32_t codepoint = 0;
    std::optional<std::uint32_t> shifted_codepoint;
    std::optional<std::uint32_t> base_layout_codepoint;
    TerminalKeyModifiers modifiers = 0;
    TerminalKeyAction action = TerminalKeyAction::Press;
    std::vector<std::uint32_t> text_codepoints;
    bool kitty = false;
};

// Decode one complete terminal key encoding. Invalid, incomplete, control-mode,
// mouse, paste-marker, and terminal-response sequences return std::nullopt.
std::optional<TerminalKeyEvent> decode_terminal_key(std::string_view input);
std::optional<TerminalKeyEvent> decode_terminal_key(const ftxui::Event& event);

// Match press/repeat events. Release events never trigger shortcuts. Lock
// modifiers are ignored by default; callers may also ignore Shift where legacy
// control-letter encodings could not distinguish it.
bool matches_terminal_key(
    const ftxui::Event& event,
    TerminalKey key,
    TerminalKeyModifiers modifiers = 0,
    TerminalKeyModifiers ignored_modifiers =
        terminal_modifier(TerminalKeyModifier::CapsLock) |
        TerminalKeyModifier::NumLock);

bool matches_terminal_codepoint(
    const ftxui::Event& event,
    std::uint32_t codepoint,
    TerminalKeyModifiers modifiers = 0,
    TerminalKeyModifiers ignored_modifiers =
        terminal_modifier(TerminalKeyModifier::CapsLock) |
        TerminalKeyModifier::NumLock);

}} // namespace acecode::tui
