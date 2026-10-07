#include <gtest/gtest.h>

#include "session/compact_checkpoint.hpp"
#include "llm/message_predicates.hpp"
#include "session/request_context_record.hpp"
#include "session/session_history_page.hpp"
#include "session/session_manager.hpp"
#include "session/session_recent_activity.hpp"
#include "session/session_rewind.hpp"
#include "session/session_serializer.hpp"
#include "session/session_user_message_search.hpp"
#include "utils/uuid.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

acecode::ChatMessage msg(std::string role, std::string content) {
    acecode::ChatMessage m;
    m.role = std::move(role);
    m.content = std::move(content);
    return m;
}

acecode::ChatMessage transcript_only(std::string content) {
    auto m = msg("system", std::move(content));
    m.metadata = nlohmann::json{{"transcript_only", true}};
    return m;
}

acecode::ChatMessage malformed_checkpoint() {
    acecode::ChatMessage m;
    m.role = "system";
    m.content = "[bad checkpoint]";
    m.is_meta = true;
    m.subtype = acecode::kCompactCheckpointSubtype;
    m.metadata = nlohmann::json{{"version", acecode::kCompactCheckpointVersion}};
    return m;
}

acecode::ChatMessage request_context(const char* subtype, std::string content) {
    auto m = msg("user", std::move(content));
    m.is_meta = true;
    m.subtype = subtype;
    m.uuid = acecode::generate_uuid();
    m.metadata = {{"request_context_version", 1},
                  {"skills", "unchanged skill index"},
                  {"context_state", {{"session", "project instructions"}}}};
    return m;
}

struct RequestContextSession {
    const std::filesystem::path cwd = std::filesystem::temp_directory_path() /
        ("acecode_context_checkpoint_" + acecode::generate_uuid());
    std::string project_dir;
    const std::string session_id = acecode::SessionStorage::generate_session_id();
    acecode::SessionManager session;

    RequestContextSession() {
        std::filesystem::create_directories(cwd);
        session.start_session(cwd.string(), "stub", "stub-model", session_id);
        // Windows resolves drive aliases/junctions only after cwd exists. Use
        // the same resolved project identity as the manager that writes rows.
        project_dir = session.current_project_dir();
    }

    ~RequestContextSession() {
        session.end_current_session();
        std::error_code ec;
        std::filesystem::remove_all(project_dir, ec);
        std::filesystem::remove_all(cwd, ec);
    }
};

} // namespace

TEST(CompactCheckpoint, RoundTripsReplacementHistory) {
    acecode::ChatMessage assistant = msg("assistant", "kept assistant");
    assistant.tool_calls = nlohmann::json::array({
        {
            {"id", "call-1"},
            {"type", "function"},
            {"function", {{"name", "bash"}, {"arguments", "{}"}}}
        }
    });

    acecode::CompactCheckpoint checkpoint;
    checkpoint.id = "checkpoint-id";
    checkpoint.timestamp = "2026-06-24T01:02:03Z";
    checkpoint.trigger = "manual";
    checkpoint.summary = "summary";
    checkpoint.messages_compressed = 12;
    checkpoint.estimated_tokens_saved = 345;
    checkpoint.pre_tokens = 1000;
    checkpoint.post_tokens = 200;
    checkpoint.window_number = 3;
    checkpoint.first_window_id = "window-first";
    checkpoint.previous_window_id = "window-previous";
    checkpoint.window_id = "window-current";
    checkpoint.replacement_history = {msg("system", "summary"), assistant};

    auto encoded = acecode::encode_compact_checkpoint(checkpoint);
    EXPECT_TRUE(acecode::is_compact_checkpoint_message(encoded));
    EXPECT_TRUE(encoded.is_meta);
    EXPECT_EQ(encoded.subtype, acecode::kCompactCheckpointSubtype);

    auto decoded = acecode::decode_compact_checkpoint(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->id, "checkpoint-id");
    EXPECT_EQ(decoded->timestamp, "2026-06-24T01:02:03Z");
    EXPECT_EQ(decoded->trigger, "manual");
    EXPECT_EQ(decoded->summary, "summary");
    EXPECT_EQ(decoded->messages_compressed, 12);
    EXPECT_EQ(decoded->estimated_tokens_saved, 345);
    EXPECT_EQ(decoded->window_number, 3);
    EXPECT_EQ(decoded->first_window_id, "window-first");
    EXPECT_EQ(decoded->previous_window_id, "window-previous");
    EXPECT_EQ(decoded->window_id, "window-current");
    ASSERT_EQ(decoded->replacement_history.size(), 2u);
    EXPECT_EQ(decoded->replacement_history[0].content, "summary");
    EXPECT_TRUE(decoded->replacement_history[1].tool_calls.is_array());
}

