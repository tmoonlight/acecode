#include <gtest/gtest.h>

#include "session_host/swarm_command.hpp"

// /swarm [star|mesh|off] 的文本实现(add-mesh-swarm-mode):TUI 命令与 daemon
// 内置命令共用,保证两端提示文案一致。

using namespace acecode;

// 场景:不带参数 / 带空白。
// 期望:视为查看当前模式,状态文本带用法提示。
TEST(SwarmCommand, EmptyArgsShowCurrentMode) {
    const auto request = parse_swarm_command("   ");
    EXPECT_TRUE(request.show);
    EXPECT_FALSE(request.mode.has_value());
    EXPECT_TRUE(request.error.empty());
    EXPECT_EQ(swarm_command_status_text(SwarmMode::Mesh),
              "Swarm mode: mesh (agent_* collaboration tree)\nUsage: /swarm [star|mesh|off]");
}

// 场景:三个规范名,大小写与首尾空白不敏感。
// 期望:解析成对应模式;切换成功文案点明工具族。
TEST(SwarmCommand, ParsesCanonicalModes) {
    EXPECT_EQ(parse_swarm_command("star").mode, SwarmMode::Star);
    EXPECT_EQ(parse_swarm_command(" MESH ").mode, SwarmMode::Mesh);
    EXPECT_EQ(parse_swarm_command("off").mode, SwarmMode::Off);
    EXPECT_EQ(swarm_command_applied_text(SwarmMode::Star),
              "Swarm mode set to star (spawn_subagent fan-out).");
    EXPECT_EQ(swarm_command_applied_text(SwarmMode::Off), "Swarm mode set to off.");
}

// 场景:旧消息接口的布尔字面量与拼错的模式名。
// 期望:命令只认三个规范名,其余都报错且不带模式 —— "/swarm true" 这种写法
// 不会被静默当成星型(布尔兼容只属于 Web 消息接口)。
TEST(SwarmCommand, RejectsUnknownWordsIncludingLegacyBooleans) {
    for (const char* bad : {"true", "false", "grid", "mesh please"}) {
        const auto request = parse_swarm_command(bad);
        EXPECT_FALSE(request.mode.has_value()) << bad;
        EXPECT_FALSE(request.show) << bad;
        EXPECT_NE(request.error.find("(allowed: star, mesh, off)"), std::string::npos) << bad;
    }
}
