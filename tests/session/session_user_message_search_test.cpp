#include <gtest/gtest.h>
#include <sqlite3.h>

#include "session/session_storage.hpp"
#include "session/session_history_page.hpp"
#include "session/session_user_message_search.hpp"

#include <filesystem>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path temp_dir(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_user_message_search_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

acecode::ChatMessage user_message(std::string content) {
    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = std::move(content);
    return msg;
}

} // namespace

TEST(SessionUserMessageSearch, DisplayTextWinsOverExpandedContent) {
    auto msg = user_message("[SYSTEM: expanded prompt]");
    msg.metadata = nlohmann::json{{"display_text", "/skill inspect"}};

    auto searchable = acecode::build_searchable_user_message("sid", 0, msg);
    ASSERT_TRUE(searchable.has_value());
    EXPECT_EQ(searchable->user_text, "/skill inspect");
    EXPECT_NE(searchable->search_text.find("/skill inspect"), std::string::npos);
    EXPECT_EQ(searchable->search_text.find("[SYSTEM: expanded prompt]"), std::string::npos);
}

TEST(SessionUserMessageSearch, HiddenAndMetaUserMessagesAreExcluded) {
    auto hidden = user_message("hidden goal");
    hidden.metadata = nlohmann::json{{"hidden_goal_context", true}};
    EXPECT_FALSE(acecode::is_searchable_visible_user_message(hidden));
    EXPECT_FALSE(acecode::build_searchable_user_message("sid", 0, hidden).has_value());

    auto meta = user_message("compact boundary");
    meta.is_meta = true;
    EXPECT_FALSE(acecode::is_searchable_visible_user_message(meta));
    EXPECT_FALSE(acecode::build_searchable_user_message("sid", 1, meta).has_value());

    auto compact_summary = user_message("compact summary");
    compact_summary.is_compact_summary = true;
    EXPECT_FALSE(acecode::is_searchable_visible_user_message(compact_summary));
    EXPECT_FALSE(acecode::build_searchable_user_message("sid", 2, compact_summary).has_value());
}

TEST(SessionUserMessageSearch, ExtractsOnlyFileAndImageAttachmentNames) {
    auto msg = user_message("please inspect");
    msg.content_parts = nlohmann::json::array({
        {{"type", "text"}, {"text", "please inspect"}},
        {{"type", "file"}, {"attachment", {{"id", "att_file"}, {"name", "report-final.pdf"}, {"path", "C:/secret/report-final.pdf"}}}},
        {{"type", "image"}, {"attachment", {{"id", "att_img"}, {"name", "diagram.png"}, {"blob_url", "/blob"}}}},
        {{"type", "browser_context"}, {"context", {{"title", "Browser Tab"}}}},
    });

    auto names = acecode::searchable_user_message_attachment_names(msg);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "report-final.pdf");
    EXPECT_EQ(names[1], "diagram.png");

    auto searchable = acecode::build_searchable_user_message("sid", 2, msg);
    ASSERT_TRUE(searchable.has_value());
    EXPECT_NE(searchable->search_text.find("report-final.pdf"), std::string::npos);
    EXPECT_NE(searchable->search_text.find("diagram.png"), std::string::npos);
    EXPECT_EQ(searchable->search_text.find("C:/secret"), std::string::npos);
    EXPECT_EQ(searchable->search_text.find("att_file"), std::string::npos);
}

TEST(SessionUserMessageSearch, AttachmentOnlyMessageIsSearchable) {
    auto msg = user_message("");
    msg.content_parts = nlohmann::json::array({
        {{"type", "file"}, {"attachment", {{"name", "notes.txt"}}}},
    });

    auto searchable = acecode::build_searchable_user_message("sid", 3, msg);
    ASSERT_TRUE(searchable.has_value());
    EXPECT_TRUE(searchable->user_text.empty());
    EXPECT_EQ(searchable->attachment_text, "notes.txt");
    EXPECT_EQ(searchable->search_text, "notes.txt");
}

