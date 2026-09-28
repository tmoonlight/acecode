#include "session_writer_lease.hpp"

#include "utils/atomic_file.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <utility>

namespace fs = std::filesystem;

namespace acecode {
namespace {

SessionWriterLeaseInfo from_json(const nlohmann::json& j) {
    SessionWriterLeaseInfo info;
    info.pid = j.value("pid", static_cast<daemon::pid_t_compat>(0));
    info.cwd = j.value("cwd", std::string{});
    info.project_dir = j.value("project_dir", std::string{});
    info.session_id = j.value("session_id", std::string{});
    info.surface = j.value("surface", std::string{});
    info.updated_at_ms = j.value("updated_at_ms", static_cast<std::int64_t>(0));
    return info;
}

nlohmann::json to_json(const SessionWriterLeaseInfo& info) {
    nlohmann::json j;
    j["pid"] = info.pid;
    j["cwd"] = info.cwd;
    j["project_dir"] = info.project_dir;
    j["session_id"] = info.session_id;
    j["surface"] = info.surface;
    j["updated_at_ms"] = info.updated_at_ms;
    return j;
}

bool write_lease(const std::string& path, const SessionWriterLeaseInfo& info) {
    return atomic_write_file(path, to_json(info).dump(2) + '\n');
}

} // namespace

std::int64_t SessionWriterLease::now_ms() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::string SessionWriterLease::lease_path(const std::string& project_dir,
                                           const std::string& session_id) {
    return path_to_utf8(path_from_utf8(project_dir) / "leases" / (session_id + ".writer.json"));
}

std::optional<SessionWriterLeaseInfo> SessionWriterLease::read(
    const std::string& project_dir,
    const std::string& session_id) {
    const std::string path = lease_path(project_dir, session_id);
    std::ifstream ifs(path_from_utf8(path));
    if (!ifs.is_open()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(ifs);
        auto info = from_json(j);
        if (info.session_id.empty()) info.session_id = session_id;
        if (info.project_dir.empty()) info.project_dir = project_dir;
        return info;
    } catch (...) {
        return std::nullopt;
    }
}

SessionWriterLeaseResult SessionWriterLease::acquire(
    const std::string& project_dir,
    const std::string& session_id,
    const std::string& cwd,
    const std::string& surface,
    daemon::pid_t_compat pid,
    std::int64_t now,
    std::int64_t stale_ms) {
    SessionWriterLeaseResult result;
    result.path = lease_path(project_dir, session_id);

    if (project_dir.empty() || session_id.empty()) {
        result.status = SessionWriterLeaseResult::Status::Error;
        result.error = "missing project_dir or session_id";
        return result;
    }

    if (pid == 0) pid = daemon::current_pid();
    if (now == 0) now = now_ms();
    if (stale_ms <= 0) stale_ms = kDefaultStaleMs;

    std::error_code ec;
    fs::create_directories(path_from_utf8(result.path).parent_path(), ec);
    if (ec) {
        result.status = SessionWriterLeaseResult::Status::Error;
        result.error = ec.message();
        return result;
    }

    auto existing = read(project_dir, session_id);
    if (existing.has_value() && existing->pid != 0 && existing->pid != pid) {
        const bool alive = daemon::is_pid_alive(existing->pid);
        const bool fresh = existing->updated_at_ms > 0 &&
                           now >= existing->updated_at_ms &&
                           (now - existing->updated_at_ms) <= stale_ms;
        if (alive && fresh) {
            result.status = SessionWriterLeaseResult::Status::Conflict;
            result.owner = *existing;
            return result;
        }
        result.stale_recovered = true;
    }

    SessionWriterLeaseInfo owner;
    owner.pid = pid;
    owner.cwd = cwd;
    owner.project_dir = project_dir;
    owner.session_id = session_id;
    owner.surface = surface;
    owner.updated_at_ms = now;

    if (!write_lease(result.path, owner)) {
        result.status = SessionWriterLeaseResult::Status::Error;
        result.error = "failed to write lease";
        return result;
    }

    result.status = SessionWriterLeaseResult::Status::Acquired;
    result.owner = std::move(owner);
    return result;
}

bool SessionWriterLease::refresh(const std::string& project_dir,
                                 const std::string& session_id,
                                 daemon::pid_t_compat pid,
                                 std::int64_t now) {
    if (pid == 0) pid = daemon::current_pid();
    if (now == 0) now = now_ms();

    auto existing = read(project_dir, session_id);
    if (!existing.has_value()) return false;
    if (existing->pid != pid) return false;

    existing->updated_at_ms = now;
    return write_lease(lease_path(project_dir, session_id), *existing);
}

bool SessionWriterLease::release(const std::string& project_dir,
                                 const std::string& session_id,
                                 daemon::pid_t_compat pid) {
    if (pid == 0) pid = daemon::current_pid();
    auto existing = read(project_dir, session_id);
    if (!existing.has_value() || existing->pid != pid) return false;

    std::error_code ec;
    fs::remove(path_from_utf8(lease_path(project_dir, session_id)), ec);
    return !ec;
}

void SessionWriterLease::remove(const std::string& project_dir,
                                const std::string& session_id) {
    std::error_code ec;
    fs::remove(path_from_utf8(lease_path(project_dir, session_id)), ec);
}

WriterLease::WriterLease(std::string project_dir, std::string session_id)
    : project_dir_(std::move(project_dir)), session_id_(std::move(session_id)),
      pid_(daemon::current_pid()) {}

WriterLease::~WriterLease() { reset(); }

WriterLease::WriterLease(WriterLease&& other) noexcept
    : project_dir_(std::move(other.project_dir_)), session_id_(std::move(other.session_id_)),
      pid_(other.pid_), active_(std::exchange(other.active_, false)) {}

WriterLease& WriterLease::operator=(WriterLease&& other) noexcept {
    if (this != &other) {
        reset();
        project_dir_ = std::move(other.project_dir_);
        session_id_ = std::move(other.session_id_);
        pid_ = other.pid_;
        active_ = std::exchange(other.active_, false);
    }
    return *this;
}

SessionWriterLeaseResult WriterLease::acquire(const std::string& cwd, const std::string& surface) {
    auto result = SessionWriterLease::acquire(project_dir_, session_id_, cwd, surface, pid_);
    active_ = result.status == SessionWriterLeaseResult::Status::Acquired;
    return result;
}

bool WriterLease::refresh() {
    if (!active_) return false;
    active_ = SessionWriterLease::refresh(project_dir_, session_id_, pid_);
    return active_;
}

bool WriterLease::matches(const std::string& project_dir, const std::string& session_id) const {
    return project_dir_ == project_dir && session_id_ == session_id;
}

void WriterLease::reset() noexcept {
    if (!std::exchange(active_, false)) return;
    try {
        SessionWriterLease::release(project_dir_, session_id_, pid_);
    } catch (...) {
        // Destruction is best effort, like explicit SessionManager shutdown.
        // A failed release is recovered by the existing stale-lease mechanism.
    }
}

} // namespace acecode
