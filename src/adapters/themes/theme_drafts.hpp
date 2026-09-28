#pragma once

#include "theme_store.hpp"

namespace acecode::themes {

// Durable, session-owned workflow. Approval comes exclusively from the native
// question callback and binds the palette and copied image bytes together.
class ThemeDraftStore {
public:
    using Confirm = std::function<nlohmann::json(const nlohmann::json&)>;
    explicit ThemeDraftStore(std::filesystem::path theme_root);
    nlohmann::json execute(const nlohmann::json& arguments,
                           const std::string& session_id,
                           const std::string& cwd,
                           const Confirm& confirm);
private:
    std::filesystem::path root_;
};

} // namespace acecode::themes
