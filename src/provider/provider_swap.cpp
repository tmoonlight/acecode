// provider_swap 实现。见 design.md D4。
#include "provider_swap.hpp"

#include "copilot_provider.hpp"
#include "openai_provider.hpp"
#include "provider_factory.hpp"
#include "model_context_resolver.hpp"
#include "../utils/logger.hpp"

namespace acecode {

void swap_provider_if_needed(std::shared_ptr<LlmProvider>& handle,
                             std::mutex& mu,
                             const ModelProfile& entry,
                             AppConfig& cfg) {
    {
        std::lock_guard<std::mutex> lk(mu);
        std::string current_name = handle ? handle->name() : std::string{};
        if (current_name == entry.provider) {
            // 同 provider 类别 —— 复用现有实例,只切 model + (openai 还要 reconfigure)。
            handle->set_model(entry.model);
            if (entry.provider == "openai") {
                // OpenAiCompatProvider 是 OpenAI / Copilot 共同的基类;但只有
                // 真正的 openai-compat entry 才需要重新指向 base_url/api_key。
                auto* op = dynamic_cast<OpenAiCompatProvider*>(handle.get());
                if (op) op->reconfigure(entry.base_url, entry.api_key);
            }
        } else {
            // 跨 provider —— 销毁旧实例(引用计数归零后),新建。worker 线程拿
            // 的旧 shared_ptr 仍能跑完当前 turn(design D4 风险缓解)。
            handle = create_provider_from_entry(entry);
            // 切到 Copilot 时,新实例的 github_token_ 需要从磁盘加载;否则首次
            // chat() 报 "Copilot session token unavailable"。
            // 用 provider->name() 判定(一定是字面 "copilot"),而非 entry.provider —
            // 后者可能为空/大小写不同,但 create_provider_from_entry 只要不是 "openai"
            // 就默认造 Copilot,所以 entry.provider 字段不可靠。
            if (handle && handle->name() == "copilot") {
                if (auto copilot = std::dynamic_pointer_cast<CopilotProvider>(handle)) {
                    if (!copilot->try_silent_auth()) {
                        LOG_WARN("[provider_swap] Copilot silent_auth failed after swap "
                                 "(model='" + entry.model + "')");
                    }
                }
            }
        }
    }

    // 重新计算 context_window —— 不持锁(避免 resolve 调长 IO 时阻塞 worker)。
    if (handle) {
        cfg.context_window = resolve_model_context_window(
            cfg, handle->name(), handle->model(), cfg.context_window);
        LOG_INFO("[provider_swap] now using entry '" + entry.name +
                 "' (" + handle->name() + "/" + handle->model() +
                 "), context_window=" + std::to_string(cfg.context_window));
    }
}

} // namespace acecode
