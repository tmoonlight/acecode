#include "pa_adapter.hpp"

#include <atomic>

namespace acecode::pa {
namespace {
std::atomic<bool> g_enabled{true};
} // namespace

bool enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void set_enabled(bool value) {
    g_enabled.store(value, std::memory_order_relaxed);
}

} // namespace acecode::pa
