#pragma once
#include <string>

namespace acecode::platform {
bool should_register_invite_protocol(const std::string& registered_executable,
                                    bool registered_exists,
                                    const std::string& current_executable);
// Per-user only. A different live installation is never overwritten.
bool register_invite_protocol(const std::string& executable, const std::string& registry_root = {});
bool unregister_invite_protocol(const std::string& executable, const std::string& registry_root = {});
} // namespace acecode::platform