TEST(SessionUserMessageIndex, RebuildsAndSearchesUserTextAndAttachmentNames) {
    auto dir = temp_dir("index");
    const std::string project_dir = dir.string();
    const std::string sid = "session-a";
    const std::string jsonl = acecode::SessionStorage::session_path(project_dir, sid);

    auto first = user_message("我们要做 sqlite索引");
    first.uuid = "u1";
    auto hidden = user_message("quarterly-secret");
    hidden.metadata = nlohmann::json{{"hidden_goal_context", true}};
    auto attachment = user_message("");
    attachment.uuid = "u2";
    attachment.content_parts = nlohmann::json::array({
        {{"type", "file"}, {"attachment", {{"name", "report-final.pdf"}}}},
    });
    acecode::SessionStorage::write_messages(jsonl, {first, hidden, attachment});

    acecode::SessionUserMessageIndex index(project_dir);
    std::string error;
    ASSERT_TRUE(index.rebuild_session(sid, jsonl, &error)) << error;

    auto text_matches = index.search("sqlite索引", 10, &error);
    ASSERT_EQ(text_matches.size(), 1u) << error;
    EXPECT_EQ(text_matches[0].session_id, sid);
    EXPECT_NE(text_matches[0].snippet.find("sqlite索引"), std::string::npos);

    auto file_matches = index.search("report-final", 10, &error);
    ASSERT_EQ(file_matches.size(), 1u) << error;
    ASSERT_EQ(file_matches[0].matched_attachment_names.size(), 1u);
    EXPECT_EQ(file_matches[0].matched_attachment_names[0], "report-final.pdf");

    auto hidden_matches = index.search("quarterly-secret", 10, &error);
    EXPECT_TRUE(hidden_matches.empty());
}

TEST(SessionUserMessageIndex, EnsureSessionRebuildsStaleJsonl) {
    auto dir = temp_dir("stale");
    const std::string project_dir = dir.string();
    const std::string sid = "session-b";
    const std::string jsonl = acecode::SessionStorage::session_path(project_dir, sid);

    acecode::SessionStorage::write_messages(jsonl, {user_message("first searchable")});

    acecode::SessionUserMessageIndex index(project_dir);
    std::string error;
    ASSERT_TRUE(index.ensure_session_indexed(sid, jsonl, &error)) << error;
    ASSERT_EQ(index.search("first", 10, &error).size(), 1u);

    acecode::SessionStorage::write_messages(jsonl, {
        user_message("first searchable"),
        user_message("second searchable"),
    });
    ASSERT_TRUE(index.ensure_session_indexed(sid, jsonl, &error)) << error;
    ASSERT_EQ(index.search("second", 10, &error).size(), 1u);
}

TEST(SessionUserMessageIndex, IncrementalAppendUsesFreshSource) {
    auto dir = temp_dir("append");
    const std::string project_dir = dir.string();
    const std::string sid = "session-c";
    const std::string jsonl = acecode::SessionStorage::session_path(project_dir, sid);

    auto first = user_message("first turn");
    acecode::SessionStorage::write_messages(jsonl, {first});

    acecode::SessionUserMessageIndex index(project_dir);
    std::string error;
    ASSERT_TRUE(index.rebuild_session(sid, jsonl, &error)) << error;

    auto before = acecode::session_user_message_file_signature(jsonl);
    auto second = user_message("second turn");
    acecode::SessionStorage::append_message(jsonl, second);
    ASSERT_TRUE(index.index_appended_message(sid, 1, second, jsonl, before, &error)) << error;

    EXPECT_EQ(index.search("first", 10, &error).size(), 1u);
    EXPECT_EQ(index.search("second", 10, &error).size(), 1u);
}

