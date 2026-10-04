#include "anthropic_provider.hpp"
#include "openai_provider.hpp"

#include <utility>

namespace acecode {

OpenAiCompatProvider::OpenAiCompatProvider(const std::string& base_url,
                                         const std::string& api_key,
                                         const std::string& model,
                                         int stream_timeout_ms,
                                         std::map<std::string, std::string> request_headers,
                                         ProviderRequestOptions request_options)
    : base_url_(normalize_endpoint(base_url, request_options.endpoint_mode)),
      api_key_(api_key),
      model_(model),
      request_headers_(std::move(request_headers)),
      request_options_(std::move(request_options)),
      stream_timeout_ms_(stream_timeout_ms > 0
          ? stream_timeout_ms
          : OpenAiConfig::kDefaultStreamTimeoutMs) {}

AnthropicProvider::AnthropicProvider(const std::string& base_url,
                                     const std::string& api_key,
                                     const std::string& model,
                                     int stream_timeout_ms,
                                     std::map<std::string, std::string> request_headers,
                                     ProviderRequestOptions request_options)
    : base_url_(normalize_base_url(base_url)),
      api_key_(api_key),
      model_(model),
      request_headers_(std::move(request_headers)),
      request_options_(std::move(request_options)),
      stream_timeout_ms_(stream_timeout_ms > 0
          ? stream_timeout_ms
          : OpenAiConfig::kDefaultStreamTimeoutMs) {}

} // namespace acecode
