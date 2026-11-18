#pragma once

#include "page_annotation_runtime.hpp"
#include "page_annotation_text_runtime.hpp"
#include "page_annotation_targets.hpp"
#include "page_annotation_style_runtime.hpp"
#include "page_annotation_submission.hpp"
#include "page_annotation_navigation.hpp"
#include "page_annotation_requests.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode::desktop {

class PageAnnotationSession {
public:
    using Json = nlohmann::json;

    ~PageAnnotationSession() { requests.stop(); }

    const std::string& document_id() const { return document_id_; }
    AnnotationNavigationGuard navigation;
    AnnotationRequests requests;
    std::uint64_t revision() const { return revision_; }
    bool has_unsent_work() const {
        return !targets_.empty() || !region_.is_null() || !selection_mode_.is_null() || busy_ ||
            std::any_of(records_.begin(), records_.end(), [](const auto& record) {
                return record["status"] != "sent";
            });
    }

    bool observe_lifecycle(const Json& result) {
        if (!result.is_object() || result.value("document_id", Json()) != document_id_) return false;
        if (!result.contains("invalid_numbers")) return true;
        const auto& numbers = result["invalid_numbers"];
        if (!numbers.is_array() || numbers.size() > records_.size()) return false;
        Json matched = Json::array();
        for (const auto& number : numbers) {
            if (!number.is_number_unsigned() && !number.is_number_integer()) return false;
            const auto found = std::find_if(records_.begin(), records_.end(), [&](const auto& record) {
                return record["document_id"] == document_id_ && record["number"] == number;
            });
            if (found == records_.end() || std::find(matched.begin(), matched.end(), number) != matched.end()) return false;
            matched.push_back(number);
        }
        bool changed = false;
        for (auto& record : records_) {
            if (record["document_id"] == document_id_ && record["valid"] == true &&
                std::find(matched.begin(), matched.end(), record["number"]) != matched.end()) {
                record["valid"] = false;
                record["invalid_reason"] = "target_removed";
                changed = true;
            }
        }
        if (changed) ++revision_;
        return true;
    }

    Json snapshot() const {
        const auto pending_count = std::count_if(records_.begin(), records_.end(),
            [](const auto& record) { return record["status"] != "sent"; });
        return {{"document_id", document_id_}, {"active", active_},
                {"popup", popup_}, {"target", target_}, {"targets", targets_}, {"region", region_},
                {"selection_mode", selection_mode_}, {"viewport", viewport_}, {"records", records_},
                {"pending_count", pending_count}, {"original", original_}, {"differences", differences_}, {"revision", revision_}};
    }

    bool submission(const Json& command, std::string& expression, std::string& error) {
        if (!command.is_object() || command.dump().size() > 262144 ||
            !command.contains("action") || !command["action"].is_string() ||
            !command.contains("document_id") || !command["document_id"].is_string()) {
            error = "Invalid annotation submission";
            return false;
        }
        const bool acknowledge = command["action"] == "acknowledge";
        const bool release = command["action"] == "release";
        if (!acknowledge && !release && command["action"] != "capture" && command["action"] != "validate") {
            error = "Invalid submission action";
            return false;
        }
        for (const auto& field : command.items()) {
            if (field.key() != "action" && field.key() != "document_id" &&
                field.key() != "record_ids" && field.key() != "lease_id") {
                error = "Unsupported submission field";
                return false;
            }
        }
        if (release) {
            if (!command.contains("lease_id") || !command["lease_id"].is_string() ||
                !navigation.release(command["lease_id"].get<std::string>())) {
                error = "Stale annotation submission lease";
                return false;
            }
            return true;
        }
        if (command.contains("lease_id") && command["action"] != "validate") {
            error = "Unsupported submission lease";
            return false;
        }
        if (!acknowledge && navigation.blocked()) {
            error = "Annotation navigation is pending";
            return false;
        }
        Json selected;
        if (!annotation_submission_records(command, records_, selected, error)) return false;
        for (const auto& record : selected) {
            if (record["document_id"] != command["document_id"] ||
                (!acknowledge && (record["valid"] != true || record["status"] == "sent" ||
                    command["document_id"] != document_id_))) {
                error = "Annotation document or target is invalid";
                return false;
            }
        }
        if (acknowledge) {
            for (auto& record : records_) {
                for (const auto& accepted : selected) {
                    if (record["id"] == accepted["id"]) record["status"] = "sent";
                }
            }
            ++revision_;
        } else {
            if (command["action"] == "validate" && command.contains("lease_id")) {
                if (!command["lease_id"].is_string() ||
                    !navigation.acquire(command["lease_id"].get<std::string>())) {
                    error = "Annotation submission is already in progress";
                    return false;
                }
            }
            expression = annotation_submission_expression(document_id_, selected);
        }
        return true;
    }

