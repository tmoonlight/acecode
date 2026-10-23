#include <gtest/gtest.h>

#include "session/session_attention.hpp"

using acecode::SessionAttentionRecord;
using acecode::SessionAttentionState;
using acecode::SessionEventKind;
using acecode::apply_session_attention_event;
using acecode::mark_session_attention_read;
using acecode::mark_session_attention_unread;
using acecode::session_attention_state_for;
using acecode::session_event_has_user_visible_output;

TEST(SessionAttention, BusyStateTakesDisplayPrecedence) {
    SessionAttentionRecord r;
    r.update_cursor = 10;
    r.read_cursor = 0;
    r.busy = true;
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::InProgress);
}

TEST(SessionAttention, OutputMakesIdleSessionUnread) {
    SessionAttentionRecord r;
    r = apply_session_attention_event(r, SessionEventKind::Message,
                                      nlohmann::json{{"role", "assistant"}},
                                      4, 1000);
    EXPECT_EQ(r.update_cursor, 4u);
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Unread);
}

TEST(SessionAttention, UserMessageDoesNotMakeSessionUnread) {
    SessionAttentionRecord r;
    r = apply_session_attention_event(r, SessionEventKind::Message,
                                      nlohmann::json{{"role", "user"}},
                                      4, 1000);
    EXPECT_EQ(r.update_cursor, 0u);
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Read);
}

TEST(SessionAttention, ReadAcknowledgementClearsUnreadAtCurrentCursor) {
    SessionAttentionRecord r;
    r.update_cursor = 8;
    r.read_cursor = 2;
    r = mark_session_attention_read(r, 8, 2000);
    EXPECT_EQ(r.read_cursor, 8u);
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Read);
}

TEST(SessionAttention, StaleReadAcknowledgementDoesNotClearNewerOutput) {
    SessionAttentionRecord r;
    r.update_cursor = 10;
    r.read_cursor = 2;
    r = mark_session_attention_read(r, 8, 2000);
    EXPECT_EQ(r.read_cursor, 8u);
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Unread);
}

TEST(SessionAttention, MissingStatusFallsBackToRead) {
    SessionAttentionRecord r;
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Read);
}

TEST(SessionAttention, InteractiveRequestsAreUserVisible) {
    EXPECT_TRUE(session_event_has_user_visible_output(SessionEventKind::PermissionRequest, nlohmann::json::object()));
    EXPECT_TRUE(session_event_has_user_visible_output(SessionEventKind::QuestionRequest, nlohmann::json::object()));
    EXPECT_TRUE(session_event_has_user_visible_output(SessionEventKind::Error, nlohmann::json::object()));
}

TEST(SessionAttention, QuestionClosedIsLifecycleOnly) {
    EXPECT_FALSE(session_event_has_user_visible_output(SessionEventKind::QuestionClosed, nlohmann::json::object()));
}

TEST(SessionAttention, PermissionClosedIsLifecycleOnly) {
    EXPECT_FALSE(session_event_has_user_visible_output(SessionEventKind::PermissionClosed, nlohmann::json::object()));
}

TEST(SessionAttention, TurnDiffIsLifecycleOnly) {
    EXPECT_FALSE(session_event_has_user_visible_output(
        SessionEventKind::TurnDiff, nlohmann::json::object()));
}

// 场景:用户在会话右键菜单点「标记为未读」,会话此前已读(read_cursor == update_cursor)。
// 期望:已读游标退到最新输出之前 → 状态变成 Unread;之后再标记已读(cursor=0 表示
// 读到最新)又回到 Read。
TEST(SessionAttention, MarkUnreadRewindsReadCursorAndMarkReadRestores) {
    SessionAttentionRecord r;
    r.update_cursor = 8;
    r.read_cursor = 8;
    r = mark_session_attention_unread(r, 3000);
    EXPECT_EQ(r.update_cursor, 8u);
    EXPECT_EQ(r.read_cursor, 7u);
    EXPECT_EQ(r.updated_at_ms, 3000);
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Unread);
    r = mark_session_attention_read(r, 0, 4000);
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Read);
}

// 场景:会话从没产出过助手输出(update_cursor 为 0,例如刚新建)就被标记为未读。
// 期望:仍然能变成 Unread(update_cursor 补成 1),而不是因为「没有可退的游标」静默无效。
TEST(SessionAttention, MarkUnreadWorksForSessionWithoutOutput) {
    SessionAttentionRecord r;
    r = mark_session_attention_unread(r, 1000);
    EXPECT_EQ(r.update_cursor, 1u);
    EXPECT_EQ(r.read_cursor, 0u);
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Unread);
}

// 场景:会话本来就是未读(已读游标落后于最新输出),又点了一次「标记为未读」。
// 期望:已读游标原样保留,不把它往前挪也不重复后退。
TEST(SessionAttention, MarkUnreadKeepsExistingUnreadCursor) {
    SessionAttentionRecord r;
    r.update_cursor = 10;
    r.read_cursor = 4;
    r = mark_session_attention_unread(r, 2000);
    EXPECT_EQ(r.read_cursor, 4u);
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Unread);
}

// 场景:运行中的会话被标记为未读。期望:显示状态仍以运行中优先(InProgress),
// 但游标已退回,回合结束(busy=false)后显示未读。
TEST(SessionAttention, MarkUnreadOnBusySessionSurfacesAfterTurnEnds) {
    SessionAttentionRecord r;
    r.update_cursor = 6;
    r.read_cursor = 6;
    r.busy = true;
    r = mark_session_attention_unread(r, 2000);
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::InProgress);
    r.busy = false;
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Unread);
}

TEST(SessionAttention, ManualUnreadIntentSurvivesOutputAndClearsOnExplicitRead) {
    auto record = mark_session_attention_unread(SessionAttentionRecord{}, 1000);
    ASSERT_TRUE(record.manual_unread);
    record = apply_session_attention_event(record, acecode::SessionEventKind::Token,
        nlohmann::json{{"text", "later"}}, 5, 2000);
    EXPECT_TRUE(record.manual_unread);
    record.busy = true;
    record = mark_session_attention_read(record, 0, 3000);
    EXPECT_FALSE(record.manual_unread);
    EXPECT_EQ(record.read_cursor, record.update_cursor);
}
