// 场景:五组业务状态迁出 utils 后仍共享同一写锁,并发写入不能覆盖其它模块的字段。
// 保留原并发用例名称,扩展验证跨模块原子更新及数据目录迁移时的写入暂停。
#include <gtest/gtest.h>

#include "desktop/workspace_state.hpp"
#include "provider/model_probe_cache.hpp"
#include "tool/web_search/region_cache.hpp"
#include "tui/slash_command_usage.hpp"
#include "test_support/utils/state_file_fixture.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

using acecode::test_support::StateFileTest;

TEST_F(StateFileTest, ConcurrentCheckedWritesPreserveEveryKey) {
    constexpr int kWriterCount = 12;
    std::vector<std::thread> writers;
    writers.reserve(kWriterCount);
    for (int i = 0; i < kWriterCount; ++i) {
        writers.emplace_back([i]() {
            EXPECT_TRUE(acecode::try_write_state_flag(
                "concurrent_flag_" + std::to_string(i), true));
            acecode::write_last_active_workspace_hash("active-" + std::to_string(i));
            acecode::write_last_home_workspace_hash("home-" + std::to_string(i));
            acecode::write_web_search_region_cache({i % 2 ? "cn" : "global", i});
            const auto usage = acecode::record_tui_slash_command_use("help");
            EXPECT_TRUE(usage.persisted);
            acecode::ModelProbeCacheEntry probe;
            probe.models = {"model-" + std::to_string(i)};
            probe.probed_at_ms = i;
            EXPECT_TRUE(acecode::write_model_probe_cache(
                std::string(64, "0123456789abcdef"[i]), probe));
        });
    }
    for (auto& writer : writers) writer.join();

    for (int i = 0; i < kWriterCount; ++i) {
        EXPECT_TRUE(acecode::read_state_flag("concurrent_flag_" + std::to_string(i)));
        const auto probe = acecode::read_model_probe_cache(
            std::string(64, "0123456789abcdef"[i]));
        ASSERT_TRUE(probe.has_value());
        EXPECT_EQ(probe->models, std::vector<std::string>{"model-" + std::to_string(i)});
    }
    EXPECT_EQ(acecode::read_tui_slash_command_usage().at("help"), kWriterCount);
    EXPECT_FALSE(acecode::read_last_active_workspace_hash().empty());
    EXPECT_FALSE(acecode::read_last_home_workspace_hash().empty());
    EXPECT_TRUE(acecode::read_web_search_region_cache().has_value());

    // 场景:暂停写入后,各模块均应保留原内容;命令计数不能伪报已更新。
    const auto before = acecode::read_state_json();
    acecode::set_state_file_writes_paused(true);
    EXPECT_FALSE(acecode::try_write_state_flag("paused_flag", true));
    acecode::write_last_active_workspace_hash("paused-active");
    acecode::write_last_home_workspace_hash("paused-home");
    acecode::write_web_search_region_cache({"cn", 999});
    acecode::clear_web_search_region_cache();
    const auto paused_usage = acecode::record_tui_slash_command_use("help");
    EXPECT_FALSE(paused_usage.persisted);
    EXPECT_EQ(paused_usage.count, 0u);
    acecode::ModelProbeCacheEntry paused_probe;
    paused_probe.models = {"paused-model"};
    EXPECT_FALSE(acecode::write_model_probe_cache(std::string(64, 'a'), paused_probe));
    EXPECT_EQ(acecode::read_state_json(), before);
}
