// 覆盖 src/base/config/swarm_config.cpp(add-mesh-swarm-mode):蜂群模式（网状）的
// 调优项,默认值对齐 Codex MultiAgentV2Config —— 并发上限 4(含根 agent)、
// agent_wait 最短 10s / 默认 30s / 最长 1h、默认向模型展示可选模型。
// 越界值静默夹取(只是调优值,不值得阻塞启动);写回时只写非默认项(稀疏写)。

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <nlohmann/json.hpp>

using namespace acecode;

// 场景:config.json 没有 swarm 段的机器。
// 期望:结构体默认值与 Codex 一致,序列化结果为空对象(不往配置里写默认值)。
TEST(ConfigSwarm, DefaultsMatchCodexAndSerializeSparse) {
    SwarmConfig cfg;
    EXPECT_EQ(cfg.mesh.max_concurrent_agents, 4);
    EXPECT_EQ(cfg.mesh.min_wait_timeout_ms, 10000);
    EXPECT_EQ(cfg.mesh.default_wait_timeout_ms, 30000);
    EXPECT_EQ(cfg.mesh.max_wait_timeout_ms, 3600000);
    EXPECT_TRUE(cfg.mesh.expose_model_overrides);
    EXPECT_TRUE(swarm_config_to_json(cfg).empty());
    AppConfig app;
    EXPECT_EQ(app.swarm.mesh.max_concurrent_agents, 4);
}

// 场景:用户调大并发、关掉模型覆盖展示。
// 期望:读入后原样生效;写回只包含改动的两项,往返一致。
TEST(ConfigSwarm, LoadsOverridesAndRoundTrips) {
    SwarmConfig cfg;
    load_swarm_config_json(
        {{"mesh", {{"max_concurrent_agents", 8}, {"expose_model_overrides", false}}}}, cfg);
    EXPECT_EQ(cfg.mesh.max_concurrent_agents, 8);
    EXPECT_FALSE(cfg.mesh.expose_model_overrides);
    const auto json = swarm_config_to_json(cfg);
    EXPECT_EQ(json, (nlohmann::json{{"mesh",
        {{"max_concurrent_agents", 8}, {"expose_model_overrides", false}}}}));
    SwarmConfig reloaded;
    load_swarm_config_json(json, reloaded);
    EXPECT_EQ(reloaded.mesh.max_concurrent_agents, 8);
    EXPECT_FALSE(reloaded.mesh.expose_model_overrides);
}

// 场景:越界或类型错误的调优值。
// 期望:并发夹到 [2, 64](1 会让根 agent 没有任何子 agent 名额);等待区间保持
// min <= default <= max;非整数字段忽略,沿用默认值,不抛异常。
TEST(ConfigSwarm, ClampsOutOfRangeValues) {
    SwarmConfig low;
    load_swarm_config_json({{"mesh", {{"max_concurrent_agents", 1},
                                      {"min_wait_timeout_ms", 5},
                                      {"default_wait_timeout_ms", 1},
                                      {"max_wait_timeout_ms", 2}}}},
                           low);
    EXPECT_EQ(low.mesh.max_concurrent_agents, 2);
    EXPECT_EQ(low.mesh.min_wait_timeout_ms, 1000);
    EXPECT_EQ(low.mesh.max_wait_timeout_ms, 1000);
    EXPECT_EQ(low.mesh.default_wait_timeout_ms, 1000);

    SwarmConfig high;
    load_swarm_config_json({{"mesh", {{"max_concurrent_agents", 1000},
                                      {"default_wait_timeout_ms", 99999999}}}},
                           high);
    EXPECT_EQ(high.mesh.max_concurrent_agents, 64);
    EXPECT_EQ(high.mesh.default_wait_timeout_ms, high.mesh.max_wait_timeout_ms);

    SwarmConfig wrong_type;
    load_swarm_config_json({{"mesh", {{"max_concurrent_agents", "8"},
                                      {"expose_model_overrides", 1}}}},
                           wrong_type);
    EXPECT_EQ(wrong_type.mesh.max_concurrent_agents, 4);
    EXPECT_TRUE(wrong_type.mesh.expose_model_overrides);
    load_swarm_config_json(nlohmann::json::array(), wrong_type);
    EXPECT_EQ(wrong_type.mesh.max_concurrent_agents, 4);
}
