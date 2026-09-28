#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace acecode::computer_use::keyboard_input {

struct Key {
    WORD virtual_key = 0;
    bool extended = false;
};

struct Chord {
    std::vector<Key> keys;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};

namespace detail {
inline Key key(WORD value, bool extended = false) { return {value, extended}; }

inline std::optional<Key> parse_token(std::string token) {
    std::transform(token.begin(), token.end(), token.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    // These fixed virtual-key mappings match the existing Windows backend.
    // Punctuation uses US OEM virtual keys; the active keyboard layout still
    // determines the resulting character. We do not call VkKeyScanEx:
    // layout-dependent inferred Ctrl/Alt/Shift bits could change a shortcut.
    // Shifted keysyms (greater, question, etc.) select the same primary key;
    // callers must supply Shift explicitly. Use type_text for literal text.
    static const std::map<std::string, Key> named{
        {"ctrl", key(VK_CONTROL)}, {"control", key(VK_CONTROL)},
        {"control_l", key(VK_LCONTROL)}, {"control_r", key(VK_RCONTROL, true)},
        {"alt", key(VK_MENU)}, {"alt_l", key(VK_LMENU)}, {"alt_r", key(VK_RMENU, true)},
        {"shift", key(VK_SHIFT)}, {"shift_l", key(VK_LSHIFT)}, {"shift_r", key(VK_RSHIFT)},
        {"win", key(VK_LWIN, true)}, {"super", key(VK_LWIN, true)},
        {"super_l", key(VK_LWIN, true)}, {"super_r", key(VK_RWIN, true)}, {"meta", key(VK_LWIN, true)},
        {"enter", key(VK_RETURN)}, {"return", key(VK_RETURN)}, {"tab", key(VK_TAB)},
        {"escape", key(VK_ESCAPE)}, {"esc", key(VK_ESCAPE)}, {"space", key(VK_SPACE)},
        {"backspace", key(VK_BACK)}, {"back", key(VK_BACK)},
        {"delete", key(VK_DELETE, true)}, {"insert", key(VK_INSERT, true)},
        {"home", key(VK_HOME, true)}, {"end", key(VK_END, true)},
        {"left", key(VK_LEFT, true)}, {"right", key(VK_RIGHT, true)},
        {"up", key(VK_UP, true)}, {"down", key(VK_DOWN, true)},
        {"pageup", key(VK_PRIOR, true)}, {"page_up", key(VK_PRIOR, true)}, {"prior", key(VK_PRIOR, true)},
        {"pagedown", key(VK_NEXT, true)}, {"page_down", key(VK_NEXT, true)}, {"next", key(VK_NEXT, true)},
        {"capslock", key(VK_CAPITAL)},
        {"period", key(VK_OEM_PERIOD)}, {"greater", key(VK_OEM_PERIOD)},
        {".", key(VK_OEM_PERIOD)}, {">", key(VK_OEM_PERIOD)},
        {"comma", key(VK_OEM_COMMA)}, {"less", key(VK_OEM_COMMA)},
        {",", key(VK_OEM_COMMA)}, {"<", key(VK_OEM_COMMA)},
        {"minus", key(VK_OEM_MINUS)}, {"underscore", key(VK_OEM_MINUS)},
        {"-", key(VK_OEM_MINUS)}, {"_", key(VK_OEM_MINUS)},
        {"equal", key(VK_OEM_PLUS)}, {"=", key(VK_OEM_PLUS)},
        {"semicolon", key(VK_OEM_1)}, {"colon", key(VK_OEM_1)}, {";", key(VK_OEM_1)}, {":", key(VK_OEM_1)},
        {"slash", key(VK_OEM_2)}, {"question", key(VK_OEM_2)}, {"/", key(VK_OEM_2)}, {"?", key(VK_OEM_2)},
        {"backslash", key(VK_OEM_5)}, {"bar", key(VK_OEM_5)}, {"\\", key(VK_OEM_5)}, {"|", key(VK_OEM_5)},
        {"apostrophe", key(VK_OEM_7)}, {"quotedbl", key(VK_OEM_7)}, {"'", key(VK_OEM_7)}, {"\"", key(VK_OEM_7)},
        {"bracketleft", key(VK_OEM_4)}, {"braceleft", key(VK_OEM_4)}, {"[", key(VK_OEM_4)}, {"{", key(VK_OEM_4)},
        {"bracketright", key(VK_OEM_6)}, {"braceright", key(VK_OEM_6)}, {"]", key(VK_OEM_6)}, {"}", key(VK_OEM_6)},
        {"grave", key(VK_OEM_3)}, {"asciitilde", key(VK_OEM_3)}, {"`", key(VK_OEM_3)}, {"~", key(VK_OEM_3)},
        // Preserve the existing plus alias for the dedicated keypad key.
        {"plus", key(VK_ADD)},
        {"kp_add", key(VK_ADD)}, {"numpad_add", key(VK_ADD)},
        {"kp_subtract", key(VK_SUBTRACT)}, {"numpad_subtract", key(VK_SUBTRACT)},
        {"kp_multiply", key(VK_MULTIPLY)}, {"numpad_multiply", key(VK_MULTIPLY)},
        {"kp_divide", key(VK_DIVIDE, true)}, {"numpad_divide", key(VK_DIVIDE, true)},
        {"kp_decimal", key(VK_DECIMAL)}, {"numpad_decimal", key(VK_DECIMAL)},
        {"kp_enter", key(VK_RETURN, true)}, {"numpad_enter", key(VK_RETURN, true)},
    };
    if (const auto found = named.find(token); found != named.end()) return found->second;
    if (token.size() == 1 && ((token[0] >= 'a' && token[0] <= 'z') || (token[0] >= '0' && token[0] <= '9')))
        return key(static_cast<WORD>(std::toupper(static_cast<unsigned char>(token[0]))));
    if (token.size() >= 2 && token.size() <= 3 && token[0] == 'f') {
        unsigned number = 0;
        for (std::size_t i = 1; i < token.size(); ++i) {
            if (token[i] < '0' || token[i] > '9') return std::nullopt;
            number = number * 10 + static_cast<unsigned>(token[i] - '0');
        }
        if (number >= 1 && number <= 24 && token == "f" + std::to_string(number))
            return key(static_cast<WORD>(VK_F1 + number - 1));
    }
    for (const std::string_view prefix : {"kp_", "numpad_"}) {
        if (token.size() == prefix.size() + 1 && token.compare(0, prefix.size(), prefix) == 0 &&
            token.back() >= '0' && token.back() <= '9')
            return key(static_cast<WORD>(VK_NUMPAD0 + token.back() - '0'));
    }
    return std::nullopt;
}

inline WORD physical_key(WORD key) {
    // Windows resolves generic modifier virtual keys to their left variant.
    if (key == VK_CONTROL) return VK_LCONTROL;
    if (key == VK_SHIFT) return VK_LSHIFT;
    if (key == VK_MENU) return VK_LMENU;
    return key;
}

inline bool same_key(Key left, Key right) {
    return physical_key(left.virtual_key) == physical_key(right.virtual_key) && left.extended == right.extended;
}
} // namespace detail

inline Chord parse_chord(std::string_view text) {
    if (text.empty() || text.size() > 256)
        return {{}, "A nonempty key chord of at most 256 bytes is required."};
    std::vector<Key> keys;
    std::size_t begin = 0;
    for (;;) {
        const auto end = text.find('+', begin);
        auto token = text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
        const auto first = token.find_first_not_of(" \t");
        if (first == std::string_view::npos)
            return {{}, "A chord cannot contain empty keys. Use plus or Shift+equal for a plus key."};
        token = token.substr(first, token.find_last_not_of(" \t") - first + 1);
        const auto parsed = detail::parse_token(std::string(token));
        if (!parsed) return {{}, "Unsupported key name. Use named keys or type_text for literal Unicode text."};
        if (std::any_of(keys.begin(), keys.end(), [&](Key prior) { return detail::same_key(prior, *parsed); }))
            return {{}, "A chord cannot repeat a key."};
        keys.push_back(*parsed);
        if (keys.size() > 8) return {{}, "A chord can contain at most eight keys."};
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return {std::move(keys), {}};
}

// Construct balanced down/up records without touching the desktop. Callers send
// all downs in chord order and all ups in reverse order in one native batch.
inline INPUT make_input(Key key, bool up) {
    INPUT result{};
    result.type = INPUT_KEYBOARD;
    result.ki.wVk = key.virtual_key;
    result.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (key.extended ? KEYEVENTF_EXTENDEDKEY : 0);
    return result;
}

} // namespace acecode::computer_use::keyboard_input
#endif // _WIN32
