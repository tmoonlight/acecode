#include "connectors/connector_first_start_auth.hpp"
#include "utils/state_file.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

class ConnectorFirstStartAuthTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
            ("acecode_connector_first_start_" +
             std::to_string(std::rand()));
        fs::create_directories(root_);
        state_path_ = root_ / "state.json";
        acecode::set_state_file_path_for_test(state_path_.string());
    }

    void TearDown() override {
        acecode::set_state_file_path_for_test("");
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    static acecode::ConnectorConfig connector(
        std::string id,
        bool enabled,
        bool with_startup_hook) {
        acecode::ConnectorConfig value;
        value.id = std::move(id);
        value.name = value.id;
        value.enabled = enabled;
        if (with_startup_hook) {
            acecode::ConnectorHookConfig hook;
            hook.command = "login-helper";
            hook.args = {"--ensure"};
            value.on_startup = std::move(hook);
        }
        return value;
    }

    fs::path root_;
    fs::path state_path_;
};

TEST_F(ConnectorFirstStartAuthTest, FirstPlanClaimsBeforeSelectingEnabledHooks) {
    auto plan = acecode::plan_connector_first_start_auth({
        connector("enabled", true, true),
        connector("disabled", false, true),
        connector("no-hook", true, false),
    });

    EXPECT_TRUE(plan.claimed);
    EXPECT_TRUE(plan.persisted);
    ASSERT_EQ(plan.connectors.size(), 1u);
    EXPECT_EQ(plan.connectors[0].id, "enabled");
    EXPECT_TRUE(acecode::read_state_flag(
        acecode::kConnectorFirstStartAuthStateKey));
}

TEST_F(ConnectorFirstStartAuthTest, LaterPlanNeverReturnsHooks) {
    auto first = acecode::plan_connector_first_start_auth({
        connector("first", true, true),
    });
    ASSERT_TRUE(first.claimed);

    auto later = acecode::plan_connector_first_start_auth({
        connector("later", true, true),
    });

    EXPECT_FALSE(later.claimed);
    EXPECT_TRUE(later.persisted);
    EXPECT_TRUE(later.connectors.empty());
}

TEST_F(ConnectorFirstStartAuthTest, EmptyFirstStartupStillConsumesGate) {
    auto first = acecode::plan_connector_first_start_auth({});
    EXPECT_TRUE(first.claimed);
    EXPECT_TRUE(first.connectors.empty());

    auto later = acecode::plan_connector_first_start_auth({
        connector("installed-later", true, true),
    });
    EXPECT_FALSE(later.claimed);
    EXPECT_TRUE(later.connectors.empty());
}

TEST_F(ConnectorFirstStartAuthTest, PersistenceFailureReturnsNoHooks) {
    const fs::path directory_target = root_ / "state-directory";
    fs::create_directories(directory_target);
    acecode::set_state_file_path_for_test(directory_target.string());

    auto plan = acecode::plan_connector_first_start_auth({
        connector("enabled", true, true),
    });

    EXPECT_FALSE(plan.claimed);
    EXPECT_FALSE(plan.persisted);
    EXPECT_TRUE(plan.connectors.empty());
}

} // namespace
