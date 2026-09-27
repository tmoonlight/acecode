#include "computer_use/pointer_appearance.hpp"
#include "computer_use/runtime.hpp"

#include <gtest/gtest.h>
#include <stdexcept>

namespace acecode::computer_use {

TEST(ComputerUsePointerAppearance, StylesAreExplicitAndLegacyDefaultsAreStable) {
    EXPECT_STREQ(pointer_appearance::kDefaultStyle, "ace");
    EXPECT_STREQ(pointer_appearance::kDefaultColor, "#2563eb");
    EXPECT_TRUE(pointer_appearance::valid_style("ace"));
    EXPECT_TRUE(pointer_appearance::valid_style("plain"));
    for (const auto* value : {"", "ACE", "theme", "plain ", "system"})
        EXPECT_FALSE(pointer_appearance::valid_style(value));
}

TEST(ComputerUsePointerAppearance, ColorsRequireExactlySixAsciiHexDigits) {
    EXPECT_EQ(pointer_appearance::normalize_color("#AaBbCc"), std::optional<std::string>("#aabbcc"));
    EXPECT_EQ(pointer_appearance::normalize_color("#000000"), std::optional<std::string>("#000000"));
    EXPECT_EQ(pointer_appearance::normalize_color("#FFFFFF"), std::optional<std::string>("#ffffff"));
    for (const auto* value : {"", "abcdef", "#abc", "#12345678", "#aabbcg", " #aabbcc", "#aabbcc ", "#aabbcc\n", "red", "rgb(0,0,0)"})
        EXPECT_FALSE(pointer_appearance::normalize_color(value));
}

TEST(ComputerUsePointerAppearance, RuntimeAppearanceDoesNotEnableDesktopControl) {
    set_enabled(false);
    set_pointer_appearance("plain", "#ABCDEF");
    EXPECT_FALSE(enabled());
    EXPECT_THROW(set_pointer_appearance("unknown", "#123456"), std::invalid_argument);
    EXPECT_THROW(set_pointer_appearance("ace", "#123"), std::invalid_argument);
    EXPECT_FALSE(enabled());
    set_pointer_appearance(pointer_appearance::kDefaultStyle, pointer_appearance::kDefaultColor);
}

} // namespace acecode::computer_use
