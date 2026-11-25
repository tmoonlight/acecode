#include "auto_review_config.hpp"

#include <stdexcept>

namespace acecode {

void read_auto_review_config(const nlohmann::json& root, AutoReviewConfig& config) {
    const auto found = root.find("auto_review");
    if (found == root.end()) return;
    if (!found->is_object() ||
        (found->contains("model_name") && !(*found)["model_name"].is_string())) {
        throw std::runtime_error("auto_review must be an object with a string model_name");
    }
    config.model_name = found->value("model_name", std::string{});
}

void write_auto_review_config(nlohmann::json& root, const AutoReviewConfig& config) {
    if (config.model_name.empty()) return;
    root["auto_review"] = {{"model_name", config.model_name}};
}

} // namespace acecode
