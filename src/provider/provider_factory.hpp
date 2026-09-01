#pragma once

#include "llm_provider.hpp"
#include "../config/saved_models.hpp"
#include <memory>
#include <optional>
#include <string>

namespace acecode {

struct AppConfig;
class PreparedProviderConstruction;

// Opaque equality token for the exact effective inputs consumed by provider
// construction. The retained value is a process-salted digest; there is no
// public string, stream, JSON, or metadata representation.
class ProviderConstructionFingerprint {
public:
    ProviderConstructionFingerprint(const ProviderConstructionFingerprint&) = default;
    ProviderConstructionFingerprint& operator=(
        const ProviderConstructionFingerprint&) = default;

    bool operator==(const ProviderConstructionFingerprint& other) const noexcept;
    bool operator!=(const ProviderConstructionFingerprint& other) const noexcept {
        return !(*this == other);
    }

private:
    explicit ProviderConstructionFingerprint(std::string digest);
    std::string digest_;

    friend class PreparedProviderConstruction;
    friend std::optional<PreparedProviderConstruction>
    prepare_provider_construction(const ModelProfile&, const AppConfig*);
};

struct ProviderConstructionResult {
    std::shared_ptr<LlmProvider> provider;
    ProviderConstructionFingerprint fingerprint;
};

// Move-only opaque handle around the private effective build plan. Callers can
// compare the fingerprint before paying provider-construction/authentication
// cost, but cannot inspect or serialize credentials retained by the plan.
class PreparedProviderConstruction {
public:
    PreparedProviderConstruction(PreparedProviderConstruction&&) noexcept;
    PreparedProviderConstruction& operator=(PreparedProviderConstruction&&) noexcept;
    ~PreparedProviderConstruction();

    PreparedProviderConstruction(const PreparedProviderConstruction&) = delete;
    PreparedProviderConstruction& operator=(const PreparedProviderConstruction&) = delete;

    const ProviderConstructionFingerprint& fingerprint() const noexcept;
    ProviderConstructionResult construct() const;

private:
    struct Impl;
    explicit PreparedProviderConstruction(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend std::optional<PreparedProviderConstruction>
    prepare_provider_construction(const ModelProfile&, const AppConfig*);
};

std::optional<PreparedProviderConstruction> prepare_provider_construction(
    const ModelProfile& entry,
    const AppConfig* config = nullptr);

std::optional<ProviderConstructionResult> create_provider_construction(
    const ModelProfile& entry,
    const AppConfig* config = nullptr);

// 基于一个 ModelProfile 构造 provider。对应 openspec/changes/model-profiles
// 任务 4.3 / design.md D4。调用方不需要再持有整份 AppConfig。
// 返回 shared_ptr 以匹配 main.cpp 可替换容器的语义(design D4)。
//
// config(可选):用于设置能力路由上下文(route-attachments-by-capability D5)。
// model_has_vision 始终从 entry.capabilities 推导;config 非空时再据
// has_any_runtime_vision_model(config) 设置 any_vision_model_available,影响非视觉
// 模型剥图后的 fallback 文本措辞。漏传 config 不影响 gate 正确性(只退化措辞)。
std::shared_ptr<LlmProvider> create_provider_from_entry(const ModelProfile& entry,
                                                        const AppConfig* config = nullptr);

} // namespace acecode
