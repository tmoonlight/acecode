#include "bridge.hpp"
#include "daemon/platform.hpp"
#include "utils/utf8_path.hpp"
#include "utils/atomic_file.hpp"
#include "lsp/lsp_which.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#endif

namespace acecode::channels {
struct Bridge::Impl {
    lsp::LspProcess process;
    mutable std::mutex mu;
    std::condition_variable cv;
    std::thread reader, writer;
    bool alive = false;
    std::string failure;
    std::uint64_t next_id = 1;
    struct Pending { bool done = false; Json result; std::string error; };
    std::map<std::uint64_t, std::shared_ptr<Pending>> pending;
    std::deque<std::string> writes;
    std::vector<Json> events;

    void fail(const std::string& reason) {
        std::lock_guard<std::mutex> lock(mu);
        alive = false; failure = reason;
        for (const auto& item : pending) { item.second->done = true; item.second->error = reason; }
        writes.clear(); cv.notify_all();
    }
    void read_loop() {
        std::string buffer;
        char bytes[8192];
        for (;;) {
            { std::lock_guard<std::mutex> lock(mu); if (!alive) break; }
            const auto count = process.read_stdout(bytes, sizeof(bytes));
            if (count <= 0) { fail("WhatsApp bridge exited. Run acecode channels to check Node.js and repair dependencies."); break; }
            buffer.append(bytes, static_cast<std::size_t>(count));
            for (;;) {
                auto newline = buffer.find('\n');
                if (newline == std::string::npos) break;
                if (newline > kMaxFrameBytes) { fail("Bridge frame exceeds limit"); return; }
                const auto line = buffer.substr(0, newline); buffer.erase(0, newline + 1);
                try {
                    const auto frame = Json::parse(line);
                    if (!frame.is_object()) throw std::runtime_error("Expected bridge object");
                    std::lock_guard<std::mutex> lock(mu);
                    if (frame.contains("id")) {
                        const auto id = frame.at("id").get<std::uint64_t>();
                        const auto it = pending.find(id);
                        if (it == pending.end()) continue;
                        it->second->done = true;
                        if (frame.value("ok", false)) it->second->result = frame.value("result", Json::object());
                        else it->second->error = frame.value("error", "Bridge request failed");
                        cv.notify_all();
                    } else if (frame.contains("event") && frame["event"].is_string()) {
                        if (events.size() >= 256) throw std::runtime_error("Bridge event queue full");
                        events.push_back(frame);
                    } else throw std::runtime_error("Unrecognized bridge frame");
                } catch (const std::exception&) { fail("Invalid or excessive WhatsApp bridge output"); return; }
            }
            if (buffer.size() > kMaxFrameBytes) { fail("Bridge frame exceeds limit"); break; }
        }
    }
    void write_loop() {
#ifndef _WIN32
        sigset_t blocked;
        sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
#endif
        for (;;) {
            std::string frame;
            {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait(lock, [&] { return !alive || !writes.empty(); });
                if (!alive) break;
                frame = std::move(writes.front()); writes.pop_front();
            }
            std::string error;
            if (!process.write_stdin(frame.data(), frame.size(), &error)) { fail("Bridge input pipe failed"); break; }
        }
    }
};
Bridge::Bridge() : impl_(std::make_unique<Impl>()) {}
Bridge::~Bridge() { stop(); }
void Bridge::start(const lsp::LspSpawnOptions& options) {
    stop();
    auto& b = *impl_;
    std::string error;
    if (!b.process.start(options, &error)) throw std::runtime_error("Cannot start WhatsApp bridge: " + error);
    { std::lock_guard<std::mutex> lock(b.mu); b.alive = true; b.failure.clear(); b.events.clear(); }
    b.reader = std::thread([&b] { b.read_loop(); });
    b.writer = std::thread([&b] { b.write_loop(); });
}
void Bridge::stop() {
    auto& b = *impl_;
    if (running()) {
        try { request("disconnect", Json::object(), std::chrono::milliseconds(750)); }
        catch (...) {}
    }
    b.fail("WhatsApp bridge stopped");
    // Kill first, join readers/writers, then close handles. Never race a pipe
    // handle mutation against an in-flight LspProcess read/write.
    b.process.kill_child();
    if (b.writer.joinable()) b.writer.join();
    if (b.reader.joinable()) b.reader.join();
    b.process.terminate();
    std::lock_guard<std::mutex> lock(b.mu); b.pending.clear(); b.events.clear();
}
bool Bridge::running() const { std::lock_guard<std::mutex> lock(impl_->mu); return impl_->alive; }
std::string Bridge::error() const { std::lock_guard<std::mutex> lock(impl_->mu); return impl_->failure; }
Json Bridge::request(const std::string& method, const Json& params, std::chrono::milliseconds timeout) {
    auto& b = *impl_;
    auto pending = std::make_shared<Impl::Pending>();
    std::unique_lock<std::mutex> lock(b.mu);
    if (!b.alive) throw std::runtime_error(b.failure.empty() ? "Bridge is not running" : b.failure);
    if (b.pending.size() >= 64 || b.writes.size() >= 64) throw std::runtime_error("Bridge request queue full");
    const auto id = b.next_id++;
    auto frame = Json{{"id", id}, {"method", method}, {"params", params}}.dump() + '\n';
    if (frame.size() > kMaxFrameBytes) throw std::runtime_error("Bridge request exceeds limit");
    b.pending[id] = pending; b.writes.push_back(std::move(frame)); b.cv.notify_all();
    const auto ready = b.cv.wait_for(lock, timeout, [&] { return pending->done; });
    b.pending.erase(id);
    if (!ready) {
        lock.unlock();
        // Do not retry a timed-out send: its delivery outcome may be unknown.
        b.fail("Bridge request timed out; delivery outcome unknown");
        throw std::runtime_error("Bridge request timed out; delivery outcome unknown");
    }
    if (!pending->error.empty()) throw std::runtime_error(pending->error);
    return pending->result;
}
std::vector<Json> Bridge::take_events() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::vector<Json> result; result.swap(impl_->events); return result;
}
std::filesystem::path find_whatsapp_bridge() {
    const auto executable = path_from_utf8(daemon::current_executable_path()).parent_path();
    std::vector<std::filesystem::path> candidates{executable / "channels" / "whatsapp" / "bridge.mjs"};
    candidates.push_back(executable.parent_path() / "Resources" / "channels" / "whatsapp" / "bridge.mjs");
#ifdef ACECODE_CHANNEL_ASSET_DIR
    candidates.push_back(path_from_utf8(ACECODE_CHANNEL_ASSET_DIR) / "bridge.mjs");
#endif
    for (const auto& path : candidates) if (std::filesystem::is_regular_file(path)) return path;
    throw std::runtime_error("WhatsApp bridge missing; install channels/whatsapp beside acecode");
}
std::filesystem::path prepare_whatsapp_bridge(const std::filesystem::path& directory) {
    const auto source = find_whatsapp_bridge().parent_path();
    const auto installed = directory / "bridge";
    // Never install npm dependencies inside Program Files or a signed .app.
    // Only fixed packaged assets are copied; account credentials live elsewhere.
    for (const auto* name : {"bridge.mjs", "protocol.mjs", "package.json", "package-lock.json"}) {
        std::ifstream file(source / name, std::ios::binary);
        if (!file || std::filesystem::file_size(source / name) > 1024 * 1024)
            throw std::runtime_error("Incomplete WhatsApp bridge package");
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::ifstream existing(installed / name, std::ios::binary);
        const std::string previous((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
        existing.close();
        if (previous != bytes && !atomic_write_file(path_to_utf8(installed / name), bytes, true))
            throw std::runtime_error("Cannot prepare private WhatsApp bridge files");
    }
    return installed;
}
bool whatsapp_dependencies_ready(const std::filesystem::path& installed) {
    try {
        std::ifstream manifest(installed / "package.json", std::ios::binary);
        const auto required = Json::parse(manifest).at("dependencies");
        for (const auto& dependency : required.items()) {
            const auto path = installed / "node_modules" / path_from_utf8(dependency.key()) / "package.json";
            if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 128 * 1024) {
                return false;
            }
            std::ifstream package(path, std::ios::binary);
            if (Json::parse(package).at("version") != dependency.value()) {
                return false;
            }
        }
        return !required.empty();
    } catch (...) { return false; }
}
lsp::LspSpawnOptions whatsapp_bridge_options(const std::filesystem::path& directory) {
    const auto installed = prepare_whatsapp_bridge(directory);
    if (!whatsapp_dependencies_ready(installed))
        throw std::runtime_error("WhatsApp dependencies missing or outdated. Run acecode channels to install them automatically.");
    const auto node = lsp::which("node");
    if (!node) throw std::runtime_error("Node.js 22+ is required on PATH for WhatsApp");
    return {{*node, path_to_utf8(installed / "bridge.mjs"), "--state-dir", path_to_utf8(directory)}, path_to_utf8(installed), {}};
}
} // namespace acecode::channels