TEST(CompactCheckpoint, DecodesLegacyVersionWithoutWindowMetadata) {
    acecode::CompactCheckpoint checkpoint;
    checkpoint.id = "legacy-checkpoint-id";
    checkpoint.replacement_history = {msg("user", "legacy summary")};
    auto encoded = acecode::encode_compact_checkpoint(checkpoint);
    encoded.metadata["version"] = 1;
    encoded.metadata.erase("window_number");
    encoded.metadata.erase("first_window_id");
    encoded.metadata.erase("previous_window_id");
    encoded.metadata.erase("window_id");

    auto decoded = acecode::decode_compact_checkpoint(encoded);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->version, 1);
    EXPECT_EQ(decoded->id, "legacy-checkpoint-id");
    EXPECT_EQ(decoded->window_number, 0);
    EXPECT_TRUE(decoded->first_window_id.empty());
    EXPECT_TRUE(decoded->previous_window_id.empty());
    EXPECT_TRUE(decoded->window_id.empty());
    ASSERT_EQ(decoded->replacement_history.size(), 1u);
    EXPECT_EQ(decoded->replacement_history[0].content, "legacy summary");
}

TEST(CompactCheckpoint, RoundTripsUnsignedWindowNumberWithoutOverflow) {
    acecode::CompactCheckpoint checkpoint;
    checkpoint.window_number = std::numeric_limits<std::uint64_t>::max();
    checkpoint.replacement_history = {msg("user", "summary")};

    auto decoded = acecode::decode_compact_checkpoint(
        acecode::encode_compact_checkpoint(checkpoint));

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->window_number,
              std::numeric_limits<std::uint64_t>::max());
}

TEST(CompactCheckpoint, ProviderRelevantMessagesFiltersHiddenRows) {
    auto meta = msg("system", "meta");
    meta.is_meta = true;
    auto pseudo = msg("tool_result", "display only");

    auto filtered = acecode::provider_relevant_messages({
        msg("user", "visible user"),
        meta,
        transcript_only("visible only"),
        pseudo,
        msg("assistant", "visible assistant"),
    });

    ASSERT_EQ(filtered.size(), 2u);
    EXPECT_EQ(filtered[0].content, "visible user");
    EXPECT_EQ(filtered[1].content, "visible assistant");
}

TEST(CompactCheckpoint, PreservesOnlySupportedRequestContextRecords) {
    const auto snapshot = request_context(acecode::kRequestContextSnapshot, "frozen snapshot");
    const auto update = request_context(acecode::kRequestContextUpdate, "appended update");
    auto unknown = snapshot;
    unknown.subtype = "unrelated_meta";
    auto future_version = snapshot;
    future_version.metadata["request_context_version"] = 2;
    auto unversioned = snapshot;
    unversioned.metadata.erase("request_context_version");
    auto invalid_version = snapshot;
    invalid_version.metadata["request_context_version"] = "1";
    auto wrong_role = snapshot;
    wrong_role.role = "system";
    auto transcript = snapshot;
    transcript.metadata["transcript_only"] = true;
    auto malformed_metadata = snapshot;
    malformed_metadata.metadata = nlohmann::json::array({1});

    const auto effective = acecode::reconstruct_effective_model_history({
        snapshot, unknown, future_version, unversioned, invalid_version,
        wrong_role, transcript, malformed_metadata, msg("user", "visible"), update,
    });
    ASSERT_EQ(effective.size(), 3u);
    EXPECT_EQ(acecode::serialize_message(effective[0]), acecode::serialize_message(snapshot));
    EXPECT_EQ(effective[1].content, "visible");
    EXPECT_EQ(acecode::serialize_message(effective[2]), acecode::serialize_message(update));
}