    void replace_document() {
        poll_pending_ = false;
        ++poll_token_;
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
        differences_ = Json::array();
        document_id_.clear();
        for (auto& record : records_) {
            record["valid"] = false;
            record["invalid_reason"] = "document_replaced";
        }
        ++revision_;
        requests.cancel("Annotation document replaced");
        requests.restart();
    }

    bool prepare(const std::string& page_id, const Json& command,
                 std::string& expression, std::string& error) {
        if (navigation.blocked() || navigation.sending()) {
            error = "Annotation navigation is pending";
            return false;
        }
        if (!command.is_object() || command.dump().size() > 32768 ||
            !command.contains("action") || !command["action"].is_string()) {
            error = "Invalid annotation command";
            return false;
        }
        const auto action = command["action"].get<std::string>();
        if (action != "start" && action != "read" && action != "exit" &&
            action != "cancel" && action != "save" && action != "add_target" &&
            action != "add_region" && action != "select_target" && action != "select_region" &&
            action != "preview_text" && action != "preview_style" && action != "original") {
            error = "Unsupported annotation action";
            return false;
        }
        for (const auto& item : command.items()) {
            if (item.key() != "action" && item.key() != "document_id" &&
                !(action == "preview_style" && (item.key() == "styles" || item.key() == "ratio_lock" || item.key() == "target_id")) &&
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
        if (action == "save" && (!active_ || !popup_ || original_ || !selection_mode_.is_null() ||
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
        if (action == "preview_style" && (!active_ || !popup_ || original_ || !selection_mode_.is_null() || !target_.is_object() ||
            !command.contains("target_id") || command["target_id"] != target_["id"] ||
            !command.contains("styles") || !valid_styles(command["styles"]) ||
            (command.contains("ratio_lock") && !command["ratio_lock"].is_boolean()))) {
            error = "Invalid annotation style draft";
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
        if (action == "select_target") payload["target_id"] = command["target_id"];
        if (action == "preview_style") {
            payload["target_id"] = command["target_id"];
            payload["styles"] = command["styles"];
            if (command.contains("ratio_lock")) payload["ratio_lock"] = command["ratio_lock"];
        }
        expression = std::string(kPageAnnotationRuntime) + "(" + payload.dump() + "," + kPageAnnotationTextRuntime +
            "," + kPageAnnotationStyleRuntime + ")";
        busy_ = true;
        ++sequence_;
        return true;
    }

    std::uint64_t sequence() const { return sequence_; }
    void suspend() {
        ++sequence_;
        busy_ = false;
        poll_pending_ = false;
        ++poll_token_;
        requests.cancel("Annotation navigation started");
    }
    bool pollable() const {
        return !document_id_.empty() && !busy_ && !poll_pending_ && !navigation.blocked() && !navigation.sending();
    }
    std::uint64_t begin_poll() {
        if (!pollable()) return 0;
        poll_pending_ = true;
        return ++poll_token_;
    }
    bool finish_poll(std::uint64_t token) {
        if (!poll_pending_ || token != poll_token_) return false;
        poll_pending_ = false;
        return true;
    }

    bool complete(std::uint64_t sequence, const Json& result, std::string& error) {
        if (sequence != sequence_ || !busy_) {
            error = "Stale annotation callback";
            return false;
        }
        busy_ = false;
        if (result.is_object() && result.contains("error") && result["error"].is_string()) {
            const auto runtime_error = result["error"].get<std::string>();
            if (runtime_error == "replaced_document") {
                replace_document();
                return true;
            }
            if (runtime_error == "invalid_style_value") error = "Invalid style value";
            else if (runtime_error == "invalid_current_ratio") error = "Cannot lock zero or unresolved dimensions";
            else if (runtime_error == "original_view_active") error = "Leave original view before editing";
            else error = "Invalid or stale annotation result";
            return false;
        }
        if (!valid_result(result)) {
            error = "Invalid or stale annotation result";
            return false;
        }
        const auto action = pending_["action"].get<std::string>();
        if (!observe_lifecycle(result)) {
            error = "Invalid annotation lifecycle report";
            return false;
        }
        if ((action == "save" || action == "cancel" || action == "exit") &&
            (!result["target"].is_null() || result["popup"] != false ||
             !result["targets"].empty() || !result["region"].is_null() || !result["selection_mode"].is_null())) {
            error = "Annotation action did not complete";
            return false;
        }
        const bool changed = action == "save" || action == "original" || active_ != result["active"] ||
            popup_ != result["popup"] || target_ != result["target"] || viewport_ != result["viewport"] ||
            targets_ != result["targets"] || region_ != result["region"] || selection_mode_ != result["selection_mode"] ||
            differences_ != result.value("differences", Json::array());
        if (((action == "preview_style" || action == "preview_text") &&
             (result["target"].is_null() || result["target"]["id"] != target_["id"])) ||
            (action == "original" && result.value("original", false) != pending_["enabled"])) {
            error = "Annotation preview did not complete";
            return false;
        }
        if (action == "save") {
            auto saved_targets = targets_;
            for (auto& target : saved_targets) {
                target.erase("style_preview");
                target.erase("text_preview");
            }
            records_.push_back({
                {"id", document_id_ + ":annotation:" + std::to_string(next_number_)},
                {"number", next_number_++}, {"document_id", document_id_},
                {"page_id", page_id_},
                {"description", pending_["description"]}, {"targets", saved_targets},
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
        differences_ = targets_.empty() ? Json::array() : result.value("differences", Json::array());
        if (changed) ++revision_;
        return true;
    }

    void fail(std::uint64_t sequence) {
        if (sequence == sequence_) busy_ = false;
    }

private:
    static bool style_field(const std::string& field, bool difference = false) {
        if (field.empty() || field.find('|') != std::string::npos) return false;
        static const std::string fields = "|color|background-color|opacity|font-family|font-size|font-weight|"
            "width|height|letter-spacing|word-spacing|gap|row-gap|column-gap|padding|margin|"
            "padding-top|padding-right|padding-bottom|padding-left|margin-top|margin-right|margin-bottom|margin-left|"
            "border-radius|border-color|border-width|border-top-left-radius|border-top-right-radius|"
            "border-bottom-right-radius|border-bottom-left-radius|border-top-color|border-right-color|"
            "border-bottom-color|border-left-color|border-top-width|border-right-width|border-bottom-width|border-left-width|";
        if (fields.find("|" + field + "|") != std::string::npos) return true;
        return difference && (field == "border-top-style" || field == "border-right-style" ||
                              field == "border-bottom-style" || field == "border-left-style");
    }

    static bool valid_styles(const Json& styles) {
        if (!styles.is_object() || styles.size() > 48) return false;
        for (const auto& item : styles.items()) {
            if (!style_field(item.key()) || !item.value().is_string()) return false;
            const auto& value = item.value().get_ref<const std::string&>();
            if (value.size() > 256 || value.find_first_of(";{}@!<>\\\r\n") != std::string::npos ||
                value.find('\0') != std::string::npos) return false;
        }
        return true;
    }

    static bool valid_style_preview(const Json& preview) {
        if (!preview.is_object() || !preview.contains("ratio_locked") || !preview["ratio_locked"].is_boolean() ||
            !preview.contains("ratio") || !preview.contains("errors") || !preview["errors"].is_array() ||
            preview["errors"].size() > 48) return false;
        if (!preview["ratio"].is_null()) {
            if (!preview["ratio"].is_number()) return false;
            const double ratio = preview["ratio"].get<double>();
            if (!std::isfinite(ratio) || ratio <= 0) return false;
        }
        for (const auto& field : {"values", "styles"}) {
            if (!preview.contains(field) || !preview[field].is_object() || preview[field].size() > 48) return false;
            for (const auto& item : preview[field].items()) {
                if (!style_field(item.key(), true) || !item.value().is_string() ||
                    item.value().get_ref<const std::string&>().size() > 2048) return false;
            }
        }
        for (const auto& error : preview["errors"]) {
            if (!error.is_string() || error.get_ref<const std::string&>().size() > 128) return false;
        }
        return true;
    }

    bool valid_result(const Json& result) const {
        if (!result.is_object() || result.dump().size() > 4194304 ||
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
            if (!result["differences"].is_array() || result["differences"].size() > 3136) return false;
            std::vector<std::pair<std::string, std::string>> seen;
            for (const auto& difference : result["differences"]) {
                const auto& allowed_targets = pending_["action"] == "save" ? targets_ : result["targets"];
                if (!difference.is_object() || difference.size() != 4 ||
                    !difference.contains("property") || !difference["property"].is_string() ||
                    std::none_of(allowed_targets.begin(), allowed_targets.end(), [&](const Json& item) {
                        return difference.value("target_id", Json()) == item["id"];
                    }) ||
                    (difference["property"] != "text" && !style_field(difference["property"].get<std::string>(), true))) return false;
                const auto identity = std::make_pair(difference["target_id"].get<std::string>(), difference["property"].get<std::string>());
                if (std::find(seen.begin(), seen.end(), identity) != seen.end()) return false;
                seen.push_back(identity);
                for (const auto& field : {"old_value", "new_value"}) {
                    if (!difference.contains(field) || !difference[field].is_string() ||
                        difference[field].get_ref<const std::string&>().size() > 16384) return false;
                }
                if (difference["old_value"] == difference["new_value"]) return false;
            }
        }
        for (const auto& target : result["targets"]) {
            if (target.contains("style_preview") && !valid_style_preview(target["style_preview"])) return false;
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
    bool poll_pending_ = false;
    std::uint64_t poll_token_ = 0;
    Json viewport_ = nullptr;
    Json records_ = Json::array();
    Json differences_ = Json::array();
    Json pending_;
};

} // namespace acecode::desktop
