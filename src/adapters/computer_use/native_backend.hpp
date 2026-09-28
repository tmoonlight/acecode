#pragma once

#include <memory>
#include <nlohmann/json.hpp>

namespace acecode::computer_use {

// Owned by the private worker. Observations authorize one stateful action.
class NativeBackend {
public:
    NativeBackend();
    ~NativeBackend();
    NativeBackend(const NativeBackend&) = delete;
    NativeBackend& operator=(const NativeBackend&) = delete;
    nlohmann::json dispatch(const nlohmann::json& request);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode::computer_use
