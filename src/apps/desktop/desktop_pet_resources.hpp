#pragma once

#include "utils/base64.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace acecode {
const unsigned char* desktop_pet_page_data();
std::size_t desktop_pet_page_size();
const unsigned char* classic_pet_page_data();
std::size_t classic_pet_page_size();
const unsigned char* clippy_pet_sprite_data();
std::size_t clippy_pet_sprite_size();
const unsigned char* rover_pet_sprite_data();
std::size_t rover_pet_sprite_size();
const unsigned char* whale_pet_sprite_data();
std::size_t whale_pet_sprite_size();
const unsigned char* whale_pet_peek_data();
std::size_t whale_pet_peek_size();
} // namespace acecode

namespace acecode::desktop {
inline std::string desktop_pet_html(std::string_view kind) {
    const bool office = kind == "office";
    const auto* data = office ? desktop_pet_page_data() : classic_pet_page_data();
    return {reinterpret_cast<const char*>(data), office ? desktop_pet_page_size() : classic_pet_page_size()};
}

inline nlohmann::json desktop_pet_character(std::string_view kind) {
    const bool rover = kind == "rover";
    const auto* data = kind == "whale" ? whale_pet_sprite_data() : rover ? rover_pet_sprite_data() : clippy_pet_sprite_data();
    const auto size = kind == "whale" ? whale_pet_sprite_size() : rover ? rover_pet_sprite_size() : clippy_pet_sprite_size();
    nlohmann::json character = {{"type", "pet-character"}, {"kind", kind},
            {"sprite", "data:image/png;base64," + base64_encode({reinterpret_cast<const char*>(data), size})}};
    // Edge-peek frames used while Whale Girl is docked to the left/right of the screen.
    if (kind == "whale") {
        character["peek"] = "data:image/png;base64," +
            base64_encode({reinterpret_cast<const char*>(whale_pet_peek_data()), whale_pet_peek_size()});
    }
    return character;
}
} // namespace acecode::desktop
