#include <gtest/gtest.h>
#include <vector>

#include "desktop/page_annotation_session.hpp"
#include "desktop/agent_browser_host.hpp"
#include "desktop/page_annotation_host_submission.hpp"

namespace acecode::desktop {
namespace {

using Json = nlohmann::json;

class PageAnnotationSessionTest : public testing::Test {
protected:
    Json command(const std::string& action) const {
        Json request{{"action", action}, {"document_id", session.snapshot()["document_id"]}};
        if (action == "save") {
            request["target_ids"] = annotation_target_ids(session.snapshot()["targets"]);
            request["region"] = session.snapshot()["region"];
        }
        return request;
    }

    Json result(bool active = true, bool popup = false, Json target = nullptr) const {
        return {{"document_id", session.snapshot()["document_id"]},
                {"active", active}, {"popup", popup}, {"target", target},
                {"original", false}, {"differences", Json::array()},
                {"targets", target.is_null() ? Json::array() : Json::array({target})},
                {"region", nullptr}, {"selection_mode", nullptr},
                {"viewport", {{"width", 800}, {"height", 600}, {"scroll_x", 0}, {"scroll_y", 0}}}};
    }

    Json target() const {
        return {{"id", "target-1"}, {"tag", "button"}, {"text", "Click"},
                {"locator", "button#target"},
                {"bounds", {{"x", 10}, {"y", 20}, {"width", 80}, {"height", 30}}}};
    }

    bool prepare(const Json& request) {
        return session.prepare("page-1", request, expression, error);
    }

    void start() {
        ASSERT_TRUE(prepare(command("start"))) << error;
        ASSERT_TRUE(session.complete(session.sequence(), result(), error)) << error;
    }

    void select() {
        ASSERT_TRUE(prepare(command("read"))) << error;
        ASSERT_TRUE(session.complete(session.sequence(), result(true, true, target()), error)) << error;
    }

    void save(const std::string& description) {
        auto request = command("save");
        request["description"] = description;
        request["target_id"] = "target-1";
        ASSERT_TRUE(prepare(request)) << error;
        EXPECT_EQ(expression.find(description), std::string::npos);
        ASSERT_TRUE(session.complete(session.sequence(), result(), error)) << error;
    }