TEST(CompactCheckpoint, MalformedRequestContextFieldsStayExcludedFromModelHistory) {
    const std::vector<nlohmann::json> invalid_fields = {
        {{"request_context_version", 1.0}},
        {{"context_state", nlohmann::json::array()}},
        {{"context_state", {{"session", nlohmann::json::object()}}}},
        {{"skills", nlohmann::json::array()}},
        {{"project_rules_bytes", -1}},
        {{"project_rules_bytes", 1.5}},
        {{"project_rules_bytes", "12"}},
        {{"memory_active", "false"}},
        {{"transcript_only", "false"}},
    };
    for (const char* subtype : {acecode::kRequestContextSnapshot, acecode::kRequestContextUpdate}) {
        for (const auto& fields : invalid_fields) {
            SCOPED_TRACE(fields.dump());
            auto record = request_context(subtype, "untrusted malformed context");
            record.metadata.update(fields);
            EXPECT_FALSE(acecode::is_request_context_record(record));
            const auto reloaded = acecode::deserialize_message(acecode::serialize_message(record));
            const auto effective = acecode::reconstruct_effective_model_history(
                {reloaded, msg("user", "visible task")});
            ASSERT_EQ(effective.size(), 1u);
            EXPECT_EQ(effective.front().content, "visible task");
        }
    }
}

TEST(CompactCheckpoint, CheckedContextAppendReportsFailureAndCanRetryWithoutPhantomRows) {
    RequestContextSession fixture;
    auto user = msg("user", "visible task");
    ASSERT_TRUE(fixture.session.try_on_message(user));
    const auto path = std::filesystem::path(acecode::SessionStorage::session_path(
        fixture.project_dir, fixture.session_id));
    const auto saved_path = path.string() + ".saved";
    // Only this fixture's generated transcript is moved. A directory in its
    // place gives a deterministic append failure on Windows and Unix alike.
    std::filesystem::rename(path, saved_path);
    ASSERT_TRUE(std::filesystem::create_directory(path));
    const auto record = request_context(acecode::kRequestContextSnapshot, "durable context");
    EXPECT_FALSE(fixture.session.try_on_message(record));
    EXPECT_EQ(fixture.session.last_error(), "failed to append session message");
    EXPECT_EQ(fixture.session.load_session_meta(fixture.session_id).message_count, 1);
    EXPECT_EQ(fixture.session.display_snapshot().turn_count, 1);

    ASSERT_TRUE(std::filesystem::remove(path));
    std::filesystem::rename(saved_path, path);
    ASSERT_TRUE(fixture.session.try_on_message(record));
    EXPECT_TRUE(fixture.session.last_error().empty());
    const auto persisted = fixture.session.load_active_messages();
    ASSERT_EQ(persisted.size(), 2u);
    EXPECT_EQ(persisted.front().content, user.content);
    EXPECT_EQ(acecode::serialize_message(persisted.back()), acecode::serialize_message(record));
    EXPECT_EQ(fixture.session.load_session_meta(fixture.session_id).message_count, 2);
    EXPECT_EQ(fixture.session.display_snapshot().turn_count, 1);
}

TEST(CompactCheckpoint, RequestContextIsNeitherVisibleInputNorARewindTarget) {
    const auto snapshot = request_context(acecode::kRequestContextSnapshot, "frozen snapshot");
    const auto update = request_context(acecode::kRequestContextUpdate, "appended update");
    auto user = msg("user", "real user input");
    user.uuid = "real-user";
    user.timestamp = "2026-10-07T08:00:00Z";
    for (const auto& hidden : {snapshot, update}) {
        EXPECT_FALSE(acecode::is_real_user_message(hidden));
        EXPECT_FALSE(acecode::is_searchable_visible_user_message(hidden));
        EXPECT_FALSE(acecode::is_recent_user_message(hidden));
        EXPECT_FALSE(acecode::is_visible_paged_history_message(hidden));
        EXPECT_FALSE(acecode::is_rewind_selectable_user_message(hidden));
    }
    const std::vector<acecode::ChatMessage> history = {snapshot, user, update};
    const auto targets = acecode::collect_rewind_targets(history);
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets[0].message_uuid, user.uuid);
    EXPECT_EQ(targets[0].message_index, 1u);
    EXPECT_EQ(acecode::recent_activity_from_messages(history).last_user_message_at,
              user.timestamp);
    EXPECT_EQ(acecode::resolve_fork_anchor_index(history, 2), 1u);
}