// 场景:每回合开头 SessionManager 会往 JSONL 追加文件检查点等不含可搜索文本的
// 记录。期望:追加前索引是新鲜的,note_non_searchable_append 之后仍是新鲜的
// (下一条消息走增量快路径);追加前已经过期的不被「洗白」,留给下一次重建。
// 回归表现:这些追加以前不更新签名,下一条消息落盘时整份 JSONL 被重读重建,
// 会话越长发消息越慢(YTB 反馈:重启后第一条「继续」18 秒才开跑)。
TEST(SessionUserMessageIndex, NonSearchableAppendKeepsSourceFresh) {
    auto dir = temp_dir("checkpoint");
    const std::string project_dir = dir.string();
    const std::string sid = "session-cp";
    const std::string jsonl = acecode::SessionStorage::session_path(project_dir, sid);

    acecode::SessionStorage::write_messages(jsonl, {user_message("first turn")});
    acecode::SessionUserMessageIndex index(project_dir);
    std::string error;
    ASSERT_TRUE(index.rebuild_session(sid, jsonl, &error)) << error;

    acecode::ChatMessage checkpoint;
    checkpoint.role = "system";
    checkpoint.is_meta = true;
    checkpoint.content = "[File checkpoint]";

    auto before = acecode::session_user_message_file_signature(jsonl);
    ASSERT_TRUE(acecode::SessionStorage::append_message(jsonl, checkpoint));
    auto after = acecode::session_user_message_file_signature(jsonl);
    EXPECT_FALSE(index.source_is_fresh(sid, after, &error));
    ASSERT_TRUE(index.note_non_searchable_append(sid, jsonl, before, &error)) << error;
    EXPECT_TRUE(index.source_is_fresh(sid, after, &error));

    // 过期状态不被洗白:先有一次没记录的追加,再 note 一次,仍然过期。
    ASSERT_TRUE(acecode::SessionStorage::append_message(jsonl, checkpoint));
    auto unnoted = acecode::session_user_message_file_signature(jsonl);
    ASSERT_TRUE(acecode::SessionStorage::append_message(jsonl, checkpoint));
    ASSERT_TRUE(index.note_non_searchable_append(sid, jsonl, unnoted, &error)) << error;
    EXPECT_FALSE(index.source_is_fresh(
        sid, acecode::session_user_message_file_signature(jsonl), &error));
}

TEST(SessionUserMessageIndex, SearchLimitCountsSessionsNotRawMatchingMessages) {
    auto dir = temp_dir("limit");
    const std::string project_dir = dir.string();
    const std::string busy_sid = "session-many";
    const std::string other_sid = "session-other";
    const std::string busy_jsonl = acecode::SessionStorage::session_path(project_dir, busy_sid);
    const std::string other_jsonl = acecode::SessionStorage::session_path(project_dir, other_sid);

    std::vector<acecode::ChatMessage> busy_messages;
    for (int i = 0; i < 20; ++i) {
        busy_messages.push_back(user_message("needle repeated " + std::to_string(i)));
    }
    acecode::SessionStorage::write_messages(busy_jsonl, busy_messages);
    acecode::SessionStorage::write_messages(other_jsonl, {user_message("needle in other session")});

    acecode::SessionUserMessageIndex index(project_dir);
    std::string error;
    ASSERT_TRUE(index.rebuild_session(busy_sid, busy_jsonl, &error)) << error;
    ASSERT_TRUE(index.rebuild_session(other_sid, other_jsonl, &error)) << error;

    auto matches = index.search("needle", 2, &error);
    ASSERT_EQ(matches.size(), 2u) << error;
    EXPECT_EQ(matches[0].session_id, busy_sid);
    EXPECT_EQ(matches[1].session_id, other_sid);
}

