#pragma once

#include "config/config.hpp"
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace acecode::speech {

constexpr std::size_t kMaxAudioBytes = 20 * 1024 * 1024;
struct RequestPlan {
    std::string url;
    std::string file_field;
    std::vector<std::pair<std::string, std::string>> fields;
};
struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int status = 502;
};

RequestPlan request_plan(const SpeechRecognitionConfig& config);
Result parse_response(int status, const std::string& body);
std::string silent_wav();
bool valid_wav(const std::string& audio, int max_seconds);
// Cancellation is shared by the request registry and the owned HTTP operation.
Result transcribe(SpeechRecognitionConfig config, std::string audio,
                  std::shared_ptr<std::atomic<bool>> cancelled,
                  const std::atomic<bool>* shutdown = nullptr);

} // namespace acecode::speech
