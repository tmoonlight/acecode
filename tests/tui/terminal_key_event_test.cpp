#include <gtest/gtest.h>

#include "tui/terminal_key_event.hpp"

#include <ftxui/component/event.hpp>

#include <string>

namespace {

using acecode::tui::TerminalKey;
using acecode::tui::TerminalKeyAction;
using acecode::tui::TerminalKeyModifier;
using acecode::tui::decode_terminal_key;
using acecode::tui::matches_terminal_codepoint;
using acecode::tui::matches_terminal_key;
using acecode::tui::terminal_modifier;

constexpr auto kShift = terminal_modifier(TerminalKeyModifier::Shift);
constexpr auto kAlt = terminal_modifier(TerminalKeyModifier::Alt);
constexpr auto kCtrl = terminal_modifier(TerminalKeyModifier::Ctrl);

ftxui::Event special(const std::string& input) {
    return ftxui::Event::Special(input);
}

} // namespace

TEST(TerminalKeyEvent, DecodesKittyControlLetterWithoutC0Collision) {
    const auto decoded = decode_terminal_key("\x1B[97;5u");
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->key, TerminalKey::Codepoint);
    EXPECT_EQ(decoded->codepoint, static_cast<std::uint32_t>('a'));
    EXPECT_EQ(decoded->modifiers, kCtrl);
    EXPECT_EQ(decoded->action, TerminalKeyAction::Press);
    EXPECT_TRUE(decoded->kitty);

    EXPECT_TRUE(matches_terminal_codepoint(special("\x1B[97;5u"), 'a', kCtrl));
    EXPECT_FALSE(matches_terminal_key(special("\x1B[97;5u"),
                                      TerminalKey::Tab));
}

TEST(TerminalKeyEvent, DistinguishesKittyCtrlShiftIFromTab) {
    const auto decoded = decode_terminal_key("\x1B[105;6u");
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->key, TerminalKey::Codepoint);
    EXPECT_EQ(decoded->codepoint, static_cast<std::uint32_t>('i'));
    EXPECT_EQ(decoded->modifiers, kCtrl | TerminalKeyModifier::Shift);
    EXPECT_TRUE(matches_terminal_codepoint(
        special("\x1B[105;6u"), 'i', kCtrl | TerminalKeyModifier::Shift));
    EXPECT_FALSE(matches_terminal_codepoint(
        special("\x1B[105;6u"), 'i', kCtrl));
    EXPECT_FALSE(matches_terminal_key(special("\x1B[105;6u"),
                                      TerminalKey::Tab));
}

TEST(TerminalKeyEvent, DecodesKittyEscapeAsSemanticKey) {
    const auto decoded = decode_terminal_key("\x1B[27;1u");
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->key, TerminalKey::Escape);
    EXPECT_EQ(decoded->codepoint, 27U);
    EXPECT_TRUE(decoded->kitty);
    EXPECT_TRUE(matches_terminal_key(special("\x1B[27;1u"),
                                     TerminalKey::Escape));
}

TEST(TerminalKeyEvent, PreservesKittyAlternateCodesActionAndText) {
    const auto decoded =
        decode_terminal_key("\x1B[97:65:99;6:2;65:769u");
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->codepoint, static_cast<std::uint32_t>('a'));
    ASSERT_TRUE(decoded->shifted_codepoint);
    EXPECT_EQ(*decoded->shifted_codepoint, static_cast<std::uint32_t>('A'));
    ASSERT_TRUE(decoded->base_layout_codepoint);
    EXPECT_EQ(*decoded->base_layout_codepoint,
              static_cast<std::uint32_t>('c'));
    EXPECT_EQ(decoded->modifiers, kCtrl | TerminalKeyModifier::Shift);
    EXPECT_EQ(decoded->action, TerminalKeyAction::Repeat);
    EXPECT_EQ(decoded->text_codepoints,
              (std::vector<std::uint32_t>{65U, 769U}));
}

TEST(TerminalKeyEvent, AcceptsAssociatedTextWithoutAnAssociatedKey) {
    const auto decoded = decode_terminal_key("\x1B[0;;229u");
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->codepoint, 0U);
    EXPECT_EQ(decoded->text_codepoints,
              (std::vector<std::uint32_t>{229U}));
}

TEST(TerminalKeyEvent, AcceptsEmptyShiftedAlternateBeforeBaseLayout) {
    const auto decoded = decode_terminal_key("\x1B[1092::99;5u");
    ASSERT_TRUE(decoded);
    EXPECT_FALSE(decoded->shifted_codepoint);
    ASSERT_TRUE(decoded->base_layout_codepoint);
    EXPECT_EQ(*decoded->base_layout_codepoint,
              static_cast<std::uint32_t>('c'));
    EXPECT_TRUE(matches_terminal_codepoint(
        special("\x1B[1092::99;5u"), 'c', kCtrl));
}

TEST(TerminalKeyEvent, ReleaseDoesNotTriggerShortcut) {
    const auto decoded = decode_terminal_key("\x1B[97;5:3u");
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->action, TerminalKeyAction::Release);
    EXPECT_FALSE(matches_terminal_codepoint(
        special("\x1B[97;5:3u"), 'a', kCtrl));
}

TEST(TerminalKeyEvent, LockModifiersAreIgnoredByDefault) {
    const auto decoded = decode_terminal_key("\x1B[97;69u");
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->modifiers,
              kCtrl | TerminalKeyModifier::CapsLock);
    EXPECT_TRUE(matches_terminal_codepoint(
        special("\x1B[97;69u"), 'a', kCtrl));
}

