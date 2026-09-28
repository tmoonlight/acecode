#include <gtest/gtest.h>

#include "computer_use/session_lease.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

TEST(ComputerUseSessionLease, MoveTransfersScopeExitReleaseAndOwnerUpdate) {
    auto releases = std::make_shared<std::vector<std::string>>();
    {
        acecode::computer_use::SessionLease first("", [releases](const auto& id) {
            releases->push_back(id);
        });
        first.set_owner("persisted-session");
        auto second = std::move(first);
        second.release_before_terminal();
        EXPECT_EQ(*releases, (std::vector<std::string>{"persisted-session"}));
    }
    EXPECT_EQ(*releases, (std::vector<std::string>{
        "persisted-session", "persisted-session"}));
}

TEST(ComputerUseSessionLease, ExceptionStillReleasesCapturedOwner) {
    auto releases = std::make_shared<std::vector<std::string>>();
    EXPECT_THROW({
        acecode::computer_use::SessionLease lease("session", [releases](const auto& id) {
            releases->push_back(id);
        });
        throw std::runtime_error("turn failed");
    }, std::runtime_error);
    EXPECT_EQ(*releases, (std::vector<std::string>{"session"}));
}
