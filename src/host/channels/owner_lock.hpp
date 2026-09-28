#pragma once

#include <filesystem>
#include <memory>

namespace acecode::channels {

class OwnerLock {
public:
    OwnerLock();
    ~OwnerLock();
    bool acquire(const std::filesystem::path& path);
    void release();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode::channels
