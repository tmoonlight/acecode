#pragma once

#include "page_annotation_runtime.hpp"
#include "page_annotation_text_runtime.hpp"
#include "page_annotation_targets.hpp"

#include <algorithm>
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
                {"popup", popup_}, {"target", target_}, {"targets", targets_}, {"region", region_},
                {"selection_mode", selection_mode_}, {"viewport", viewport_}, {"records", records_},
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
        targets_ = Json::array();
        region_ = nullptr;
        selection_mode_ = nullptr;
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
            action != "cancel" && action != "save" && action != "add_target" &&
            action != "add_region" && action != "select_target" && action != "select_region" &&
            action != "preview_text" && action != "original") {
            error = "Unsupported annotation action";
            return false;
        }
        for (const auto& item : command.items()) {
            if (item.key() != "action" && item.key() != "document_id" &&
                !(action == "preview_text" && (item.key() == "text" || item.key() == "target_id")) &&
                !(action == "original" && item.key() == "enabled") &&
                !(action == "save" && (item.key() == "description" || item.key() == "target_id" ||
                                      item.key() == "target_ids" || item.key() == "region")) &&
                !(action == "select_target" && item.key() == "target_id")) {
                error = "Unsupported annotation field";
                return false;
            }
        }
        if (busy_) {
            error = "Annotation command already in progress";
            return false;
        }
        if (!page_id_.empty() && page_id != page_id_) {
            error = "Stale annotation page";
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
        if ((action == "add_target" || action == "add_region" || action == "select_target" ||
             action == "select_region") && (!active_ || !popup_ || !selection_mode_.is_null())) {
            error = "Invalid annotation selection";
            return false;
        }
        if (action == "select_target" && (!command.contains("target_id") ||
            !command["target_id"].is_string() ||
            std::none_of(targets_.begin(), targets_.end(), [&](const Json& target) {
                return target["id"] == command["target_id"];
            }))) {
            error = "Unknown annotation target";
            return false;
        }
        if (action == "select_region" && region_.is_null()) {
            error = "Missing annotation region";
            return false;
        }
        if (action == "save" && (!active_ || !popup_ || !selection_mode_.is_null() ||
            (targets_.empty() && region_.is_null()) ||
            !command.contains("target_id") || command["target_id"] != (target_.is_null() ? Json(nullptr) : target_["id"]) ||
            !command.contains("target_ids") || command["target_ids"] != annotation_target_ids(targets_) ||
            !command.contains("region") || command["region"] != region_ ||
            !command.contains("description") || !command["description"].is_string() ||
            command["description"].get_ref<const std::string&>().size() > 16384)) {
            error = "Invalid annotation draft";
            return false;
        }
        if (action == "preview_text" && (!active_ || !popup_ || !selection_mode_.is_null() || !target_.is_object() ||
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
            payload["target_id"] = command["target_id"];
            payload["target_ids"] = command["target_ids"];
            payload["region"] = region_;
            payload["number"] = next_number_;
        }
        if (action == "preview_text") {
            payload["target_id"] = command["target_id"];
            payload["text"] = command["text"];
        }
        if (action == "original") payload["enabled"] = command["enabled"];
        expression = std::string(kPageAnnotationRuntime) + "(" + payload.dump() + "," + kPageAnnotationTextRuntime + ")";
        if (action == "select_target") payload["target_id"] = command["target_id"];
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
            (!result["target"].is_null() || result["popup"] != false ||
             !result["targets"].empty() || !result["region"].is_null() || !result["selection_mode"].is_null())) {
            error = "Annotation action did not complete";
            return false;
        }
        const bool changed = action == "save" || action == "original" || active_ != result["active"] ||
            popup_ != result["popup"] || target_ != result["target"] || viewport_ != result["viewport"] ||
            targets_ != result["targets"] || region_ != result["region"] || selection_mode_ != result["selection_mode"];
        if (action == "save") {
            records_.push_back({
                {"id", document_id_ + ":annotation:" + std::to_string(next_number_)},
                {"number", next_number_++}, {"document_id", document_id_},
                {"page_id", page_id_},
                {"description", pending_["description"]}, {"targets", targets_},
                {"differences", result.value("differences", Json::array())}, {"region", region_},
                {"status", "pending"}, {"valid", true},
            });
        }
        active_ = result["active"].get<bool>();
        popup_ = result["popup"].get<bool>();
        target_ = result["target"];
        targets_ = result["targets"];
        region_ = result["region"];
        selection_mode_ = result["selection_mode"];
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
        if (!result.is_object() || result.dump().size() > 262144 ||
            !result.contains("document_id") || result["document_id"] != document_id_ ||
            !result.contains("active") || !result["active"].is_boolean() ||
            !result.contains("popup") || !result["popup"].is_boolean() ||
            !result.contains("target") || !result.contains("viewport") || !result["viewport"].is_object() ||
            !result.contains("targets") || !valid_annotation_targets(result["targets"]) ||
            !result.contains("region") || (!result["region"].is_null() && !valid_annotation_bounds(result["region"], true)) ||
            !result.contains("selection_mode") || !(result["selection_mode"].is_null() ||
                result["selection_mode"] == "element" || result["selection_mode"] == "region")) return false;
        for (const auto& field : {"width", "height"}) {
            const auto& viewport = result["viewport"];
            if (!viewport.contains(field) || !viewport[field].is_number()) return false;
            const double value = viewport[field].get<double>();
            if (!std::isfinite(value) || value <= 0 || value > 10000000) return false;
        }
        if (result.contains("original") && !result["original"].is_boolean()) return false;
        if (result.contains("differences")) {
            if (!result["differences"].is_array() || result["differences"].size() > 64) return false;
            for (const auto& difference : result["differences"]) {
                if (!difference.is_object() || std::none_of(targets_.begin(), targets_.end(), [&](const Json& item) {
                        return difference.value("target_id", Json()) == item["id"];
                    }) ||
                    difference.value("property", Json()) != "text") return false;
                for (const auto& field : {"old_value", "new_value"}) {
                    if (!difference.contains(field) || !difference[field].is_string() ||
                        difference[field].get_ref<const std::string&>().size() > 16384) return false;
                }
                if (difference["old_value"] == difference["new_value"]) return false;
            }
        }
        for (const auto& target : result["targets"]) {
        if (target.contains("text_preview")) {
            const auto& text = target["text_preview"];
            if (!text.is_object() || !text.contains("value") || !text["value"].is_string() ||
                text["value"].get_ref<const std::string&>().size() > 16384 ||
                !text.contains("editable") || !text["editable"].is_boolean() ||
                !text.contains("error") || !text["error"].is_string()) return false;
        }
        }
        for (const auto& field : {"scroll_x", "scroll_y"}) {
            const auto& viewport = result["viewport"];
            if (!viewport.contains(field) || !viewport[field].is_number()) return false;
            const double value = viewport[field].get<double>();
            if (!std::isfinite(value) || std::abs(value) > 10000000) return false;
        }
        const auto& target = result["target"];
        if (result["active"] == false && (!target.is_null() || result["popup"] != false ||
            !result["targets"].empty() || !result["region"].is_null() || !result["selection_mode"].is_null())) return false;
        if (!result["selection_mode"].is_null() && result["popup"] != true) return false;
        if (target.is_null()) return result["popup"] == false || !result["region"].is_null();
        return std::find(result["targets"].begin(), result["targets"].end(), target) != result["targets"].end();
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
    Json targets_ = Json::array();
    Json region_ = nullptr;
    Json selection_mode_ = nullptr;
    Json viewport_ = nullptr;
    Json records_ = Json::array();
    Json pending_;
};

} // namespace acecode::desktop
