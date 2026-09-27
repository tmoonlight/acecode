#include "models_dev_catalog.hpp"

namespace acecode {

std::vector<std::string> model_capability_tags(const ModelEntry& model) {
    std::vector<std::string> tags;
    if (model.attachment) tags.push_back("vision");
    if (model.tool_call) tags.push_back("tool_use");
    if (model.reasoning) tags.push_back("reasoning");
    return tags;
}

} // namespace acecode
