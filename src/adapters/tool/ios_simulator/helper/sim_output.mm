#import "sim_output.h"

#include "tool/ios_simulator/wire.hpp"

#include <atomic>
#include <cerrno>
#include <mutex>
#include <unistd.h>

namespace acecode::ios_simulator::helper {
namespace {

std::mutex g_write_mu;
std::atomic<int> g_droppable_pending{0};

void write_all(const std::string& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto count = ::write(STDOUT_FILENO, bytes.data() + sent, bytes.size() - sent);
        if (count > 0) { sent += static_cast<std::size_t>(count); continue; }
        if (count < 0 && errno == EINTR) continue;
        // The daemon is gone; there is nobody left to serve.
        _exit(0);
    }
}

} // namespace

void emit(const nlohmann::json& header, const std::string& binary, bool drop_if_busy) {
    if (drop_if_busy && g_droppable_pending.load() > 2) return;
    std::string frame;
    try { frame = wire::encode(header, binary); } catch (const std::exception&) { return; }
    if (drop_if_busy) g_droppable_pending.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(g_write_mu);
        write_all(frame);
    }
    if (drop_if_busy) g_droppable_pending.fetch_sub(1);
}

void emit_log(const std::string& level, const std::string& message) {
    emit({{"event", "log"}, {"level", level}, {"message", message}});
}

} // namespace acecode::ios_simulator::helper
