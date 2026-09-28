#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace acecode::computer_use::macos {
struct KeyChord {
    std::uint16_t key = 0;
    std::uint64_t flags = 0;
    std::vector<std::uint16_t> modifiers;
};
// Throws for unknown, duplicate or incomplete chords; never returns partial input.
KeyChord parse_key_chord(const std::string& text);
}
