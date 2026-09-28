#include "utils/abandonable_call.hpp"
#include "utils/scope_exit.hpp"

#include <cstdio>
#include <thread>

namespace acecode {
namespace {

struct WorkRegistry {
    std::mutex mu;
    std::condition_variable cv;
    std::size_t active = 0;
};

std::shared_ptr<WorkRegistry> work_registry() {
    // Shared by this process access point and each worker. The workers use only
    // their captured lease, so completing after static teardown is safe.
    static const auto registry = std::make_shared<WorkRegistry>();
    return registry;
}

void complete_work(const std::shared_ptr<WorkRegistry>& registry) {
    {
        std::lock_guard<std::mutex> lock(registry->mu);
        --registry->active;
    }
    registry->cv.notify_all();
}

}  // namespace

void abandonable_detail::start_owned_work(std::string name, std::function<void()> fn) {
    auto registry = work_registry();
    {
        std::lock_guard<std::mutex> lock(registry->mu);
        ++registry->active;
    }
    ScopeExit rollback([registry] { complete_work(registry); });
    // Deliberate primitive-level exception to C8: detached blocking work owns
    // its entire closure and is counted until that closure has been destroyed.
    // stderr diagnostics remain usable after the application Logger's teardown.
    std::fprintf(stderr, "[abandonable_call] starting owned detached work: %s\n", name.c_str());
    std::thread worker([registry, name = std::move(name), fn = std::move(fn)]() mutable {
        try {
            fn();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[abandonable_call] %s failed: %s\n", name.c_str(), error.what());
        } catch (...) {
            std::fprintf(stderr, "[abandonable_call] %s failed with an unknown exception\n", name.c_str());
        }
        fn = {};
        complete_work(registry);
    });
    worker.detach();
    rollback.release();
}

bool wait_for_abandoned_work(std::chrono::steady_clock::time_point deadline) {
    auto registry = work_registry();
    std::unique_lock<std::mutex> lock(registry->mu);
    const bool finished = registry->cv.wait_until(lock, deadline, [registry] {
        return registry->active == 0;
    });
    const auto pending = registry->active;
    lock.unlock();
    if (!finished) {
        std::fprintf(stderr, "[abandonable_call] wait deadline reached with %zu worker(s) pending\n", pending);
    }
    return finished;
}

}  // namespace acecode
