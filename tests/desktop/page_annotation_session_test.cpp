#include <gtest/gtest.h>

#include "desktop/page_annotation_session.hpp"

namespace acecode::desktop {
namespace {

using Json = nlohmann::json;

class PageAnnotationSessionTest : public testing::Test {
protected:
    Json command(const std::string& action) const {
        return {{"action", action}, {"document_id", session.snapshot()["document_id"]}};
    }

    Json result(bool active = true, bool popup = false, Json target = nullptr) const {
        return {{"document_id", session.snapshot()["document_id"]},
                {"active", active}, {"popup", popup}, {"target", target},
                {"viewport", {{"width", 800}, {"height", 600}}}};
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

} // namespace
} // namespace acecode::desktop
