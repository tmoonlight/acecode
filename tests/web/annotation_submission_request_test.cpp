#include <gtest/gtest.h>
#include "web/annotation_submission_request.hpp"

namespace {
using nlohmann::json;

acecode::AnnotationSubmissionIdentity parse(const json& payload, bool ordinary = true) {
    acecode::AnnotationSubmissionIdentity identity;
    std::string error;
    EXPECT_TRUE(acecode::web::parse_annotation_submission_identity(payload, ordinary, identity, error)) << error;
    return identity;
}
}

TEST(AnnotationSubmissionRequest, CanonicalFingerprintIncludesTextAttachmentsAndClientIdentity) {
    const auto payload = json::parse(R"({"text":"changes","annotation_batch_id":"batch-1","attachments":[{"id":"att-1"}],"client_message_id":"client-1"})");
    const auto original = parse(payload);
    EXPECT_EQ(original.batch_id, "batch-1");
    EXPECT_EQ(original.payload_hash.size(), 64u);
    EXPECT_EQ(original.payload_hash, parse(json::parse(R"({
        "client_message_id":"client-1", "attachments":[{"id":"att-1"}],
        "annotation_batch_id":"batch-1", "text":"changes"
    })")).payload_hash);
    for (const auto* field : {"text", "attachments", "client_message_id"}) {
        auto changed = payload;
        if (std::string(field) == "attachments") changed[field] = json::array({"att-2"});
        else changed[field] = "changed";
        EXPECT_NE(original.payload_hash, parse(changed).payload_hash);
    }
}

TEST(AnnotationSubmissionRequest, RejectsInvalidIdentityAndSideEffectFields) {
    const json base{{"annotation_batch_id", "batch-1"}, {"text", "changes"}};
    for (const json& batch_id : {json(nullptr), json(1), json(""), json("../bad"), json(std::string(129, 'a'))}) {
        auto payload = base;
        payload["annotation_batch_id"] = batch_id;
        acecode::AnnotationSubmissionIdentity identity;
        std::string error;
        EXPECT_FALSE(acecode::web::parse_annotation_submission_identity(payload, true, identity, error));
        EXPECT_TRUE(identity.batch_id.empty());
    }
    for (const auto* field : {"swarm_mode", "worktree", "contexts", "session_references", "content", "expected_turn_id"}) {
        auto payload = base;
        payload[field] = json::object();
        acecode::AnnotationSubmissionIdentity identity;
        std::string error;
        EXPECT_FALSE(acecode::web::parse_annotation_submission_identity(payload, true, identity, error));
    }
    acecode::AnnotationSubmissionIdentity identity;
    std::string error;
    EXPECT_FALSE(acecode::web::parse_annotation_submission_identity(base, false, identity, error));
}

TEST(AnnotationSubmissionRequest, OrdinaryAndSelectionPayloadsRemainUnchanged) {
    const json payload{{"text", "ordinary"}, {"client_message_id", "same-id"},
                       {"contexts", json::array({{{"type", "selection"}, {"text", "selected"}}})}};
    EXPECT_TRUE(parse(payload).batch_id.empty());
    EXPECT_TRUE(parse(payload, false).batch_id.empty());
}
