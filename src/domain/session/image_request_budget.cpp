#include "image_request_budget.hpp"

#include "attachment_prompt_context.hpp"
#include "utils/logger.hpp"

#include <algorithm>

namespace acecode {

namespace {

constexpr std::size_t kTriggerHeadroomBytes = 3u * 1024u * 1024u;
// Below this cap the reclaim target would meet the trigger.
constexpr std::size_t kMinRequestBytes = 4u * kTriggerHeadroomBytes;
constexpr std::size_t kMessageFrameBytes = 48;
constexpr std::size_t kImagePartFrameBytes = 64;
// Approximate size of the file reference that replaces a removed image.
constexpr std::size_t kWithheldReferenceBytes = 720;
constexpr std::size_t kFileReferenceBytes = 600;

struct ImageLocation {
    std::size_t message = 0;
    std::size_t part = 0;
    std::string key;
    std::size_t bytes = 0;
};

bool is_image_part(const nlohmann::json& part) {
    return part.is_object() && part.value("type", std::string{}) == "image";
}

std::string image_key(const nlohmann::json& part) {
    const auto attachment = part.find("attachment");
    if (attachment == part.end() || !attachment->is_object()) return {};
    std::string key = attachment->value("id", std::string{});
    if (key.empty()) key = attachment->value("path", std::string{});
    return key;
}

std::size_t message_text_bytes(const ChatMessage& message) {
    std::size_t bytes = kMessageFrameBytes + message.reasoning_content.size() +
                        message.tool_call_id.size();
    if (message.tool_calls.is_array() && !message.tool_calls.empty()) {
        bytes += message.tool_calls.dump().size();
    }
    bool has_text_part = false;
    if (message.content_parts.is_array()) {
        for (const auto& part : message.content_parts) {
            if (!part.is_object() || is_image_part(part)) continue;
            const std::string type = part.value("type", std::string{});
            if (type == "text") {
                has_text_part = true;
                bytes += part.value("text", std::string{}).size();
            } else if (type == "file") {
                bytes += kFileReferenceBytes;
            } else {
                bytes += part.dump().size();
            }
        }
    }
    // The serializer sends `content` only when no text part carries the text.
    if (!has_text_part) bytes += message.content.size();
    return bytes;
}

void withhold_image_part(nlohmann::json& part) {
    part["type"] = "file";
    const auto attachment = part.find("attachment");
    if (attachment == part.end() || !attachment->is_object()) return;
    auto& metadata = (*attachment)["metadata"];
    if (!metadata.is_object()) metadata = nlohmann::json::object();
    metadata[kImageBudgetWithheldMetadataKey] = true;
}

} // namespace

ImageRequestBudgetLimits image_request_budget_limits(std::size_t max_request_bytes) {
    const std::size_t cap = std::max(max_request_bytes, kMinRequestBytes);
    return {cap - kTriggerHeadroomBytes, cap / 2};
}

ImageRequestBudget::ImageRequestBudget(const ImageRequestBudget& other) {
    std::lock_guard<std::mutex> lock(other.mu_);
    watermark_ = other.watermark_;
}

ImageRequestBudget& ImageRequestBudget::operator=(const ImageRequestBudget& other) {
    if (this == &other) return *this;
    std::string watermark;
    {
        std::lock_guard<std::mutex> lock(other.mu_);
        watermark = other.watermark_;
    }
    std::lock_guard<std::mutex> lock(mu_);
    watermark_ = std::move(watermark);
    return *this;
}

ImageRequestBudgetOutcome ImageRequestBudget::apply(std::vector<ChatMessage>& messages,
                                                    const ImageRequestBudgetLimits& limits,
                                                    const ImageWireBytesFn& image_wire_bytes) {
    ImageRequestBudgetOutcome outcome;
    std::vector<ImageLocation> images;
    std::size_t total = 0;
    for (std::size_t m = 0; m < messages.size(); ++m) {
        const auto& message = messages[m];
        total += message_text_bytes(message);
        if (!message.content_parts.is_array()) continue;
        for (std::size_t p = 0; p < message.content_parts.size(); ++p) {
            const auto& part = message.content_parts[p];
            if (!is_image_part(part)) continue;
            const std::size_t bytes = image_wire_bytes ? image_wire_bytes(part) : 0;
            total += kImagePartFrameBytes + bytes;
            if (bytes > 0) images.push_back({m, p, image_key(part), kImagePartFrameBytes + bytes});
        }
    }
    outcome.body_bytes = total;
    outcome.body_bytes_after = total;
    outcome.inline_images = images.size();
    if (images.empty() || limits.trigger_bytes == 0) return outcome;

    std::size_t evicted = 0;
    std::size_t running = total;
    const auto evict_next = [&] {
        running -= std::min(running, images[evicted].bytes);
        running += kWithheldReferenceBytes;
        ++evicted;
    };

    std::lock_guard<std::mutex> lock(mu_);
    if (!watermark_.empty()) {
        const auto found = std::find_if(images.begin(), images.end(),
            [this](const ImageLocation& image) { return image.key == watermark_; });
        if (found != images.end()) {
            const auto through = static_cast<std::size_t>(found - images.begin()) + 1;
            while (evicted < through) evict_next();
        }
    }
    const std::size_t remembered = evicted;
    if (running >= limits.trigger_bytes) {
        while (evicted < images.size() && running > limits.reclaim_target_bytes) evict_next();
        if (evicted > remembered && !images[evicted - 1].key.empty()) {
            watermark_ = images[evicted - 1].key;
        }
    }
    if (evicted == 0) return outcome;

    for (std::size_t i = 0; i < evicted; ++i) {
        withhold_image_part(messages[images[i].message].content_parts[images[i].part]);
    }
    outcome.evicted = evicted;
    outcome.body_bytes_after = running;
    if (evicted > remembered) {
        LOG_INFO("[image-budget] body_bytes=" + std::to_string(total) +
                 " trigger=" + std::to_string(limits.trigger_bytes) +
                 " reclaim_target=" + std::to_string(limits.reclaim_target_bytes) +
                 " inline_images=" + std::to_string(images.size()) +
                 " evicted=" + std::to_string(evicted) +
                 " body_bytes_after=" + std::to_string(running));
    }
    return outcome;
}

} // namespace acecode
