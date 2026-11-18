#pragma once

#include "page_annotation_runtime.hpp"
#include "page_annotation_text_runtime.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

namespace acecode::desktop {

class PageAnnotationSession {
public:
    using Json = nlohmann::json;

    Json snapshot() const {
        return {{"document_id", document_id_}, {"active", active_},
                {"popup", popup_}, {"target", target_}, {"viewport", viewport_}, {"records", records_},
                {"pending_count", records_.size()}, {"original", original_}, {"revision", revision_}};
    }

    void replace_document() {
        ++epoch_;
        ++sequence_;
        busy_ = false;
        active_ = false;
        popup_ = false;
        original_ = false;
        target_ = nullptr;
        document_id_.clear();
        for (auto& record : records_) record["valid"] = false;
        ++revision_;
    }

    bool prepare(const std::string& page_id, const Json& command,
                 std::string& expression, std::string& error) {
        if (!command.is_object() || command.dump().size() > 32768 ||
            !command.contains("action") || !command["action"].is_string()) {
            error = "Invalid annotation command";
            return false;
        }
        const auto action = command["action"].get<std::string>();
        if (action != "start" && action != "read" && action != "exit" &&
            action != "cancel" && action != "save" && action != "preview_text" && action != "original") {
            error = "Unsupported annotation action";
            return false;
        }
        for (const auto& item : command.items()) {
            if (item.key() != "action" && item.key() != "document_id" &&
                !(action == "save" && (item.key() == "description" || item.key() == "target_id")) &&
                !(action == "preview_text" && (item.key() == "text" || item.key() == "target_id")) &&
                !(action == "original" && item.key() == "enabled")) {
                error = "Unsupported annotation field";
                return false;
            }
        }
        if (busy_) {
            error = "Annotation command already in progress";
            return false;
        }
        if (document_id_.empty() && action == "start") {
            document_id_ = page_id + ":document:" + std::to_string(epoch_);
        }
        if (!command.contains("document_id") || !command["document_id"].is_string() ||
            (action != "start" && command["document_id"] != document_id_) ||
            (action == "start" && command["document_id"] != "" &&
             command["document_id"] != document_id_) || document_id_.empty()) {
            error = "Stale annotation document";
            return false;
        }
        if (action == "save" && (!active_ || !popup_ || !target_.is_object() ||
            !command.contains("target_id") || command["target_id"] != target_["id"] ||
            !command.contains("description") || !command["description"].is_string() ||
            command["description"].get_ref<const std::string&>().size() > 16384)) {
            error = "Invalid annotation draft";
            return false;
        }
        if (action == "preview_text" && (!active_ || !popup_ || !target_.is_object() ||
            !command.contains("target_id") || command["target_id"] != target_["id"] ||
            !command.contains("text") || !command["text"].is_string() ||
            command["text"].get_ref<const std::string&>().size() > 16384)) {
            error = "Invalid annotation text draft";
            return false;
        }
        if (action == "original" && (!command.contains("enabled") || !command["enabled"].is_boolean())) {
            error = "Invalid original view request";
            return false;
        }
        pending_ = command;
        page_id_ = page_id;
        Json payload{{"action", action}, {"document_id", document_id_}};
        if (action == "save") {
            payload["target_id"] = target_["id"];
            payload["number"] = next_number_;
        }
        if (action == "preview_text") {
            payload["target_id"] = command["target_id"];
            payload["text"] = command["text"];
        }
        if (action == "original") payload["enabled"] = command["enabled"];
        expression = std::string(kPageAnnotationRuntime) + "(" + payload.dump() + "," + kPageAnnotationTextRuntime + ")";
        busy_ = true;
        ++sequence_;
        return true;
    }

    std::uint64_t sequence() const { return sequence_; }