TEST(CompactCheckpoint, SessionResumePreservesContextBytesAndVisibleTurnState) {
    RequestContextSession fixture;
    const auto snapshot = request_context(acecode::kRequestContextSnapshot, "frozen\n context bytes");
    auto user = msg("user", "visible task");
    user.uuid = "visible-task-id";
    user.timestamp = "2026-10-07T08:00:00Z";
    auto update = request_context(acecode::kRequestContextUpdate, "new project instruction");
    update.timestamp = "2026-10-07T09:00:00Z";
    fixture.session.on_message(snapshot);
    fixture.session.on_message(user);
    fixture.session.on_message(update);
    EXPECT_EQ(fixture.session.display_snapshot().turn_count, 1);
    EXPECT_EQ(fixture.session.display_snapshot().last_user_message_at, user.timestamp);
    fixture.session.end_current_session();

    acecode::SessionManager resumed;
    resumed.start_session(fixture.cwd.string(), "stub", "stub-model");
    const auto raw = resumed.resume_session(fixture.session_id, true);
    ASSERT_TRUE(resumed.last_error().empty()) << resumed.last_error();
    const auto effective = acecode::reconstruct_effective_model_history(raw);
    ASSERT_EQ(effective.size(), 3u);
    EXPECT_EQ(acecode::serialize_message(effective[0]), acecode::serialize_message(snapshot));
    EXPECT_EQ(effective[1].uuid, user.uuid);
    EXPECT_EQ(acecode::serialize_message(effective[2]), acecode::serialize_message(update));
    EXPECT_EQ(resumed.display_snapshot().turn_count, 1);
    EXPECT_EQ(resumed.display_snapshot().last_user_message_at, user.timestamp);
    EXPECT_EQ(resumed.load_session_meta(fixture.session_id).summary, "visible task");

    const auto path = acecode::SessionStorage::session_path(fixture.project_dir, fixture.session_id);
    ASSERT_TRUE(std::filesystem::is_regular_file(path));
    const auto page = acecode::load_session_history_page(path, {});
    ASSERT_EQ(page.messages.size(), 1u);
    EXPECT_EQ(page.messages.front().message.uuid, user.uuid);
}

TEST(CompactCheckpoint, CheckpointResumeKeepsFreshContextAndAppendedUpdate) {
    RequestContextSession fixture;
    const auto old_snapshot = request_context(acecode::kRequestContextSnapshot, "old snapshot");
    const auto fresh = request_context(acecode::kRequestContextSnapshot, "fresh snapshot and todos");
    auto summary = msg("user", "compact summary");
    summary.is_compact_summary = true;
    auto user = msg("user", "retained task");
    user.uuid = "retained-user";
    const auto update = request_context(acecode::kRequestContextUpdate, "after compact update");
    auto unknown = request_context("unrelated_meta", "must remain hidden from model");
    fixture.session.on_message(old_snapshot);
    fixture.session.on_message(user);
    acecode::CompactCheckpoint checkpoint;
    checkpoint.replacement_history = {fresh, unknown, user, summary};
    ASSERT_TRUE(fixture.session.append_compact_checkpoint(checkpoint));
    fixture.session.on_message(update);
    fixture.session.end_current_session();

    acecode::SessionManager resumed;
    resumed.start_session(fixture.cwd.string(), "stub", "stub-model");
    const auto raw = resumed.resume_session(fixture.session_id, true);
    ASSERT_TRUE(resumed.last_error().empty()) << resumed.last_error();
    const auto effective = acecode::reconstruct_effective_model_history(raw);
    ASSERT_EQ(effective.size(), 4u);
    EXPECT_EQ(acecode::serialize_message(effective[0]), acecode::serialize_message(fresh));
    EXPECT_EQ(effective[1].uuid, user.uuid);
    EXPECT_TRUE(effective[2].is_compact_summary);
    EXPECT_EQ(acecode::serialize_message(effective[3]), acecode::serialize_message(update));
    EXPECT_EQ(resumed.display_snapshot().turn_count, 1);
}

TEST(CompactCheckpoint, RewindAndForkPreserveRetainedRequestContextOnly) {
    RequestContextSession fixture;
    const auto snapshot = request_context(acecode::kRequestContextSnapshot, "frozen snapshot");
    auto first = msg("user", "first task");
    first.uuid = "first-user";
    const auto update = request_context(acecode::kRequestContextUpdate, "context before second task");
    auto second = msg("user", "second task");
    second.uuid = "second-user";
    const auto later = request_context(acecode::kRequestContextUpdate, "context after second task");
    for (const auto& message : {snapshot, first, update, second, later}) {
        fixture.session.on_message(message);
    }
    const auto prefix = acecode::retained_prefix_before_index(
        fixture.session.load_active_messages(), 3);
    ASSERT_EQ(prefix.size(), 3u);
    ASSERT_TRUE(fixture.session.replace_active_messages(prefix));
    EXPECT_EQ(fixture.session.display_snapshot().turn_count, 1);
    const auto fork_id = fixture.session.fork_session_to_new_id(
        prefix, "fork", fixture.session_id, first.uuid);
    ASSERT_FALSE(fork_id.empty());
    const auto fork_path = acecode::SessionStorage::session_path(fixture.project_dir, fork_id);
    ASSERT_TRUE(std::filesystem::is_regular_file(fork_path));
    const auto forked = acecode::reconstruct_effective_model_history(
        acecode::SessionStorage::load_messages(fork_path));
    ASSERT_EQ(forked.size(), 3u);
    EXPECT_EQ(acecode::serialize_message(forked[0]), acecode::serialize_message(snapshot));
    EXPECT_EQ(forked[1].uuid, first.uuid);
    EXPECT_EQ(acecode::serialize_message(forked[2]), acecode::serialize_message(update));
    EXPECT_EQ(fixture.session.load_session_meta(fork_id).turn_count, 1);

    ASSERT_FALSE(fixture.session.fork_active_session(prefix).empty());
    const auto active_fork = acecode::reconstruct_effective_model_history(
        fixture.session.load_active_messages());
    ASSERT_EQ(active_fork.size(), 3u);
    EXPECT_EQ(acecode::serialize_message(active_fork[0]), acecode::serialize_message(snapshot));
    EXPECT_EQ(acecode::serialize_message(active_fork[2]), acecode::serialize_message(update));
    EXPECT_EQ(fixture.session.display_snapshot().turn_count, 1);
}