// 回归:索引最初复用 goal 的 state.sqlite3,而 SessionManager::existing_goal_store()
// 以「state.sqlite3 文件存在」为惰性打开条件 —— 索引一建库,每个普通会话的
// goal store 都会打开常驻连接,Windows 上锁住项目目录(表现:AgentLoopPlanMode
// 系列在析构 remove_all(project_dir) 时抛 filesystem_error → noexcept 析构
// terminate,进程退出码 3)。期望:索引使用独立数据库文件,绝不创建
// state.sqlite3;索引对象析构后项目目录可完整删除(无残留文件句柄)。
TEST(SessionUserMessageIndex, DoesNotCreateGoalStateDbAndReleasesHandles) {
    auto dir = temp_dir("handles");
    const std::string project_dir = dir.string();
    const std::string sid = "session-handle";
    const std::string jsonl = acecode::SessionStorage::session_path(project_dir, sid);
    acecode::SessionStorage::write_messages(jsonl, {user_message("handle probe")});

    {
        acecode::SessionUserMessageIndex index(project_dir);
        std::string error;
        ASSERT_TRUE(index.rebuild_session(sid, jsonl, &error)) << error;
        ASSERT_EQ(index.search("handle probe", 10, &error).size(), 1u);
        EXPECT_FALSE(fs::exists(dir / "state.sqlite3"))
            << "索引不能创建 goal 的 state.sqlite3(existing_goal_store 以其存在为打开条件)";
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    EXPECT_FALSE(static_cast<bool>(ec))
        << "索引析构后项目目录必须可删除: " << ec.message();
    EXPECT_FALSE(fs::exists(dir));
}

// 触发场景:用户在 Web「后台任务」面板或 TUI /tasks clear 点「清除」
// (purge = 永久删除磁盘数据)。期望:remove_session 后该会话的用户消息
// 全文从 state.sqlite3 彻底消失,不能因为索引残留而继续被搜到。
TEST(SessionUserMessageIndex, RemoveSessionDeletesIndexedContent) {
    auto dir = temp_dir("remove");
    const std::string project_dir = dir.string();
    const std::string sid = "session-purged";
    const std::string other_sid = "session-kept";
    const std::string jsonl = acecode::SessionStorage::session_path(project_dir, sid);
    const std::string other_jsonl = acecode::SessionStorage::session_path(project_dir, other_sid);

    acecode::SessionStorage::write_messages(jsonl, {user_message("private purge target")});
    acecode::SessionStorage::write_messages(other_jsonl, {user_message("keep this session")});

    acecode::SessionUserMessageIndex index(project_dir);
    std::string error;
    ASSERT_TRUE(index.rebuild_session(sid, jsonl, &error)) << error;
    ASSERT_TRUE(index.rebuild_session(other_sid, other_jsonl, &error)) << error;
    ASSERT_EQ(index.search("purge target", 10, &error).size(), 1u);

    ASSERT_TRUE(index.remove_session(sid, &error)) << error;

    EXPECT_TRUE(index.search("purge target", 10, &error).empty())
        << "purge 后索引残留了已删除会话的用户消息";
    // 只删目标会话,其它会话不受影响。
    EXPECT_EQ(index.search("keep this", 10, &error).size(), 1u);
}

// 回归:purge_session_files 只删 JSONL/meta 文件,不碰 state.sqlite3。
// 修复前,通过任何绕过 remove_session 的删除路径(手工删文件、旧版本
// purge)删掉的会话,其用户消息全文会永久残留在索引里。期望:
// ensure_project_indexed 的孤儿回收发现 JSONL 已不存在时清掉对应行。
TEST(SessionUserMessageIndex, EnsureProjectIndexedPrunesDeletedSessions) {
    auto dir = temp_dir("prune");
    const std::string project_dir = dir.string();
    const std::string sid = "session-deleted";
    const std::string jsonl = acecode::SessionStorage::session_path(project_dir, sid);

    acecode::SessionStorage::write_messages(jsonl, {user_message("orphan secret text")});

    acecode::SessionUserMessageIndex index(project_dir);
    std::string error;
    ASSERT_TRUE(index.rebuild_session(sid, jsonl, &error)) << error;
    ASSERT_EQ(index.search("orphan secret", 10, &error).size(), 1u);

    fs::remove(jsonl);
    ASSERT_TRUE(index.ensure_project_indexed(&error)) << error;

    EXPECT_TRUE(index.search("orphan secret", 10, &error).empty())
        << "JSONL 已删除的会话内容仍留在索引里";
}

TEST(SessionUserMessageIndex, BytePositionsSurviveHiddenCheckpointsRebuildAndAppend) {
    auto dir = temp_dir("positions");
    const std::string sid = "session-position";
    const auto path = acecode::SessionStorage::session_path(dir.string(), sid);
    std::vector<acecode::ChatMessage> history;
    for (int i = 0; i < 8; ++i) {
        history.push_back(user_message("target-" + std::to_string(i)));
        acecode::ChatMessage hidden; hidden.role = "system"; hidden.is_meta = true;
        hidden.subtype = "file_checkpoint"; history.push_back(hidden);
    }
    acecode::SessionStorage::write_messages(path, history);
    acecode::SessionUserMessageIndex index(dir.string());
    std::string error;
    ASSERT_TRUE(index.rebuild_session(sid, path, &error)) << error;
    auto results = index.search("target-5", 10, &error);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].message_ordinal, 10);
    acecode::SessionHistoryRequest request; request.from_position = results[0].message_position;
    auto page = acecode::load_session_history_page(path, request);
    ASSERT_FALSE(page.messages.empty());
    EXPECT_EQ(page.messages[0].message.content, "target-5");
    const auto before = acecode::session_user_message_file_signature(path);
    const auto appended = user_message("appended-target");
    ASSERT_TRUE(acecode::SessionStorage::append_message(path, appended));
    ASSERT_TRUE(index.index_appended_message(sid, 16, appended, path, before, &error)) << error;
    results = index.search("appended-target", 10, &error);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].message_position, static_cast<std::uint64_t>(before.size));
    request.from_position = results[0].message_position;
    EXPECT_EQ(acecode::load_session_history_page(path, request).messages[0].message.content, "appended-target");
}

