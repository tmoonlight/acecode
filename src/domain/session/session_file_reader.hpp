#pragma once

#include "platform/process/unique_resources.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>

namespace acecode {

class HistorySnapshotChanged : public std::runtime_error {
public:
    HistorySnapshotChanged() : std::runtime_error("session history changed while reading") {}
};

// An owned, immutable-size view of one file identity. Opening captures only OS
// metadata; all content reads are explicit so callers can first release locks.
class SessionFileReader {
public:
    explicit SessionFileReader(std::string path);
    SessionFileReader(SessionFileReader&&) noexcept = default;
    SessionFileReader& operator=(SessionFileReader&&) noexcept = default;
    SessionFileReader(const SessionFileReader&) = delete;
    SessionFileReader& operator=(const SessionFileReader&) = delete;

    bool valid() const;
    std::uint64_t size() const { return size_; }
    const std::string& path() const { return path_; }
    const std::string& identity() const { return identity_; }
    std::string read(std::uint64_t offset, std::uint64_t count) const;
    std::string prefix() const;
    bool unchanged(const std::string& original_prefix) const;

private:
    std::string path_;
    std::string identity_;
    std::uint64_t size_ = 0;
#ifdef _WIN32
    platform::UniqueHandle handle_;
#else
    platform::UniqueFd handle_;
#endif
};

} // namespace acecode
