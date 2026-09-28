#pragma once

#include "runtime.hpp"

#include <functional>
#include <string>
#include <utility>

namespace acecode::computer_use {

// Owns the obligation to release the session at scope exit. The normal turn
// also releases before terminal events; the second release is intentionally
// preserved because release_session is idempotent (P0-11 lifecycle contract).
class SessionLease {
public:
    using Release = std::function<void(const std::string&)>;
    explicit SessionLease(std::string owner, Release release = {})
        : owner_(std::move(owner)),
          release_(release ? std::move(release) : Release(release_session)) {}
    ~SessionLease() { if (active_) release_(owner_); }
    SessionLease(const SessionLease&) = delete;
    SessionLease& operator=(const SessionLease&) = delete;
    SessionLease(SessionLease&& other) noexcept
        : owner_(std::move(other.owner_)), release_(std::move(other.release_)),
          active_(std::exchange(other.active_, false)) {}
    SessionLease& operator=(SessionLease&&) = delete;
    // Initial persistence may assign the owner after the turn lease is created.
    void set_owner(std::string owner) { owner_ = std::move(owner); }
    const std::string& owner() const noexcept { return owner_; }
    void release_before_terminal() { if (active_) release_(owner_); }

private:
    std::string owner_;
    Release release_; // Owned value; no borrowed AgentLoop is retained.
    bool active_ = true;
};

} // namespace acecode::computer_use