TEST(CompactCheckpoint, ReconstructsFromLatestCheckpointAndSuffix) {
    acecode::CompactCheckpoint first;
    first.trigger = "manual";
    first.replacement_history = {msg("system", "first summary")};
    acecode::CompactCheckpoint second;
    second.trigger = "auto";
    second.replacement_history = {msg("system", "second summary")};

    std::vector<acecode::ChatMessage> raw = {
        msg("user", "old before first"),
        acecode::encode_compact_checkpoint(first),
        msg("user", "after first"),
        acecode::encode_compact_checkpoint(second),
        transcript_only("compact marker"),
        msg("user", "after second"),
    };

    auto effective = acecode::reconstruct_effective_model_history(raw);
    ASSERT_EQ(effective.size(), 2u);
    EXPECT_EQ(effective[0].content, "second summary");
    EXPECT_EQ(effective[1].content, "after second");
}

TEST(CompactCheckpoint, RepairedHistoryFillsMissingResultAcrossCheckpointSuffix) {
    acecode::ChatMessage interrupted = msg("assistant", "");
    interrupted.tool_calls = nlohmann::json::array({
        {
            {"id", "call-interrupted"},
            {"type", "function"},
            {"function", {{"name", "file_write"}, {"arguments", "{}"}}}
        }
    });

    acecode::CompactCheckpoint checkpoint;
    checkpoint.replacement_history = {
        msg("user", "summary context"),
        interrupted,
    };
    std::vector<acecode::ChatMessage> raw = {
        acecode::encode_compact_checkpoint(checkpoint),
        msg("user", "continue after restart"),
    };

    auto effective = acecode::reconstruct_effective_model_history(raw);
    ASSERT_EQ(effective.size(), 4u);
    EXPECT_EQ(effective[1].role, "assistant");
    EXPECT_EQ(effective[2].role, "tool");
    EXPECT_EQ(effective[2].tool_call_id, "call-interrupted");
    EXPECT_NE(effective[2].content.find("outcome is unknown"),
              std::string::npos);
    EXPECT_EQ(effective[3].content, "continue after restart");
}

TEST(CompactCheckpoint, MalformedCheckpointFallsBackToLatestValidCheckpoint) {
    acecode::CompactCheckpoint valid;
    valid.replacement_history = {msg("system", "valid summary")};

    std::vector<acecode::ChatMessage> raw = {
        msg("user", "old before valid"),
        acecode::encode_compact_checkpoint(valid),
        malformed_checkpoint(),
        msg("user", "after malformed"),
    };

    auto effective = acecode::reconstruct_effective_model_history(raw);
    ASSERT_EQ(effective.size(), 2u);
    EXPECT_EQ(effective[0].content, "valid summary");
    EXPECT_EQ(effective[1].content, "after malformed");
}

TEST(CompactCheckpoint, OnlyMalformedCheckpointUsesLegacyVisibleHistory) {
    std::vector<acecode::ChatMessage> raw = {
        msg("user", "legacy old"),
        malformed_checkpoint(),
        msg("assistant", "legacy reply"),
    };

    auto effective = acecode::reconstruct_effective_model_history(raw);
    ASSERT_EQ(effective.size(), 2u);
    EXPECT_EQ(effective[0].content, "legacy old");
    EXPECT_EQ(effective[1].content, "legacy reply");
}