TEST(TerminalKeyEvent, DecodesLegacyModifiedArrows) {
    const auto shifted_left = decode_terminal_key("\x1B[1;2D");
    ASSERT_TRUE(shifted_left);
    EXPECT_EQ(shifted_left->key, TerminalKey::ArrowLeft);
    EXPECT_EQ(shifted_left->modifiers, kShift);

    const auto alt_up = decode_terminal_key("\x1B[1;3A");
    ASSERT_TRUE(alt_up);
    EXPECT_EQ(alt_up->key, TerminalKey::ArrowUp);
    EXPECT_EQ(alt_up->modifiers, kAlt);
    EXPECT_FALSE(alt_up->kitty);
}

TEST(TerminalKeyEvent, DecodesLegacyNavigationAndFunctionKeys) {
    const auto ctrl_delete = decode_terminal_key("\x1B[3;5~");
    ASSERT_TRUE(ctrl_delete);
    EXPECT_EQ(ctrl_delete->key, TerminalKey::Delete);
    EXPECT_EQ(ctrl_delete->modifiers, kCtrl);

    const auto f4 = decode_terminal_key("\x1BOS");
    ASSERT_TRUE(f4);
    EXPECT_EQ(f4->key, TerminalKey::F4);
    EXPECT_EQ(f4->modifiers, 0U);

    const auto f3 = decode_terminal_key("\x1BOR");
    ASSERT_TRUE(f3);
    EXPECT_EQ(f3->key, TerminalKey::F3);

    const auto shift_tab = decode_terminal_key("\x1B[Z");
    ASSERT_TRUE(shift_tab);
    EXPECT_EQ(shift_tab->key, TerminalKey::Tab);
    EXPECT_EQ(shift_tab->modifiers, kShift);
}

TEST(TerminalKeyEvent, KeepsLegacyTabDistinctFromControlI) {
    const auto tab = decode_terminal_key(std::string_view("\x09", 1));
    ASSERT_TRUE(tab);
    EXPECT_EQ(tab->key, TerminalKey::Tab);
    EXPECT_EQ(tab->modifiers, 0U);

    const auto ctrl_a = decode_terminal_key(std::string_view("\x01", 1));
    ASSERT_TRUE(ctrl_a);
    EXPECT_EQ(ctrl_a->key, TerminalKey::Codepoint);
    EXPECT_EQ(ctrl_a->codepoint, static_cast<std::uint32_t>('a'));
    EXPECT_EQ(ctrl_a->modifiers, kCtrl);
}

TEST(TerminalKeyEvent, DecodesLegacyEscapePrefixAndUtf8Character) {
    const auto alt_shift_v = decode_terminal_key("\x1BV");
    ASSERT_TRUE(alt_shift_v);
    EXPECT_EQ(alt_shift_v->key, TerminalKey::Codepoint);
    EXPECT_EQ(alt_shift_v->codepoint, static_cast<std::uint32_t>('v'));
    EXPECT_EQ(alt_shift_v->modifiers,
              kAlt | TerminalKeyModifier::Shift);

    const auto chinese = decode_terminal_key("\xE4\xB8\xAD");
    ASSERT_TRUE(chinese);
    EXPECT_EQ(chinese->key, TerminalKey::Codepoint);
    EXPECT_EQ(chinese->codepoint, 0x4E2DU);
}

TEST(TerminalKeyEvent, DecodesFtxuiEventOverload) {
    const auto decoded = decode_terminal_key(ftxui::Event::CtrlA);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->key, TerminalKey::Codepoint);
    EXPECT_EQ(decoded->codepoint, static_cast<std::uint32_t>('a'));
    EXPECT_EQ(decoded->modifiers, kCtrl);
}

TEST(TerminalKeyEvent, RejectsProtocolControlAndUnrelatedCsiSequences) {
    EXPECT_FALSE(decode_terminal_key("\x1B[?1u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[>1u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[<u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[<0;1;1M"));
    EXPECT_FALSE(decode_terminal_key("\x1B[200~"));
    EXPECT_FALSE(decode_terminal_key("\x1B[12;40R"));
    EXPECT_FALSE(decode_terminal_key("\x1B[1;2R"));
}

TEST(TerminalKeyEvent, RejectsMalformedOrIncompleteKittySequences) {
    EXPECT_FALSE(decode_terminal_key(""));
    EXPECT_FALSE(decode_terminal_key("\x1B["));
    EXPECT_FALSE(decode_terminal_key("\x1B[97"));
    EXPECT_FALSE(decode_terminal_key("\x1B[;5u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[97;0u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[97;257u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[97;5:4u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[97;:2u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[97:65;5u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[0;1u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[55296;1u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[42949672960;1u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[97;5;u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[97;5;65:u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[97;5;31u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[97;5;127u"));
    EXPECT_FALSE(decode_terminal_key("\x1B[3;~"));
    EXPECT_FALSE(decode_terminal_key("\x1B[1;A"));
}

TEST(TerminalKeyEvent, RejectsOversizedSequence) {
    std::string oversized = "\x1B[" + std::string(510, '1') + "u";
    ASSERT_GT(oversized.size(), 512U);
    EXPECT_FALSE(decode_terminal_key(oversized));
}
