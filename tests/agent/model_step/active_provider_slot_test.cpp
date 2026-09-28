#include <gtest/gtest.h>

#include "agent/model_step/active_provider_slot.hpp"
#include "test_support/agent/stub_provider.hpp"

#include <memory>
#include <stdexcept>

TEST(ActiveProviderScope, OwnsLeaseThroughExceptionAndReleasesOnUnwind) {
    acecode::agent::ActiveProviderSlot slot;
    std::weak_ptr<acecode::LlmProvider> observed;
    EXPECT_THROW({
        auto provider = std::make_shared<acecode_test::StubLlmProvider>();
        observed = provider;
        acecode::agent::ActiveProviderScope active(slot, provider);
        provider.reset();
        EXPECT_FALSE(observed.expired());
        slot.wake();
        throw std::runtime_error("provider failed");
    }, std::runtime_error);
    EXPECT_TRUE(observed.expired());
    slot.wake();
}
