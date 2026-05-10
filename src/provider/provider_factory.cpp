#include "provider_factory.hpp"
#include "openai_provider.hpp"
#include "copilot_provider.hpp"

namespace acecode {

std::shared_ptr<LlmProvider> create_provider_from_entry(const ModelProfile& entry) {
    if (entry.provider == "openai") {
        return std::make_shared<OpenAiCompatProvider>(
            entry.base_url,
            entry.api_key,
            entry.model
        );
    }
    // 默认走 copilot —— 与 create_provider 的 else 分支行为一致。
    return std::make_shared<CopilotProvider>(entry.model);
}

} // namespace acecode
