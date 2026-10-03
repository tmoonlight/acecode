// 斜杠命令别名:注册表的别名映射 + TUI 下拉菜单的 "/原名 (别名)" 显示。
//
// 背景:旧实现把别名当成独立命令注册(/new、/side、/rc 各一条),下拉菜单里
// 同一个命令出现两三行、描述还是 "Alias for /xxx"。现在别名挂在原名上,下拉
// 只出原名一行;只有用户敲的正是某个别名时,才在括号里标出那个别名。

#include <gtest/gtest.h>

#include "tui/commands/command_registry.hpp"
#include "tui/slash_dropdown.hpp"
#include "tui/tui_state.hpp"

#include <string>
#include <vector>

namespace {

acecode::SlashCommand make_command(const std::string& name,
                                   const std::string& description,
                                   std::vector<std::string> aliases = {}) {
    acecode::SlashCommand cmd;
    cmd.name = name;
    cmd.description = description;
    cmd.execute = [](acecode::CommandContext&, const std::string&) {};
    cmd.aliases = std::move(aliases);
    return cmd;
}

// 下拉需要的最小注册表:一个三名命令(generate / new / create)、
// 一个短别名命令(remote-control / rc)、几个普通命令。
acecode::CommandRegistry make_registry() {
    acecode::CommandRegistry registry;
    registry.register_command(
        make_command("generate", "Generate a scaffold", {"new", "create"}));
    registry.register_command(
        make_command("remote-control", "Manage remote control", {"rc"}));
    registry.register_command(make_command("resume", "Resume a session"));
    registry.register_command(make_command("help", "Show help"));
    return registry;
}

const acecode::TuiState::SlashDropdownItem* find_item(
    const acecode::TuiState& state, const std::string& name) {
    for (const auto& item : state.slash_dropdown_items) {
        if (item.name == name) return &item;
    }
    return nullptr;
}

int count_items(const acecode::TuiState& state, const std::string& name) {
    int n = 0;
    for (const auto& item : state.slash_dropdown_items) {
        if (item.name == name) ++n;
    }
    return n;
}

} // namespace

// 场景:注册一个带两个别名的命令。
// 期望:原名与别名都能 has_command / find,别名解析回原名;commands() 只有原名一条。
TEST(CommandAliasRegistry, AliasesResolveToCanonicalWithoutSeparateEntries) {
    auto registry = make_registry();

    EXPECT_TRUE(registry.has_command("generate"));
    EXPECT_TRUE(registry.has_command("new"));
    EXPECT_TRUE(registry.has_command("create"));
    EXPECT_EQ(registry.resolve_name("new"), "generate");
    EXPECT_EQ(registry.resolve_name("create"), "generate");
    EXPECT_EQ(registry.resolve_name("generate"), "generate");
    EXPECT_EQ(registry.resolve_name("missing"), "");
    ASSERT_NE(registry.find("create"), nullptr);
    EXPECT_EQ(registry.find("create")->name, "generate");
    EXPECT_EQ(registry.commands().count("new"), 0u);
    EXPECT_EQ(registry.commands().count("create"), 0u);
}

// 场景:按原名注销命令(skill / opencode 命令重载走这条路)。
// 期望:别名随原名一起消失,不残留指向已删除命令的别名。
TEST(CommandAliasRegistry, UnregisterRemovesAliases) {
    auto registry = make_registry();

    EXPECT_FALSE(registry.unregister_command("rc")) << "只接受原名";
    ASSERT_TRUE(registry.unregister_command("remote-control"));

    EXPECT_FALSE(registry.has_command("remote-control"));
    EXPECT_FALSE(registry.has_command("rc"));
}

// 场景:同名命令重复注册,新版本换了别名。
// 期望:旧别名失效,新别名生效 —— 不会出现旧别名指向新命令的残留。
TEST(CommandAliasRegistry, ReRegisterReplacesOldAliases) {
    acecode::CommandRegistry registry;
    registry.register_command(make_command("target", "v1", {"old"}));
    registry.register_command(make_command("target", "v2", {"fresh"}));

    EXPECT_FALSE(registry.has_command("old"));
    EXPECT_EQ(registry.resolve_name("fresh"), "target");
    EXPECT_EQ(registry.commands().at("target").description, "v2");
}