TEST(SessionUserMessageIndex, UpgradesLegacyProjectionAndRebuildsPositions) {
    auto dir = temp_dir("legacy_schema");
    const auto path = acecode::SessionStorage::session_path(dir.string(), "legacy");
    acecode::SessionStorage::write_messages(path, {user_message("schema migration target")});
    {
        acecode::platform::UniqueSqlite database;
        ASSERT_EQ(sqlite3_open((dir / "user_message_search.sqlite3").string().c_str(), database.put()), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(database.get(),
            "CREATE TABLE session_user_message_index(session_id TEXT, message_ordinal INTEGER);"
            "CREATE TABLE session_user_message_sources(session_id TEXT);"
            "INSERT INTO session_user_message_sources VALUES('legacy');", nullptr, nullptr, nullptr), SQLITE_OK);
    }
    acecode::SessionUserMessageIndex index(dir.string());
    std::string error;
    ASSERT_TRUE(index.ensure_session_indexed("legacy", path, &error)) << error;
    const auto rows = index.search("migration target", 10, &error);
    ASSERT_EQ(rows.size(), 1u) << error;
    EXPECT_EQ(rows[0].message_position, 0u);
}

TEST(SessionUserMessageIndex, CurrentSchemaInitializationDoesNotAcquireAWriteLock) {
    auto dir = temp_dir("schema_reader");
    acecode::SessionUserMessageIndex initial(dir.string());
    std::string error;
    ASSERT_TRUE(initial.initialize(&error)) << error;
    acecode::platform::UniqueSqlite writer;
    ASSERT_EQ(sqlite3_open((dir / "user_message_search.sqlite3").string().c_str(), writer.put()), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(writer.get(), "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), SQLITE_OK);
    acecode::SessionUserMessageIndex reader(dir.string());
    EXPECT_TRUE(reader.initialize(&error)) << error;
    EXPECT_EQ(sqlite3_exec(writer.get(), "ROLLBACK;", nullptr, nullptr, nullptr), SQLITE_OK);
}