    bool complete(std::uint64_t sequence, const Json& result, std::string& error) {
        if (sequence != sequence_ || !busy_) {
            error = "Stale annotation callback";
            return false;
        }
        busy_ = false;
        if (!valid_result(result)) {
            error = "Invalid or stale annotation result";
            return false;
        }
        const auto action = pending_["action"].get<std::string>();
        if ((action == "save" || action == "cancel" || action == "exit") &&
            (!result["target"].is_null() || result["popup"] != false)) {
            error = "Annotation action did not complete";
            return false;
        }
        const bool changed = action == "save" || action == "original" || active_ != result["active"] ||
            popup_ != result["popup"] || target_ != result["target"] || viewport_ != result["viewport"];
        if (action == "save") {
            records_.push_back({
                {"id", document_id_ + ":annotation:" + std::to_string(next_number_)},
                {"number", next_number_++}, {"document_id", document_id_},
                {"page_id", page_id_},
                {"description", pending_["description"]}, {"targets", Json::array({target_})},
                {"differences", result.value("differences", Json::array())}, {"region", nullptr},
                {"status", "pending"}, {"valid", true},
            });
        }
        active_ = result["active"].get<bool>();
        popup_ = result["popup"].get<bool>();
        target_ = result["target"];
        viewport_ = result["viewport"];
        original_ = result.value("original", false);
        if (changed) ++revision_;
        return true;
    }

    void fail(std::uint64_t sequence) {
        if (sequence == sequence_) busy_ = false;
    }

private:
    bool valid_result(const Json& result) const {
        if (!result.is_object() || result.dump().size() > 131072 ||
            !result.contains("document_id") || result["document_id"] != document_id_ ||
            !result.contains("active") || !result["active"].is_boolean() ||
            !result.contains("popup") || !result["popup"].is_boolean() ||
            !result.contains("target") || !result.contains("viewport") || !result["viewport"].is_object()) return false;
        for (const auto& field : {"width", "height"}) {
            const auto& viewport = result["viewport"];
            if (!viewport.contains(field) || !viewport[field].is_number()) return false;
            const double value = viewport[field].get<double>();
            if (!std::isfinite(value) || value <= 0 || value > 10000000) return false;
        }
        if (result.contains("original") && !result["original"].is_boolean()) return false;
        if (result.contains("differences")) {
            if (!result["differences"].is_array() || result["differences"].size() > 1) return false;
            for (const auto& difference : result["differences"]) {
                if (!difference.is_object() || !target_.is_object() ||
                    difference.value("target_id", Json()) != target_["id"] ||
                    difference.value("property", Json()) != "text") return false;
                for (const auto& field : {"old_value", "new_value"}) {
                    if (!difference.contains(field) || !difference[field].is_string() ||
                        difference[field].get_ref<const std::string&>().size() > 16384) return false;
                }
                if (difference["old_value"] == difference["new_value"]) return false;
            }
        }
        const auto& target = result["target"];
        if (result["active"] == false && (!target.is_null() || result["popup"] != false)) return false;
        if (target.is_null()) return result["popup"] == false;
        if (!target.is_object()) return false;
        if (target.contains("text_preview")) {
            const auto& text = target["text_preview"];
            if (!text.is_object() || !text.contains("value") || !text["value"].is_string() ||
                text["value"].get_ref<const std::string&>().size() > 16384 ||
                !text.contains("editable") || !text["editable"].is_boolean() ||
                !text.contains("error") || !text["error"].is_string()) return false;
        }
        for (const auto& field : {"id", "tag", "text", "locator"}) {
            if (!target.contains(field) || !target[field].is_string() ||
                target[field].get_ref<const std::string&>().size() > 2048) return false;
        }
        if (target["id"] == "" || target["tag"] == "" ||
            !target.contains("bounds") || !target["bounds"].is_object()) return false;
        for (const auto& field : {"x", "y", "width", "height"}) {
            const auto& bounds = target["bounds"];
            if (!bounds.contains(field) || !bounds[field].is_number()) return false;
            const double value = bounds[field].get<double>();
            if (!std::isfinite(value) || std::abs(value) > 10000000 ||
                ((std::string(field) == "width" || std::string(field) == "height") && value < 0)) return false;
        }
        return true;
    }

    std::string document_id_;
    std::string page_id_;
    std::uint64_t epoch_ = 1;
    std::uint64_t sequence_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t next_number_ = 1;
    bool busy_ = false;
    bool active_ = false;
    bool popup_ = false;
    bool original_ = false;
    Json target_ = nullptr;
    Json viewport_ = nullptr;
    Json records_ = Json::array();
    Json pending_;
};

} // namespace acecode::desktop
