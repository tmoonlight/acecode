#pragma once
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace acecode::share {
nlohmann::json project_message(const nlohmann::json& message, bool include_tools);
nlohmann::json redact_share_json(nlohmann::json value);
bool guest_sensitive_path(std::string_view path);

// Hold incomplete lines and PEM blocks so secrets split across model chunks
// cannot escape before the existing redactor has seen the complete value.
class ShareStreamRedactor {
public:
    std::string append(std::string_view delta);
    std::string finish();
    void reset() { pending_.clear(); }
private:
    std::string pending_;
};
} // namespace acecode::share