// 场景:别名与已注册的命令名或别名撞名。
// 期望:撞名的别名被跳过,已有命令不被劫持;同时 has_command 对别名返回 true,
// 使 skill / opencode 命令注册时的撞名检查继续挡住 "rc"、"new" 这类名字。
TEST(CommandAliasRegistry, CollidingAliasIsSkipped) {
    acecode::CommandRegistry registry;
    registry.register_command(make_command("help", "Show help"));
    registry.register_command(make_command("first", "First", {"f"}));
    registry.register_command(make_command("second", "Second", {"help", "f", "s"}));

    EXPECT_EQ(registry.resolve_name("help"), "help");
    EXPECT_EQ(registry.resolve_name("f"), "first");
    EXPECT_EQ(registry.commands().at("second").aliases,
              std::vector<std::string>{"s"});
}

// 场景:用户只敲了 "/"。
// 期望:每个命令只出一行,且不带任何别名括号(没有敲别名,就不展示别名)。
TEST(CommandAliasDropdown, EmptyQueryListsEachCommandOnceWithoutAliases) {
    auto registry = make_registry();
    acecode::TuiState state;
    state.input_text = "/";

    acecode::refresh_slash_dropdown(state, registry);

    ASSERT_TRUE(state.slash_dropdown_active);
    EXPECT_EQ(state.slash_dropdown_items.size(), 4u);
    for (const auto& item : state.slash_dropdown_items) {
        EXPECT_TRUE(item.matched_alias.empty()) << item.name;
        EXPECT_NE(item.name, "new");
        EXPECT_NE(item.name, "create");
        EXPECT_NE(item.name, "rc");
    }
}

// 场景:三名命令 generate(别名 new、create),用户分别敲 "/new" 与 "/create"。
// 期望:下拉里都只有 generate 一行,括号里是用户敲的那个别名:
// "/new" → "/generate (new)","/create" → "/generate (create)",且该行被选中。
TEST(CommandAliasDropdown, TypedAliasIsShownNextToCanonicalName) {
    auto registry = make_registry();
    acecode::TuiState state;

    state.input_text = "/new";
    acecode::refresh_slash_dropdown(state, registry);
    ASSERT_TRUE(state.slash_dropdown_active);
    ASSERT_EQ(count_items(state, "generate"), 1);
    EXPECT_EQ(find_item(state, "generate")->matched_alias, "new");
    EXPECT_EQ(state.slash_dropdown_items[state.slash_dropdown_selected].name,
              "generate");

    state.input_text = "/create";
    acecode::refresh_slash_dropdown(state, registry);
    ASSERT_EQ(count_items(state, "generate"), 1);
    EXPECT_EQ(find_item(state, "generate")->matched_alias, "create");

    state.input_text = "/rc";
    acecode::refresh_slash_dropdown(state, registry);
    ASSERT_NE(find_item(state, "remote-control"), nullptr);
    EXPECT_EQ(find_item(state, "remote-control")->matched_alias, "rc");
}

// 场景:用户敲的前缀同时命中原名与别名("/re" 命中 remote-control 与 resume,
// 别名 rc 不以 re 开头;"/gen" 命中 generate 原名)。
// 期望:原名匹配得不比别名差时不标别名,只显示 "/remote-control"、"/generate"。
TEST(CommandAliasDropdown, CanonicalMatchDoesNotShowAlias) {
    auto registry = make_registry();
    acecode::TuiState state;

    state.input_text = "/re";
    acecode::refresh_slash_dropdown(state, registry);
    ASSERT_NE(find_item(state, "remote-control"), nullptr);
    EXPECT_TRUE(find_item(state, "remote-control")->matched_alias.empty());

    state.input_text = "/gen";
    acecode::refresh_slash_dropdown(state, registry);
    ASSERT_NE(find_item(state, "generate"), nullptr);
    EXPECT_TRUE(find_item(state, "generate")->matched_alias.empty());
}

// 场景:敲完整别名 "/rc" 之前,下拉高亮停在别的命令上("/r" 时选中了 resume)。
// 期望:完整敲出别名后,高亮跳到别名所属的 remote-control,而不是沿用旧高亮。
TEST(CommandAliasDropdown, ExactAliasWinsOverPreviousSelection) {
    auto registry = make_registry();
    acecode::TuiState state;
    state.input_text = "/r";
    acecode::refresh_slash_dropdown(state, registry);
    ASSERT_TRUE(state.slash_dropdown_active);
    for (int i = 0; i < static_cast<int>(state.slash_dropdown_items.size()); ++i) {
        if (state.slash_dropdown_items[i].name == "resume") {
            state.slash_dropdown_selected = i;
        }
    }

    state.input_text = "/rc";
    acecode::refresh_slash_dropdown(state, registry);

    EXPECT_EQ(state.slash_dropdown_items[state.slash_dropdown_selected].name,
              "remote-control");
}