    PageAnnotationSession session;
    std::string expression;
    std::string error;
};

TEST_F(PageAnnotationSessionTest, PassivePollIsBoundedAndNeverAdvancesCommandSequence) {
    EXPECT_EQ(session.begin_poll(), 0);
    start();
    const auto sequence = session.sequence();
    const auto first = session.begin_poll();
    EXPECT_NE(first, 0);
    EXPECT_EQ(session.sequence(), sequence);
    EXPECT_FALSE(session.pollable());
    EXPECT_EQ(session.begin_poll(), 0);
    EXPECT_FALSE(session.finish_poll(first + 1));
    EXPECT_TRUE(session.finish_poll(first));
    EXPECT_FALSE(session.finish_poll(first));
    const auto suspended = session.begin_poll();
    session.suspend();
    EXPECT_FALSE(session.finish_poll(suspended));
    const auto replaced = session.begin_poll();
    session.replace_document();
    start();
    const auto current = session.begin_poll();
    EXPECT_FALSE(session.finish_poll(replaced));
    EXPECT_FALSE(session.pollable());
    EXPECT_TRUE(session.finish_poll(current));
    EXPECT_TRUE(session.navigation.reserve());
    EXPECT_EQ(session.begin_poll(), 0);
}

TEST_F(PageAnnotationSessionTest, SnapshotRetainsPageBackgroundForOverlayCorners) {
    ASSERT_TRUE(prepare(command("start"))) << error;
    auto initial = result(true, false);
    initial["page_background"] = "rgb(18, 53, 30)";
    ASSERT_TRUE(session.complete(session.sequence(), initial, error)) << error;
    EXPECT_EQ(session.snapshot()["page_background"], "rgb(18, 53, 30)");

    session.replace_document();
    EXPECT_EQ(session.snapshot()["page_background"], "");
}

TEST(AnnotationRequests, DeadlineBoundShutdownAndReentrantCompletion) {
    const auto now = AnnotationRequests::Clock::time_point{};
    int completions = 0;
    std::shared_ptr<PendingProxyCall> late;
    {
        AnnotationRequests requests;
        for (int index = 0; index < 32; ++index) {
            ASSERT_TRUE(requests.add([&](Json result) {
                ++completions;
                EXPECT_EQ(result["ok"], false);
            }, now));
        }
        EXPECT_FALSE(requests.add([&](Json) { ++completions; }, now));
        EXPECT_EQ(completions, 1);
        requests.expire(now + std::chrono::seconds(14));
        EXPECT_EQ(completions, 1);
        requests.expire(now + std::chrono::seconds(15));
        EXPECT_EQ(completions, 33);
        ASSERT_TRUE(requests.add([&](Json) {
            ++completions;
            late = requests.add([&](Json) { ++completions; }, now);
        }, now));
        requests.cancel("replacement");
        ASSERT_TRUE(late);
        EXPECT_FALSE(late->completed());
    }
    EXPECT_TRUE(late->completed());
    EXPECT_EQ(completions, 35);
    late->finish({{"ok", true}});
    EXPECT_EQ(completions, 35);
}

TEST(AnnotationRequests, RetirementPrecedesResponseAndLateRegistrationCannotLeakResource) {
    AnnotationRequests requests;
    int retired = 0;
    int completions = 0;
    std::shared_ptr<PendingProxyCall> next;
    const auto first = requests.add([&](Json) {
        EXPECT_EQ(retired, 1);
        ++completions;
        next = requests.add([&](Json) { ++completions; });
        requests.cleanup(next)([&] { ++retired; });
    });
    const auto register_cleanup = requests.cleanup(first);
    auto resource = std::make_shared<int>(0);
    const std::weak_ptr<int> weak_resource = resource;
    register_cleanup([resource, &retired] { ++retired; });
    resource.reset();
    EXPECT_FALSE(weak_resource.expired());
    requests.cancel("replaced");
    EXPECT_TRUE(weak_resource.expired());
    EXPECT_EQ(completions, 1);
    ASSERT_TRUE(next);
    EXPECT_FALSE(next->completed());
    register_cleanup([&] { ++retired; });
    EXPECT_EQ(retired, 2);
    first->finish({{"ok", true}});
    EXPECT_EQ(completions, 1);
    EXPECT_FALSE(next->completed());
    requests.stop();
    EXPECT_EQ(retired, 3);
    EXPECT_EQ(completions, 2);
}

TEST_F(PageAnnotationSessionTest, SaveCreatesImmutableDescriptionOnlyPendingRecordWithoutConversation) {
    start();
    select();
    save("Make this easier to find");
    const auto record = session.snapshot()["records"][0];
    EXPECT_EQ(record["number"], 1);
    EXPECT_EQ(record["page_id"], "page-1");
    EXPECT_EQ(record["document_id"], session.snapshot()["document_id"]);
    EXPECT_EQ(record["status"], "pending");
    EXPECT_EQ(record["targets"], Json::array({target()}));
    EXPECT_EQ(record["differences"], Json::array());
    EXPECT_TRUE(record["region"].is_null());
    EXPECT_EQ(session.snapshot()["pending_count"], 1);
    auto returned = session.snapshot();
    returned["records"][0]["description"] = "Changed outside owner";
    EXPECT_EQ(session.snapshot()["records"][0], record);
    select();
    save("Second independent request");
    EXPECT_EQ(session.snapshot()["records"][0], record);
    EXPECT_EQ(session.snapshot()["records"][1]["number"], 2);
    EXPECT_NE(session.snapshot()["records"][1]["id"], record["id"]);
}

TEST_F(PageAnnotationSessionTest, CancelAndExitPreserveSavedRecordsAndDoNotAllocateNumbers) {
    start();
    select();
    save("Saved request");
    const auto records = session.snapshot()["records"];
    select();
    ASSERT_TRUE(prepare(command("cancel")));
    ASSERT_TRUE(session.complete(session.sequence(), result(), error));
    EXPECT_EQ(session.snapshot()["records"], records);
    ASSERT_TRUE(prepare(command("exit")));
    ASSERT_TRUE(session.complete(session.sequence(), result(false), error));
    EXPECT_EQ(session.snapshot()["records"], records);
    EXPECT_EQ(session.snapshot()["active"], false);
    start();
    select();
    save("After reopening");
    EXPECT_EQ(session.snapshot()["records"][1]["number"], 2);
}

TEST_F(PageAnnotationSessionTest, HideMarkersAndDiscardAllKeepSessionStateExplicit) {
    start();
    select();
    save("__annotation_description_sentinel__");
    ASSERT_EQ(session.snapshot()["pending_count"], 1);

    auto hidden = command("hide_markers");
    hidden["enabled"] = true;
    ASSERT_TRUE(prepare(hidden)) << error;
    auto hidden_result = result();
    hidden_result["markers_hidden"] = true;
    ASSERT_TRUE(session.complete(session.sequence(), hidden_result, error)) << error;
    EXPECT_EQ(session.snapshot()["markers_hidden"], true);
    EXPECT_EQ(session.snapshot()["pending_count"], 1);

    ASSERT_TRUE(prepare(command("discard"))) << error;
    ASSERT_TRUE(session.complete(session.sequence(), result(), error)) << error;
    EXPECT_EQ(session.snapshot()["markers_hidden"], false);
    EXPECT_TRUE(session.snapshot()["records"].empty());
    EXPECT_EQ(session.snapshot()["pending_count"], 0);
    EXPECT_FALSE(session.has_unsent_work());
}

TEST_F(PageAnnotationSessionTest, ModalSuspensionIsValidatedAndTracked) {
    start();
    select();

    // set_modal_suspended requires a boolean `enabled` field.
    auto missing = command("set_modal_suspended");
    EXPECT_FALSE(prepare(missing));
    auto invalid = command("set_modal_suspended");
    invalid["enabled"] = "yes";
    EXPECT_FALSE(prepare(invalid));

    auto suspend = command("set_modal_suspended");
    suspend["enabled"] = true;
    ASSERT_TRUE(prepare(suspend)) << error;
    EXPECT_NE(expression.find("set_modal_suspended"), std::string::npos);
    EXPECT_NE(expression.find("\"enabled\":true"), std::string::npos);
    auto suspended_result = result();
    suspended_result["modal_suspended"] = true;
    ASSERT_TRUE(session.complete(session.sequence(), suspended_result, error)) << error;
    EXPECT_EQ(session.snapshot()["modal_suspended"], true);

    auto bad_result_request = command("set_modal_suspended");
    bad_result_request["enabled"] = false;
    ASSERT_TRUE(prepare(bad_result_request)) << error;
    auto non_boolean = result();
    non_boolean["modal_suspended"] = "true";
    EXPECT_FALSE(session.complete(session.sequence(), non_boolean, error));

    auto resume = command("set_modal_suspended");
    resume["enabled"] = false;
    ASSERT_TRUE(prepare(resume)) << error;
    auto resumed_result = result();
    resumed_result["modal_suspended"] = false;
    ASSERT_TRUE(session.complete(session.sequence(), resumed_result, error)) << error;
    EXPECT_EQ(session.snapshot()["modal_suspended"], false);

    session.replace_document();
    EXPECT_EQ(session.snapshot()["modal_suspended"], false);
}

TEST_F(PageAnnotationSessionTest, TextCommandsBindTargetAndSaveOnlyAcknowledgedActualDifferences) {
    start();
    select();
    auto request = command("preview_text");
    request["target_id"] = "wrong";
    request["text"] = "New label";
    EXPECT_FALSE(prepare(request));
    request["target_id"] = "target-1";
    request["text"] = std::string(16385, 'x');
    EXPECT_FALSE(prepare(request));
    request["text"] = "New label";
    ASSERT_TRUE(prepare(request)) << error;
    EXPECT_NE(expression.find("__acecodePageAnnotationsV1"), std::string::npos);
    EXPECT_NE(expression.find("New label"), std::string::npos);
    auto selected = target();
    selected["text_preview"] = {{"value", "New label"}, {"editable", true}, {"error", ""}};
    ASSERT_TRUE(session.complete(session.sequence(), result(true, true, selected), error)) << error;
    request = command("save");
    request["target_id"] = "target-1";
    request["description"] = "";
    ASSERT_TRUE(prepare(request)) << error;
    auto saved = result();
    saved["differences"] = Json::array({{{"target_id", "target-1"}, {"property", "text"},
                                         {"old_value", "Click"}, {"new_value", "New label"}}});
    ASSERT_TRUE(session.complete(session.sequence(), saved, error)) << error;
    EXPECT_EQ(session.snapshot()["records"][0]["differences"], saved["differences"]);
    EXPECT_EQ(session.snapshot()["records"][0]["description"], "");
    const auto records = session.snapshot()["records"];
    request = command("original");
    request["enabled"] = "true";
    EXPECT_FALSE(prepare(request));
    request["enabled"] = true;
    ASSERT_TRUE(prepare(request));
    auto original = result();
    original["original"] = true;
    ASSERT_TRUE(session.complete(session.sequence(), original, error));
    EXPECT_EQ(session.snapshot()["original"], true);
    EXPECT_EQ(session.snapshot()["records"], records);
}

TEST_F(PageAnnotationSessionTest, RejectsMalformedTextDifferencesWithoutCreatingRecord) {
    start();
    select();
    auto request = command("save");
    request["target_id"] = "target-1";
    request["description"] = "";
    const Json valid{{"target_id", "target-1"}, {"property", "text"},
                     {"old_value", "Click"}, {"new_value", "Changed"}};
    for (const auto& field : {"target_id", "property", "old_value", "new_value"}) {
        ASSERT_TRUE(prepare(request));
        auto saved = result();
        auto malformed = valid;
        malformed[field] = 42;
        saved["differences"] = Json::array({malformed});
        EXPECT_FALSE(session.complete(session.sequence(), saved, error));
        EXPECT_EQ(session.snapshot()["pending_count"], 0);
    }
    ASSERT_TRUE(prepare(request));
    auto saved = result();
    auto unchanged = valid;
    unchanged["new_value"] = unchanged["old_value"];
    saved["differences"] = Json::array({unchanged});
    EXPECT_FALSE(session.complete(session.sequence(), saved, error));
    EXPECT_EQ(session.snapshot()["pending_count"], 0);
}

TEST_F(PageAnnotationSessionTest, RejectsArbitraryCapabilitiesAndRecordMutation) {
    start();
    for (const auto& action : {"send", "evaluate", "delete", "edit", "write_file", "capture"}) {
        EXPECT_FALSE(prepare(command(action))) << action;
    }
    auto request = command("read");
    request["script"] = "alert(1)";
    EXPECT_FALSE(prepare(request));
    request = command("read");
    request["description"] = "Unexpected field";
    EXPECT_FALSE(prepare(request));
    request = command("cancel");
    request["target_id"] = "Unexpected target";
    EXPECT_FALSE(prepare(request));
    request = command("save");
    request["description"] = "Not selected";
    request["target_id"] = "target-1";
    EXPECT_FALSE(prepare(request));
    select();
    request["target_id"] = "another-target";
    EXPECT_FALSE(prepare(request));
    request["target_id"] = "target-1";
    request["description"] = std::string(16385, 'x');
    EXPECT_FALSE(prepare(request));
}

TEST_F(PageAnnotationSessionTest, DocumentReplacementRetainsInvalidRecordsAndRejectsLateCallbacks) {
    start();
    select();
    save("Original document");
    auto old_command = command("read");
    const auto old_result = result(true, true, target());
    ASSERT_TRUE(prepare(old_command));
    const auto old_sequence = session.sequence();
    session.replace_document();
    EXPECT_FALSE(session.complete(old_sequence, old_result, error));
    EXPECT_FALSE(prepare(old_command));
    EXPECT_EQ(session.snapshot()["records"][0]["valid"], false);
    EXPECT_EQ(session.snapshot()["active"], false);
    start();
    EXPECT_NE(session.snapshot()["document_id"], old_result["document_id"]);
    select();
    save("Replacement document");
    EXPECT_EQ(session.snapshot()["records"][1]["number"], 2);
}

TEST_F(PageAnnotationSessionTest, StaleRuntimeDocumentResetsSessionAndAllowsStaleStartSnapshot) {
    start();
    select();
    save("Retained after reload");
    const std::string retired_document = session.document_id();
    ASSERT_TRUE(prepare(command("read"))) << error;

    const Json stale{{"error", "stale_document"}};
    ASSERT_TRUE(stale.is_object());
    ASSERT_TRUE(stale["error"].is_string());
    EXPECT_TRUE(session.complete(session.sequence(), stale, error)) << error;
    EXPECT_TRUE(session.document_id().empty());
    EXPECT_EQ(session.snapshot()["active"], false);
    EXPECT_EQ(session.snapshot()["records"][0]["valid"], false);
    EXPECT_EQ(session.snapshot()["records"][0]["invalid_reason"], "document_replaced");

    // The WebUI may still hold the pre-reload snapshot when the user starts
    // again. Start must establish a fresh generation instead of rejecting it.
    ASSERT_TRUE(prepare({{"action", "start"}, {"document_id", retired_document}})) << error;
    EXPECT_NE(session.document_id(), retired_document);
    ASSERT_TRUE(session.complete(session.sequence(), result(), error)) << error;
    EXPECT_EQ(session.snapshot()["active"], true);
}

TEST_F(PageAnnotationSessionTest, DocumentLossHelperRecognizesStaleRuntime) {
    start();
    const Json stale{{"error", "stale_document"}};
    EXPECT_TRUE(session.recover_document_loss(stale));
    EXPECT_TRUE(session.document_id().empty());
}

TEST_F(PageAnnotationSessionTest, FailedStartValidationDoesNotPoisonDocumentIdentity) {
    start();
    const std::string retired_document = session.document_id();
    session.replace_document();

    const auto sequence = session.sequence();
    EXPECT_FALSE(prepare({{"action", "start"}}));
    EXPECT_TRUE(session.document_id().empty());
    EXPECT_EQ(session.sequence(), sequence);

    ASSERT_TRUE(prepare({{"action", "start"}, {"document_id", retired_document}})) << error;
    EXPECT_FALSE(session.document_id().empty());
    EXPECT_NE(session.document_id(), retired_document);
    ASSERT_TRUE(session.complete(session.sequence(), result(), error)) << error;
    EXPECT_EQ(session.snapshot()["active"], true);
}

TEST_F(PageAnnotationSessionTest, SerializesCommandsAndDoesNotLetOldFailureReleaseNewOperation) {
    start();
    ASSERT_TRUE(prepare(command("read")));
    const auto old_sequence = session.sequence();
    EXPECT_FALSE(prepare(command("exit")));
    session.fail(old_sequence);
    ASSERT_TRUE(prepare(command("read")));
    session.fail(old_sequence);
    EXPECT_FALSE(prepare(command("exit")));
    EXPECT_FALSE(session.complete(old_sequence, result(), error));
    EXPECT_TRUE(session.complete(session.sequence(), result(), error));
}

TEST_F(PageAnnotationSessionTest, MalformedOrUnacknowledgedSaveCannotCreateRecord) {
    start();
    select();
    auto request = command("save");
    request["description"] = "Never accepted";
    request["target_id"] = "target-1";
    ASSERT_TRUE(prepare(request));
    EXPECT_FALSE(session.complete(session.sequence(), result(true, true, target()), error));
    EXPECT_EQ(session.snapshot()["pending_count"], 0);
    for (const auto& malformed : {Json(nullptr), Json::array(), Json{{"error", "stale_target"}}}) {
        ASSERT_TRUE(prepare(command("read")));
        EXPECT_FALSE(session.complete(session.sequence(), malformed, error));
    }
    auto malformed = result(true, true, target());
    malformed["target"]["bounds"]["width"] = -1;
    ASSERT_TRUE(prepare(command("read")));
    EXPECT_FALSE(session.complete(session.sequence(), malformed, error));
    malformed = result(false, true, target());
    ASSERT_TRUE(prepare(command("read")));
    EXPECT_FALSE(session.complete(session.sequence(), malformed, error));
    malformed = result();
    malformed["viewport"]["width"] = "800";
    ASSERT_TRUE(prepare(command("read")));
    EXPECT_FALSE(session.complete(session.sequence(), malformed, error));
}

TEST_F(PageAnnotationSessionTest, MultipleTargetsAndReplacementRegionSaveOneDescriptionAndNumber) {
    start();
    select();
    auto second = target();
    second["id"] = "target-2";
    second["tag"] = "section";
    second["differences"] = Json::array();
    auto draft = result(true, true, second);
    draft["targets"] = Json::array({target(), second});
    ASSERT_TRUE(prepare(command("add_target"))) << error;
    ASSERT_TRUE(session.complete(session.sequence(), draft, error)) << error;
    auto select_first = command("select_target");
    select_first["target_id"] = "target-1";
    draft["target"] = target();
    ASSERT_TRUE(prepare(select_first)) << error;
    ASSERT_TRUE(session.complete(session.sequence(), draft, error)) << error;
    ASSERT_TRUE(prepare(command("add_region"))) << error;
    draft["region"] = {{"x", 10}, {"y", 520}, {"width", 100}, {"height", 80}};
    draft["target"] = nullptr;
    ASSERT_TRUE(session.complete(session.sequence(), draft, error)) << error;
    ASSERT_TRUE(prepare(command("add_region"))) << error;
    draft["region"] = {{"x", 30}, {"y", 900}, {"width", 120}, {"height", 40}};
    ASSERT_TRUE(session.complete(session.sequence(), draft, error)) << error;
    auto request = command("save");
    request["target_id"] = nullptr;
    request["description"] = "One shared description";
    ASSERT_TRUE(prepare(request)) << error;
    EXPECT_EQ(expression.find("One shared description"), std::string::npos);
    ASSERT_TRUE(session.complete(session.sequence(), result(), error)) << error;
    const auto saved = session.snapshot()["records"];
    ASSERT_EQ(saved.size(), 1);
    EXPECT_EQ(saved[0]["number"], 1);
    EXPECT_EQ(saved[0]["description"], "One shared description");
    EXPECT_EQ(saved[0]["targets"], draft["targets"]);
    EXPECT_EQ(saved[0]["region"], draft["region"]);
    EXPECT_EQ(saved[0]["differences"], Json::array());
    select();
    ASSERT_TRUE(prepare(command("cancel")));
    ASSERT_TRUE(session.complete(session.sequence(), result(), error));
    EXPECT_EQ(session.snapshot()["records"], saved);
    EXPECT_TRUE(session.snapshot()["targets"].empty());
    EXPECT_TRUE(session.snapshot()["region"].is_null());
}

TEST_F(PageAnnotationSessionTest, RejectsMalformedTargetArraysRegionsAndForeignIdentity) {
    start();
    select();
    for (const auto& targets : {Json(nullptr), Json::object(), Json::array({target(), target()}),
                               Json::array({Json("target-1")}), Json(std::vector<Json>(65, target()))}) {
        auto malformed = result(true, true, target());
        malformed["targets"] = targets;
        ASSERT_TRUE(prepare(command("read")));
        EXPECT_FALSE(session.complete(session.sequence(), malformed, error));
    }
    for (const auto& region : {Json::array(), Json{{"x", -1}, {"y", 2}, {"width", 3}, {"height", 4}},
         Json{{"x", 1}, {"y", 2}, {"width", 0}, {"height", 4}},
         Json{{"x", 1}, {"y", 2}, {"width", 3}, {"height", "4"}},
         Json{{"x", 1}, {"y", 2}, {"width", 3}, {"height", 4}, {"script", "send()"}},
         Json{{"x", 1}, {"y", 10000001}, {"width", 3}, {"height", 4}}}) {
        auto malformed = result(true, true, target());
        malformed["region"] = region;
        ASSERT_TRUE(prepare(command("read")));
        EXPECT_FALSE(session.complete(session.sequence(), malformed, error));
    }
    auto request = command("select_target");
    request["target_id"] = "target-2";
    EXPECT_FALSE(prepare(request));
    EXPECT_FALSE(prepare(command("select_region")));
    EXPECT_FALSE(session.prepare("page-2", command("read"), expression, error));
    request = command("save");
    request["target_id"] = "target-1";
    request["description"] = "Stale membership";
    request["target_ids"] = Json::array({"target-1", "target-1"});
    EXPECT_FALSE(prepare(request));
    request["target_ids"] = Json::array({"target-1"});
    request["region"] = {{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}};
    EXPECT_FALSE(prepare(request));
    EXPECT_TRUE(session.snapshot()["records"].empty());
}

TEST_F(PageAnnotationSessionTest, StyleCommandsAreRestrictedToTheActiveExactTargetAndDocument) {
    start();
    auto request = command("preview_style");
    request["target_id"] = "target-1";
    request["styles"] = {{"color", "blue"}};
    EXPECT_FALSE(prepare(request));
    select();
    request["target_id"] = "another-target";
    EXPECT_FALSE(prepare(request));
    request["target_id"] = "target-1";
    for (const auto& styles : {Json{{"position", "fixed"}}, Json{{"color", "red; width:999px"}},
                              Json{{"color", 1}}, Json{{"width", std::string(257, 'x')}},
                              Json{{"color|background-color", "red"}}, Json::array()}) {
        request["styles"] = styles;
        EXPECT_FALSE(prepare(request)) << styles;
    }
    request["styles"] = {{"width", "200px"}};
    request["ratio_lock"] = "true";
    EXPECT_FALSE(prepare(request));
    request["ratio_lock"] = true;
    ASSERT_TRUE(prepare(request)) << error;
    EXPECT_NE(expression.find("\"ratio_lock\":true"), std::string::npos);
    EXPECT_NE(expression.find("CSSStyleSheet"), std::string::npos);
    auto stale = result(true, true, target());
    stale["target"]["id"] = "another-target";
    EXPECT_FALSE(session.complete(session.sequence(), stale, error));
    ASSERT_TRUE(prepare(request));
    ASSERT_TRUE(session.complete(session.sequence(), result(true, true, target()), error));
}

TEST_F(PageAnnotationSessionTest, SavesAcknowledgedActualStyleDifferencesAsImmutableRecords) {
    start();
    select();
    const Json differences = Json::array({{{"target_id", "target-1"}, {"property", "color"},
        {"old_value", "rgb(255, 0, 0)"}, {"new_value", "rgb(0, 0, 255)"}}});
    auto request = command("preview_style");
    request["target_id"] = "target-1";
    request["styles"] = {{"color", "blue"}};
    ASSERT_TRUE(prepare(request));
    auto preview = result(true, true, target());
    preview["differences"] = differences;
    ASSERT_TRUE(session.complete(session.sequence(), preview, error));
    EXPECT_EQ(session.snapshot()["differences"], differences);
    request = command("save");
    request["description"] = "";
    request["target_id"] = "target-1";
    ASSERT_TRUE(prepare(request));
    auto saved = result();
    saved["differences"] = differences;
    ASSERT_TRUE(session.complete(session.sequence(), saved, error));
    EXPECT_EQ(session.snapshot()["records"][0]["differences"], differences);
    EXPECT_EQ(session.snapshot()["differences"], Json::array());
    const auto record = session.snapshot()["records"][0];
    select();
    ASSERT_TRUE(prepare(command("cancel")));
    ASSERT_TRUE(session.complete(session.sequence(), result(), error));
    EXPECT_EQ(session.snapshot()["records"][0], record);
}

TEST_F(PageAnnotationSessionTest, RejectsNoOpUnknownAndForeignDifferencesWithoutAllocatingPins) {
    start();
    select();
    auto request = command("save");
    request["description"] = "";
    request["target_id"] = "target-1";
    const Json difference{{"target_id", "target-1"}, {"property", "width"},
                          {"old_value", "100px"}, {"new_value", "200px"}};
    for (const auto& alteration : {Json{{"target_id", "foreign"}}, Json{{"property", "script"}},
                                   Json{{"new_value", "100px"}}, Json{{"old_value", 100}}}) {
        auto malformed = result();
        auto changed = difference;
        changed.update(alteration);
        malformed["differences"] = Json::array({changed});
        ASSERT_TRUE(prepare(request));
        EXPECT_FALSE(session.complete(session.sequence(), malformed, error));
        EXPECT_EQ(session.snapshot()["pending_count"], 0);
    }
    save("First real record");
    EXPECT_EQ(session.snapshot()["records"][0]["number"], 1);
}

TEST_F(PageAnnotationSessionTest, OriginalToggleIsIndependentOfModeAndRetainsRecordsAndPins) {
    start();
    select();
    save("Saved");
    const auto records = session.snapshot()["records"];
    ASSERT_TRUE(prepare(command("exit")));
    ASSERT_TRUE(session.complete(session.sequence(), result(false), error));
    auto request = command("original");
    EXPECT_FALSE(prepare(request));
    request["enabled"] = true;
    ASSERT_TRUE(prepare(request));
    auto comparison = result(false);
    comparison["original"] = true;
    ASSERT_TRUE(session.complete(session.sequence(), comparison, error));
    EXPECT_EQ(session.snapshot()["original"], true);
    EXPECT_EQ(session.snapshot()["records"], records);
    request["enabled"] = false;
    ASSERT_TRUE(prepare(request));
    EXPECT_FALSE(session.complete(session.sequence(), comparison, error));
    ASSERT_TRUE(prepare(request));
    ASSERT_TRUE(session.complete(session.sequence(), result(false), error));
    EXPECT_EQ(session.snapshot()["original"], false);
    EXPECT_EQ(session.snapshot()["records"], records);
    session.replace_document();
    EXPECT_EQ(session.snapshot()["original"], false);
    EXPECT_EQ(session.snapshot()["differences"], Json::array());
}

TEST_F(PageAnnotationSessionTest, RemoveErasesExactlyTheNamedRecordsAndSurvivesDocumentReplacement) {
    start();
    select();
    save("First");
    const auto first = session.snapshot()["records"][0];
    select();
    save("Second");
    auto request = command("remove");
    request["record_ids"] = Json::array({first["id"]});
    std::vector<int> removed;
    ASSERT_TRUE(session.submission(request, error, &removed)) << error;
    EXPECT_EQ(removed, std::vector<int>{1});
    auto snapshot = session.snapshot();
    EXPECT_EQ(snapshot["records"].size(), 1);
    EXPECT_EQ(snapshot["records"][0]["description"], "Second");
    // 发送那一刻文档换了也不影响删除：已发出去的记录不能留在待发集合里复活。
    session.replace_document();
    auto late = command("remove");
    late["record_ids"] = Json::array({snapshot["records"][0]["id"]});
    ASSERT_TRUE(session.submission(late, error, &removed)) << error;
    EXPECT_EQ(removed, std::vector<int>{2});
    EXPECT_EQ(session.snapshot()["records"], Json::array());
    EXPECT_EQ(session.snapshot()["pending_count"], 0);
}

TEST_F(PageAnnotationSessionTest, RemovePinsRuntimeCommandCarriesDocumentIdentity) {
    const auto payload = page_annotation_remove_pins_payload("page:document:1", {1, 3});
    EXPECT_EQ(payload["action"], "remove_pins");
    EXPECT_EQ(payload["document_id"], "page:document:1");
    EXPECT_EQ(payload["numbers"], Json::array({1, 3}));
}

TEST_F(PageAnnotationSessionTest, HostSubmissionReportsRemovedNumbersForPinCleanup) {
    start();
    select();
    save("First");
    select();
    save("Second");
    const auto records = session.snapshot()["records"];
    Json ids = Json::array();
    for (const auto& record : records) ids.push_back(record["id"]);
    auto request = command("remove");
    request["record_ids"] = ids;

    std::vector<int> removed_numbers;
    std::string updated;
    std::string completion;
    ASSERT_TRUE(annotation_host_submission(
        session, "page-1", request,
        [&](std::string result) { completion = std::move(result); },
        [&](std::string snapshot) { updated = std::move(snapshot); },
        [&](const std::vector<int>& numbers) { removed_numbers = numbers; }));
    const auto response = Json::parse(completion);
    ASSERT_EQ(response["ok"], true) << completion;
    EXPECT_EQ(removed_numbers, std::vector<int>({1, 2}));
    ASSERT_FALSE(updated.empty());
    EXPECT_TRUE(Json::parse(updated)["records"].empty());
    EXPECT_TRUE(response["annotations"]["records"].empty());
}

TEST_F(PageAnnotationSessionTest, SubmissionRejectsUnknownMembershipAndRetiredLeaseActions) {
    start();
    select();
    save("Retain me");
    const auto before = session.snapshot();
    auto request = command("remove");
    const auto id = before["records"][0]["id"];
    for (const auto& ids : {Json::array(), Json::array({"absent"}), Json::array({id, id}), Json::array({id, "absent"})}) {
        request["record_ids"] = ids;
        EXPECT_FALSE(session.submission(request, error));
        EXPECT_EQ(session.snapshot(), before);
    }
    request["record_ids"] = Json::array({id});
    request["text"] = "Untrusted model input";
    EXPECT_FALSE(session.submission(request, error));
    request.erase("text");
    // 发送链路只剩 remove / attach_screenshot：批次的 capture / validate /
    // acknowledge / release 已删除，不再被接受。
    for (const auto* retired : {"capture", "validate", "acknowledge", "release"}) {
        auto stale = request;
        stale["action"] = retired;
        EXPECT_FALSE(session.submission(stale, error)) << retired;
    }
    request["document_id"] = "other-document";
    EXPECT_TRUE(session.submission(request, error)) << error;
    EXPECT_EQ(session.snapshot()["records"], Json::array());
}

TEST_F(PageAnnotationSessionTest, LifecycleReportsAreAtomicDocumentBoundAndSticky) {
    start(); select(); save("Retain");
    const auto before = session.snapshot();
    for (const auto& numbers : {Json::array({0}), Json::array({1, 1}), Json::array({"1"}), Json::array({1, 2})}) {
        EXPECT_FALSE(session.observe_lifecycle({{"document_id", session.document_id()}, {"invalid_numbers", numbers}}));
        EXPECT_EQ(session.snapshot(), before);
    }
    EXPECT_FALSE(session.observe_lifecycle({{"document_id", "wrong"}, {"invalid_numbers", Json::array({1})}}));
    EXPECT_TRUE(session.observe_lifecycle({{"document_id", session.document_id()}, {"invalid_numbers", Json::array({1})}}));
    EXPECT_EQ(session.snapshot()["records"][0]["valid"], false);
    EXPECT_EQ(session.snapshot()["records"][0]["invalid_reason"], "target_removed");
    EXPECT_TRUE(session.observe_lifecycle({{"document_id", session.document_id()}, {"invalid_numbers", Json::array()}}));
    EXPECT_EQ(session.snapshot()["records"][0]["valid"], false);
}

TEST_F(PageAnnotationSessionTest, NavigationReservationBlocksRemovalWhileRecordsOutliveTheDocument) {
    start(); select(); save("Retain");
    auto request = command("remove");
    request["record_ids"] = Json::array({session.snapshot()["records"][0]["id"]});
    ASSERT_TRUE(session.navigation.reserve());
    EXPECT_FALSE(prepare(command("start")));
    EXPECT_FALSE(session.submission(request, error));
    session.navigation.finish();
    session.replace_document();
    EXPECT_TRUE(session.submission(request, error)) << error;
    EXPECT_EQ(session.snapshot()["records"], Json::array());
    EXPECT_EQ(session.snapshot()["pending_count"], 0);
}

TEST_F(PageAnnotationSessionTest, NavigationSuspendsOldCallbacksWithoutDiscardingRecordsOnFailure) {
    start(); select(); save("Retain");
    ASSERT_TRUE(prepare(command("read")));
    const auto old = session.sequence();
    const auto response = result();
    ASSERT_TRUE(session.navigation.reserve());
    ASSERT_TRUE(session.navigation.arm());
    ASSERT_TRUE(session.navigation.start());
    session.suspend();
    EXPECT_FALSE(session.complete(old, response, error));
    EXPECT_FALSE(prepare(command("read")));
    session.navigation.finish();
    EXPECT_EQ(session.snapshot()["records"][0]["valid"], true);
    ASSERT_TRUE(prepare(command("read")));
    EXPECT_TRUE(session.complete(session.sequence(), result(), error));
    ASSERT_TRUE(prepare(command("read")));
    EXPECT_TRUE(session.complete(session.sequence(), {{"error", "replaced_document"}}, error));
    EXPECT_EQ(session.snapshot()["records"][0]["valid"], false);
    EXPECT_EQ(session.snapshot()["pending_count"], 1);
    EXPECT_TRUE(session.document_id().empty());
}

} // namespace
} // namespace acecode::desktop
