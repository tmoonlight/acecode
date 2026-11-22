#pragma once

#include "page_annotation_runtime.hpp"
#include "page_annotation_text_runtime.hpp"
#include "page_annotation_targets.hpp"
#include "page_annotation_style_runtime.hpp"
#include "page_annotation_submission.hpp"
#include "page_annotation_navigation.hpp"
#include "page_annotation_requests.hpp"
#include "session_annotation_store.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode::desktop {

class PageAnnotationSession {
public:
    using Json = nlohmann::json;

    // 记录归属会话（Q33），页签只转发。同一会话的所有页签共享一个 store；
    // 单页签场景（测试、未绑定会话的旧路径）允许自建一个。
    explicit PageAnnotationSession(std::shared_ptr<SessionAnnotationStore> store =
                                       std::make_shared<SessionAnnotationStore>())
        : store_(std::move(store)) {}

    ~PageAnnotationSession() { requests.stop(); }

    const std::shared_ptr<SessionAnnotationStore>& store() const { return store_; }

    // 同一会话的页签共享一个 store（Q33）。宿主在建页时注入；不注入则自建一个，
    // 单页签场景（测试、旧路径）行为与之前一致。
    void share_store(std::shared_ptr<SessionAnnotationStore> store) {
        if (store) store_ = std::move(store);
    }

    const std::string& document_id() const { return document_id_; }
    AnnotationNavigationGuard navigation;
    AnnotationRequests requests;
    std::uint64_t revision() const { return revision_; }
    bool has_unsent_work() const {
        const auto& records = store_->records(page_id_);
        return !targets_.empty() || !region_.is_null() || !selection_mode_.is_null() || busy_ ||
            std::any_of(records.begin(), records.end(), [](const auto& record) {
                return record["status"] != "sent";
            });
    }

