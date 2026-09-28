#pragma once
#include "llm/llm_provider.hpp"

namespace acecode::agent {
// A synchronous request-scoped sink. The collector drains its callback gate
// before returning, so these calls cannot outlive the worker's turn context.
class ModelStepSink {
public:
    virtual ~ModelStepSink() = default;
    virtual void first_output(int step, int attempt, const std::string& channel) = 0;
    virtual void accept_usage(const TokenUsage& usage) = 0;
};
} // namespace acecode::agent
