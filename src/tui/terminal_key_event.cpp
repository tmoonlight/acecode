#include "tui/terminal_key_event.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <string_view>
#include <vector>

namespace acecode { namespace tui {
namespace {

constexpr std::string_view kCsi = "\x1B[";
constexpr std::string_view kSs3 = "\x1BO";
constexpr std::size_t kMaxSequenceBytes = 512;

bool is_unicode_scalar(std::uint32_t value, bool allow_zero = false) {
    if (value == 0) {
        return allow_zero;
    }
    return value <= 0x10FFFFU &&
           !(0xD800U <= value && value <= 0xDFFFU);
}

bool parse_unsigned(std::string_view text, std::uint32_t* value) {
    if (!value || text.empty()) {
        return false;
    }
    std::uint32_t result = 0;
    for (const unsigned char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::uint32_t digit = ch - '0';
        if (result > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }
    *value = result;
    return true;
}

std::vector<std::string_view> split_preserving_empty(std::string_view text,
                                                     char separator) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t end = text.find(separator, begin);
        if (end == std::string_view::npos) {
            fields.push_back(text.substr(begin));
            return fields;
        }
        fields.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
}

std::optional<TerminalKeyModifiers> decode_modifiers(
    std::string_view encoded_text) {
    if (encoded_text.empty()) {
        return TerminalKeyModifiers{0};
    }
    std::uint32_t encoded = 0;
    if (!parse_unsigned(encoded_text, &encoded) || encoded < 1U ||
        encoded > 256U) {
        return std::nullopt;
    }
    return static_cast<TerminalKeyModifiers>(encoded - 1U);
}

std::optional<TerminalKeyAction> decode_action(std::string_view text) {
    if (text.empty()) {
        return TerminalKeyAction::Press;
    }
    std::uint32_t value = 0;
    if (!parse_unsigned(text, &value)) {
        return std::nullopt;
    }
    switch (value) {
        case 1:
            return TerminalKeyAction::Press;
        case 2:
            return TerminalKeyAction::Repeat;
        case 3:
            return TerminalKeyAction::Release;
        default:
            return std::nullopt;
    }
}

TerminalKey key_from_codepoint(std::uint32_t codepoint) {
    switch (codepoint) {
        case 27:
            return TerminalKey::Escape;
        case 13:
            return TerminalKey::Enter;
        case 9:
            return TerminalKey::Tab;
        case 8:
        case 127:
            return TerminalKey::Backspace;
        default:
            return TerminalKey::Codepoint;
    }
}

TerminalKeyEvent make_codepoint_event(std::uint32_t codepoint,
                                      TerminalKeyModifiers modifiers,
                                      bool kitty) {
    TerminalKeyEvent result;
    result.key = key_from_codepoint(codepoint);
    result.codepoint = codepoint;
    result.modifiers = modifiers;
    result.kitty = kitty;
    return result;
}

std::optional<TerminalKeyEvent> decode_kitty_csi_u(std::string_view body) {
    if (body.empty() || body.front() == '?' || body.front() == '>' ||
        body.front() == '<') {
        return std::nullopt;
    }

    const auto fields = split_preserving_empty(body, ';');
    if (fields.empty() || fields.size() > 3U) {
        return std::nullopt;
    }

    const auto key_fields = split_preserving_empty(fields[0], ':');
    if (key_fields.empty() || key_fields.size() > 3U ||
        key_fields[0].empty()) {
        return std::nullopt;
    }

    std::uint32_t codepoint = 0;
    if (!parse_unsigned(key_fields[0], &codepoint) ||
        !is_unicode_scalar(codepoint, true)) {
        return std::nullopt;
    }

    TerminalKeyEvent result = make_codepoint_event(codepoint, 0, true);
    if (key_fields.size() >= 2U && !key_fields[1].empty()) {
        std::uint32_t shifted = 0;
        if (!parse_unsigned(key_fields[1], &shifted) ||
            !is_unicode_scalar(shifted)) {
            return std::nullopt;
        }
        result.shifted_codepoint = shifted;
    }
    if (key_fields.size() == 3U && !key_fields[2].empty()) {
        std::uint32_t base = 0;
        if (!parse_unsigned(key_fields[2], &base) ||
            !is_unicode_scalar(base)) {
            return std::nullopt;
        }
        result.base_layout_codepoint = base;
    }

    if (fields.size() >= 2U) {
        const auto modifier_fields = split_preserving_empty(fields[1], ':');
        if (modifier_fields.empty() || modifier_fields.size() > 2U) {
            return std::nullopt;
        }
        // The modifier field may be empty only when it is being skipped to
        // reach associated text (for example, CSI 0;;229u). When an event
        // type is present, the protocol requires an explicit modifier value.
        if (modifier_fields.size() == 2U && modifier_fields[0].empty()) {
            return std::nullopt;
        }
        const auto modifiers = decode_modifiers(modifier_fields[0]);
        if (!modifiers) {
            return std::nullopt;
        }
        result.modifiers = *modifiers;
        if (modifier_fields.size() == 2U) {
            const auto action = decode_action(modifier_fields[1]);
            if (!action) {
                return std::nullopt;
            }
            result.action = *action;
        }
    }

    if (result.shifted_codepoint &&
        (result.modifiers &
         terminal_modifier(TerminalKeyModifier::Shift)) == 0U) {
        return std::nullopt;
    }

    if (fields.size() == 3U) {
        if (fields[2].empty()) {
            return std::nullopt;
        }
        for (const auto item : split_preserving_empty(fields[2], ':')) {
            std::uint32_t text_codepoint = 0;
            if (!parse_unsigned(item, &text_codepoint) ||
                !is_unicode_scalar(text_codepoint) ||
                text_codepoint < 0x20U ||
                (0x7FU <= text_codepoint && text_codepoint <= 0x9FU)) {
                return std::nullopt;
            }
            result.text_codepoints.push_back(text_codepoint);
        }
    }
    // Key number zero is reserved for text that has no associated key.
    if (codepoint == 0U && result.text_codepoints.empty()) {
        return std::nullopt;
    }
    return result;
}

std::optional<TerminalKey> tilde_key(std::uint32_t number) {
    switch (number) {
        case 1:
        case 7:
            return TerminalKey::Home;
        case 2:
            return TerminalKey::Insert;
        case 3:
            return TerminalKey::Delete;
        case 4:
        case 8:
            return TerminalKey::End;
        case 5:
            return TerminalKey::PageUp;
        case 6:
            return TerminalKey::PageDown;
        case 11:
            return TerminalKey::F1;
        case 12:
            return TerminalKey::F2;
        case 13:
            return TerminalKey::F3;
        case 14:
            return TerminalKey::F4;
        case 15:
            return TerminalKey::F5;
        case 17:
            return TerminalKey::F6;
        case 18:
            return TerminalKey::F7;
        case 19:
            return TerminalKey::F8;
        case 20:
            return TerminalKey::F9;
        case 21:
            return TerminalKey::F10;
        case 23:
            return TerminalKey::F11;
        case 24:
            return TerminalKey::F12;
        default:
            return std::nullopt;
    }
}

std::optional<TerminalKey> csi_final_key(char final) {
    switch (final) {
        case 'A':
            return TerminalKey::ArrowUp;
        case 'B':
            return TerminalKey::ArrowDown;
        case 'C':
            return TerminalKey::ArrowRight;
        case 'D':
            return TerminalKey::ArrowLeft;
        case 'F':
            return TerminalKey::End;
        case 'H':
            return TerminalKey::Home;
        case 'P':
            return TerminalKey::F1;
        case 'Q':
            return TerminalKey::F2;
        case 'S':
            return TerminalKey::F4;
        default:
            return std::nullopt;
    }
}

std::optional<TerminalKeyEvent> decode_legacy_csi(std::string_view input) {
    const char final = input.back();
    const std::string_view body =
        input.substr(kCsi.size(), input.size() - kCsi.size() - 1U);
    if (!body.empty() &&
        (body.front() == '?' || body.front() == '>' || body.front() == '<')) {
        return std::nullopt;
    }

    if (final == '~') {
        const auto fields = split_preserving_empty(body, ';');
        if (fields.empty() || fields.size() > 2U || fields[0].empty()) {
            return std::nullopt;
        }
        std::uint32_t number = 0;
        if (!parse_unsigned(fields[0], &number)) {
            return std::nullopt;
        }
        const auto key = tilde_key(number);
        if (!key) {
            return std::nullopt;
        }
        TerminalKeyModifiers modifiers = 0;
        if (fields.size() == 2U) {
            if (fields[1].empty()) {
                return std::nullopt;
            }
            const auto decoded = decode_modifiers(fields[1]);
            if (!decoded) {
                return std::nullopt;
            }
            modifiers = *decoded;
        }
        TerminalKeyEvent result;
        result.key = *key;
        result.modifiers = modifiers;
        return result;
    }

    if (final == 'Z') {
        if (!body.empty()) {
            return std::nullopt;
        }
        TerminalKeyEvent result;
        result.key = TerminalKey::Tab;
        result.codepoint = 9;
        result.modifiers = terminal_modifier(TerminalKeyModifier::Shift);
        return result;
    }

    const auto key = csi_final_key(final);
    if (!key) {
        return std::nullopt;
    }
    TerminalKeyModifiers modifiers = 0;
    if (!body.empty()) {
        const auto fields = split_preserving_empty(body, ';');
        if (fields.empty() || fields.size() > 2U || fields[0] != "1") {
            return std::nullopt;
        }
        if (fields.size() == 2U) {
            if (fields[1].empty()) {
                return std::nullopt;
            }
            const auto decoded = decode_modifiers(fields[1]);
            if (!decoded) {
                return std::nullopt;
            }
            modifiers = *decoded;
        }
    }
    TerminalKeyEvent result;
    result.key = *key;
    result.modifiers = modifiers;
    return result;
}

std::optional<TerminalKeyEvent> decode_ss3(std::string_view input) {
    if (input.size() != 3U || input.substr(0, 2) != kSs3) {
        return std::nullopt;
    }
    const auto key = input[2] == 'R'
                         ? std::optional<TerminalKey>{TerminalKey::F3}
                         : csi_final_key(input[2]);
    if (!key) {
        return std::nullopt;
    }
    TerminalKeyEvent result;
    result.key = *key;
    return result;
}

std::optional<std::uint32_t> decode_one_utf8(std::string_view input) {
    if (input.empty()) {
        return std::nullopt;
    }
    const auto byte = [](char value) {
        return static_cast<unsigned char>(value);
    };
    const unsigned char first = byte(input[0]);
    std::uint32_t result = 0;
    std::size_t length = 0;
    if (first <= 0x7FU) {
        result = first;
        length = 1;
    } else if (0xC2U <= first && first <= 0xDFU) {
        result = first & 0x1FU;
        length = 2;
    } else if (0xE0U <= first && first <= 0xEFU) {
        result = first & 0x0FU;
        length = 3;
    } else if (0xF0U <= first && first <= 0xF4U) {
        result = first & 0x07U;
        length = 4;
    } else {
        return std::nullopt;
    }
    if (input.size() != length) {
        return std::nullopt;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char continuation = byte(input[i]);
        if ((continuation & 0xC0U) != 0x80U) {
            return std::nullopt;
        }
        result = (result << 6U) | (continuation & 0x3FU);
    }
    static constexpr std::array<std::uint32_t, 5> minimum = {
        0, 0, 0x80U, 0x800U, 0x10000U};
    if (result < minimum[length] || !is_unicode_scalar(result)) {
        return std::nullopt;
    }
    return result;
}

std::optional<TerminalKeyEvent> decode_legacy_single(std::string_view input,
                                                     bool alt) {
    if (input.size() == 1U) {
        const unsigned char value = static_cast<unsigned char>(input[0]);
        const TerminalKeyModifiers alt_modifier =
            alt ? terminal_modifier(TerminalKeyModifier::Alt) : 0U;
        if (value == 0x1BU && !alt) {
            return make_codepoint_event(27, 0, false);
        }
        if (value == 0x09U) {
            return make_codepoint_event(
                9, alt_modifier, false);
        }
        if (value == 0x0AU || value == 0x0DU) {
            return make_codepoint_event(
                13, alt_modifier, false);
        }
        if (value == 0x08U || value == 0x7FU) {
            return make_codepoint_event(
                127, alt_modifier, false);
        }
        if (1U <= value && value <= 26U) {
            return make_codepoint_event(
                static_cast<std::uint32_t>('a' + value - 1U),
                alt_modifier | TerminalKeyModifier::Ctrl,
                false);
        }
    }

    const auto codepoint = decode_one_utf8(input);
    if (!codepoint) {
        return std::nullopt;
    }
    TerminalKeyModifiers modifiers =
        alt ? terminal_modifier(TerminalKeyModifier::Alt) : 0U;
    std::uint32_t normalized = *codepoint;
    if ('A' <= normalized && normalized <= 'Z') {
        normalized += static_cast<std::uint32_t>('a' - 'A');
        modifiers |= terminal_modifier(TerminalKeyModifier::Shift);
    }
    return make_codepoint_event(normalized, modifiers, false);
}

std::uint32_t ascii_fold(std::uint32_t value) {
    if ('A' <= value && value <= 'Z') {
        return value + static_cast<std::uint32_t>('a' - 'A');
    }
    return value;
}

bool modifiers_match(const TerminalKeyEvent& event,
                     TerminalKeyModifiers expected,
                     TerminalKeyModifiers ignored) {
    return (event.modifiers & ~ignored) == (expected & ~ignored);
}

bool is_shortcut_action(const TerminalKeyEvent& event) {
    return event.action == TerminalKeyAction::Press ||
           event.action == TerminalKeyAction::Repeat;
}

} // namespace

std::optional<TerminalKeyEvent> decode_terminal_key(std::string_view input) {
    if (input.empty() || input.size() > kMaxSequenceBytes) {
        return std::nullopt;
    }
    if (input.substr(0, kCsi.size()) == kCsi) {
        if (input.size() <= kCsi.size()) {
            return std::nullopt;
        }
        if (input.back() == 'u') {
            return decode_kitty_csi_u(
                input.substr(kCsi.size(), input.size() - kCsi.size() - 1U));
        }
        return decode_legacy_csi(input);
    }
    if (input.substr(0, kSs3.size()) == kSs3) {
        return decode_ss3(input);
    }
    if (input.front() == '\x1B' && input.size() > 1U) {
        return decode_legacy_single(input.substr(1), true);
    }
    return decode_legacy_single(input, false);
}

std::optional<TerminalKeyEvent> decode_terminal_key(const ftxui::Event& event) {
    return decode_terminal_key(event.input());
}

bool matches_terminal_key(const ftxui::Event& event,
                          TerminalKey key,
                          TerminalKeyModifiers modifiers,
                          TerminalKeyModifiers ignored_modifiers) {
    const auto decoded = decode_terminal_key(event);
    return decoded && is_shortcut_action(*decoded) && decoded->key == key &&
           modifiers_match(*decoded, modifiers, ignored_modifiers);
}

bool matches_terminal_codepoint(const ftxui::Event& event,
                                std::uint32_t codepoint,
                                TerminalKeyModifiers modifiers,
                                TerminalKeyModifiers ignored_modifiers) {
    const auto decoded = decode_terminal_key(event);
    if (!decoded || !is_shortcut_action(*decoded) ||
        decoded->key != TerminalKey::Codepoint ||
        !modifiers_match(*decoded, modifiers, ignored_modifiers)) {
        return false;
    }
    const std::uint32_t expected = ascii_fold(codepoint);
    if (ascii_fold(decoded->codepoint) == expected) {
        return true;
    }
    if (decoded->base_layout_codepoint &&
        ascii_fold(*decoded->base_layout_codepoint) == expected) {
        return true;
    }
    return decoded->shifted_codepoint &&
           ascii_fold(*decoded->shifted_codepoint) == expected;
}

}} // namespace acecode::tui