    bool observe_lifecycle(const Json& result) {
        if (!result.is_object() || result.value("document_id", Json()) != document_id_) return false;
        if (!result.contains("invalid_numbers")) return true;
        const auto& numbers = result["invalid_numbers"];
        if (!numbers.is_array()) return false;
        if (numbers.empty()) return true;
        auto& records = store_->mutable_records(page_id_);
        if (numbers.size() > records.size()) return false;
        Json matched = Json::array();
        for (const auto& number : numbers) {
            if (!number.is_number_unsigned() && !number.is_number_integer()) return false;
            const auto found = std::find_if(records.begin(), records.end(), [&](const auto& record) {
                return record["document_id"] == document_id_ && record["number"] == number;
            });
            if (found == records.end() || std::find(matched.begin(), matched.end(), number) != matched.end()) return false;
            matched.push_back(number);
        }
        bool changed = false;
        for (auto& record : records) {
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
        const auto& records = store_->records(page_id_);
        const auto pending_count = std::count_if(records.begin(), records.end(),
            [](const auto& record) { return record["status"] != "sent"; });
        return {{"document_id", document_id_}, {"active", active_},
                {"popup", popup_}, {"target", target_}, {"targets", targets_}, {"region", region_},
                {"selection_mode", selection_mode_}, {"viewport", viewport_}, {"records", records},
                {"page_shot", store_->page_shot(page_id_)},
                {"pending_count", pending_count}, {"original", original_}, {"differences", differences_},
                {"page_background", page_background_}, {"markers_hidden", markers_hidden_},
                {"modal_suspended", modal_suspended_}, {"draft_description", draft_description_},
                {"editing_record_id", editing_record_id_},
                {"revision", revision_}};
    }

    bool submission(const Json& command, std::string& error) {
        if (!command.is_object() || command.dump().size() > 262144 ||
            !command.contains("action") || !command["action"].is_string() ||
            !command.contains("document_id") || !command["document_id"].is_string()) {
            error = "Invalid annotation submission";
            return false;
        }
        // 发送成功后按 id 删记录，不校验文档代与有效性：已发出去的记录不该
        // 因为「发送这一刻文档刚好换了」留在待发集合里复活。
        const bool remove = command["action"] == "remove";
        // 保存后异步补图：把裁剪好的缩略图挂到记录上、整屏图挂到页签上。
        const bool attach = command["action"] == "attach_screenshot";
        if (!remove && !attach) {
            error = "Invalid submission action";
            return false;
        }
        for (const auto& field : command.items()) {
            if (field.key() != "action" && field.key() != "document_id" &&
                field.key() != "record_ids" &&
                !(attach && (field.key() == "record_id" || field.key() == "thumbnail" ||
                             field.key() == "page_shot"))) {
                error = "Unsupported submission field";
                return false;
            }
        }
        if (attach) {
            // 图是保存时截的（D2/D10），发送阶段不再捕获。空串表示该条无图
            // （目标不在视口内 / 捕获失败），待发卡片退化为标签 + 文本片段。
            constexpr std::size_t kThumbnailLimit = 512 * 1024;
            constexpr std::size_t kPageShotLimit = 4 * 1024 * 1024;
            const auto valid_image = [](const Json& value, std::size_t limit) {
                if (!value.is_string()) return false;
                const auto& text = value.get_ref<const std::string&>();
                return text.empty() || (text.size() <= limit && text.rfind("data:image/", 0) == 0);
            };
            if (command.contains("page_shot")) {
                if (!valid_image(command["page_shot"], kPageShotLimit)) {
                    error = "Invalid annotation screenshot";
                    return false;
                }
                store_->set_page_shot(page_id_, command["page_shot"].get<std::string>());
            }
            if (command.contains("record_id")) {
                if (!command["record_id"].is_string() ||
                    !command.contains("thumbnail") ||
                    !valid_image(command["thumbnail"], kThumbnailLimit)) {
                    error = "Invalid annotation thumbnail";
                    return false;
                }
                const auto id = command["record_id"].get<std::string>();
                bool found = false;
                for (auto& record : store_->mutable_records(page_id_)) {
                    if (record["id"] != id) continue;
                    record["thumbnail"] = command["thumbnail"];
                    found = true;
                    break;
                }
                if (!found) {
                    error = "Unknown annotation";
                    return false;
                }
            }
            ++revision_;
            return true;
        }
        if (navigation.blocked()) {
            error = "Annotation navigation is pending";
            return false;
        }
        Json selected;
        if (!annotation_submission_records(command, store_->records(page_id_), selected, error)) return false;
        auto& records = store_->mutable_records(page_id_);
        for (const auto& gone : selected) {
            records.erase(std::remove_if(records.begin(), records.end(),
                [&gone](const auto& record) { return record["id"] == gone["id"]; }), records.end());
        }
        ++revision_;
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
        markers_hidden_ = false;
        modal_suspended_ = false;
        target_ = nullptr;
        targets_ = Json::array();
        region_ = nullptr;
        selection_mode_ = nullptr;
        differences_ = Json::array();
        page_background_.clear();
        editing_record_id_.clear();
        edit_locator_.clear();
        draft_description_.clear();
        document_id_.clear();
        for (auto& record : store_->mutable_records(page_id_)) {
            record["valid"] = false;
            record["invalid_reason"] = "document_replaced";
        }
        ++revision_;
        requests.cancel("Annotation document replaced");
        requests.restart();
    }

    bool prepare(const std::string& page_id, const Json& command,
                 std::string& expression, std::string& error) {
        if (navigation.blocked()) {
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
            action != "preview_text" && action != "preview_style" && action != "original" &&
            action != "hide_markers" && action != "set_modal_suspended" && action != "discard" &&
            action != "edit_record") {
            error = "Unsupported annotation action";
            return false;
        }
        for (const auto& item : command.items()) {
            if (item.key() != "action" && item.key() != "document_id" &&
                !(action == "preview_style" && (item.key() == "styles" || item.key() == "ratio_lock" || item.key() == "target_id")) &&
                !(action == "preview_text" && (item.key() == "text" || item.key() == "target_id")) &&
                !(action == "original" && item.key() == "enabled") &&
                !(action == "hide_markers" && item.key() == "enabled") &&
                !(action == "set_modal_suspended" && item.key() == "enabled") &&
                !(action == "save" && (item.key() == "description" || item.key() == "target_id" ||
                                      item.key() == "target_ids" || item.key() == "region")) &&
                !(action == "select_target" && item.key() == "target_id") &&
                !(action == "edit_record" && item.key() == "record_id")) {
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
        if (action == "edit_record") {
            if (!command.contains("record_id") || !command["record_id"].is_string()) {
                error = "Invalid annotation record";
                return false;
            }
            const auto record_id = command["record_id"].get<std::string>();
            const auto& records = store_->records(page_id_);
            const auto found = std::find_if(records.begin(), records.end(),
                [&](const auto& record) { return record["id"] == record_id; });
            if (found == records.end()) {
                error = "Unknown annotation";
                return false;
            }
            // 只有「当前文档里还有效」的记录能回到页面改（#217）。stale / orphan
            // 走的是另一条路径（只剩文字可改，由弹窗处理）。
            if ((*found)["document_id"] != document_id_ || (*found)["valid"] != true) {
                error = "Annotation target is invalid";
                return false;
            }
            const auto& targets = (*found)["targets"];
            if (!targets.is_array() || targets.empty() || !targets[0].contains("locator") ||
                !targets[0]["locator"].is_string() ||
                targets[0]["locator"].get_ref<const std::string&>().empty()) {
                error = "Annotation target is invalid";
                return false;
            }
            editing_record_id_ = record_id;
            edit_locator_ = targets[0]["locator"].get<std::string>();
            draft_description_ = (*found).value("description", std::string());
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
        if (action == "hide_markers" && (!command.contains("enabled") || !command["enabled"].is_boolean())) {
            error = "Invalid marker visibility request";
            return false;
        }
        if (action == "set_modal_suspended" &&
            (!command.contains("enabled") || !command["enabled"].is_boolean())) {
            error = "Invalid modal suspension request";
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
            payload["number"] = store_->next_number(page_id_);
        }
        if (action == "preview_text") {
            payload["target_id"] = command["target_id"];
            payload["text"] = command["text"];
        }
        if (action == "original") payload["enabled"] = command["enabled"];
        if (action == "hide_markers") payload["enabled"] = command["enabled"];
        if (action == "set_modal_suspended") payload["enabled"] = command["enabled"];
        if (action == "select_target") payload["target_id"] = command["target_id"];
        if (action == "edit_record") {
            payload["locator"] = edit_locator_;
        }
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
        return !document_id_.empty() && !busy_ && !poll_pending_ && !navigation.blocked();
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
        if ((action == "save" || action == "cancel" || action == "exit" || action == "discard") &&
            (!result["target"].is_null() || result["popup"] != false ||
             !result["targets"].empty() || !result["region"].is_null() || !result["selection_mode"].is_null())) {
            error = "Annotation action did not complete";
            return false;
        }
        const bool changed = action == "save" || action == "original" || action == "discard" ||
            action == "hide_markers" || action == "set_modal_suspended" || active_ != result["active"] ||
            popup_ != result["popup"] || target_ != result["target"] || viewport_ != result["viewport"] ||
            targets_ != result["targets"] || region_ != result["region"] || selection_mode_ != result["selection_mode"] ||
            differences_ != result.value("differences", Json::array()) ||
            page_background_ != result.value("page_background", std::string()) ||
            markers_hidden_ != result.value("markers_hidden", false) ||
            modal_suspended_ != result.value("modal_suspended", false);
        if (((action == "preview_style" || action == "preview_text") &&
             (result["target"].is_null() || result["target"]["id"] != target_["id"])) ||
            (action == "original" && result.value("original", false) != pending_["enabled"])) {
            error = "Annotation preview did not complete";
            return false;
        }
        if (action == "edit_record" && (result["popup"] != true || result["target"].is_null())) {
            error = "Annotation target is invalid";
            return false;
        }
        if (action == "save") {
            auto saved_targets = targets_;
            for (auto& target : saved_targets) {
                target.erase("style_preview");
                target.erase("text_preview");
            }
            if (!editing_record_id_.empty()) {
                // 铅笔路径：改的是同一条，编号与 id 不变（PRD §6.5）。
                for (auto& record : store_->mutable_records(page_id_)) {
                    if (record["id"] != editing_record_id_) continue;
                    record["description"] = pending_["description"];
                    record["targets"] = saved_targets;
                    record["differences"] = result.value("differences", Json::array());
                    record["region"] = region_;
                    record["document_id"] = document_id_;
                    record["valid"] = true;
                    break;
                }
                editing_record_id_.clear();
                draft_description_.clear();
                ++revision_;
            } else {
                store_->append(page_id_, {
                    {"document_id", document_id_},
                    {"description", pending_["description"]}, {"targets", saved_targets},
                    {"differences", result.value("differences", Json::array())}, {"region", region_},
                    {"status", "pending"}, {"valid", true},
                });
            }
        } else if (action == "discard") {
            store_->discard(page_id_);
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
        page_background_ = result.value("page_background", std::string());
        markers_hidden_ = result.value("markers_hidden", false);
        modal_suspended_ = result.value("modal_suspended", false);
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
        if (result.contains("markers_hidden") && !result["markers_hidden"].is_boolean()) return false;
        if (result.contains("modal_suspended") && !result["modal_suspended"].is_boolean()) return false;
        if (result.contains("page_background") &&
            (!result["page_background"].is_string() ||
             result["page_background"].get_ref<const std::string&>().size() > 128)) return false;
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

    std::shared_ptr<SessionAnnotationStore> store_;
    std::string document_id_;
    std::string page_id_;
    std::uint64_t epoch_ = 1;
    std::uint64_t sequence_ = 0;
    std::uint64_t revision_ = 0;
    bool busy_ = false;
    bool active_ = false;
    bool popup_ = false;
    bool original_ = false;
    bool markers_hidden_ = false;
    bool modal_suspended_ = false;
    Json target_ = nullptr;
    Json targets_ = Json::array();
    Json region_ = nullptr;
    Json selection_mode_ = nullptr;
    bool poll_pending_ = false;
    std::uint64_t poll_token_ = 0;
    Json viewport_ = nullptr;
    Json differences_ = Json::array();
    std::string page_background_;
    // 铅笔路径（#217）：正在改哪一条、该条保存时的 locator 与描述。
    std::string editing_record_id_;
    std::string edit_locator_;
    std::string draft_description_;
    Json pending_;
};

} // namespace acecode::desktop
