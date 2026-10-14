#include "provider_request_options.hpp"

namespace acecode {

void append_sampling_request_fields(nlohmann::json& body,
                                    const ModelSamplingOptions& sampling,
                                    bool supports_min_p) {
    if (sampling.temperature.has_value()) body["temperature"] = *sampling.temperature;
    if (sampling.top_p.has_value()) body["top_p"] = *sampling.top_p;
    if (sampling.top_k.has_value()) body["top_k"] = *sampling.top_k;
    if (supports_min_p && sampling.min_p.has_value()) body["min_p"] = *sampling.min_p;
}

} // namespace acecode
