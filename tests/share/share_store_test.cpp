#include <gtest/gtest.h>
#include "session_share/share_store.hpp"
#include "share/share_projection.hpp"
#include "peers/peer_store.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"
#include <filesystem>
#include <thread>

namespace acecode::share {
class ShareStoreTest : public testing::Test {
protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("acecode-share-" + generate_uuid());
    void TearDown() override { std::error_code ec; std::filesystem::remove_all(root, ec); }
    ShareRecord create_record(bool interactive = true) {
        ShareRecord r;
        r.session_id = "test-session"; r.cwd = path_to_utf8(root); r.owner = "Test"; r.title = "Session";
        r.expires_at = 400000; r.allow_questions = interactive;
        return r;
    }
};

TEST_F(ShareStoreTest, PersistsCredentialsAndRevocation) {
    // 场景：重建存储后凭据仍可用；撤销后同一凭据不能再次授权。
    ShareStore store(path_to_utf8(root));
    auto r = store.create(create_record(), 100);
    ShareStore reopened(path_to_utf8(root));
    EXPECT_EQ(reopened.authorize("", 101).error, "missing_share");
    EXPECT_EQ(reopened.authorize("not-a-credential", 101).error, "invalid_share");
    EXPECT_TRUE(reopened.authorize(r.credential(), 101).share);
    EXPECT_EQ(reopened.authorize(r.credential(), 400000).error, "expired");
    EXPECT_EQ(reopened.authorize(r.id + "." + std::string(32, 'x'), 101).error, "unknown_share");
    EXPECT_EQ(share_public_json(r).dump().find(r.secret), std::string::npos);
    EXPECT_TRUE(reopened.revoke(r.id));
    EXPECT_EQ(store.authorize(r.credential(), 101).error, "revoked");
}

TEST_F(ShareStoreTest, ReadOnlyCannotSpendQuotaAndInteractiveRequestsSerialize) {
    // 场景：只读提问失败且不扣次数；交互分享并发拒绝、日限额与次日恢复。
    ShareStore store(path_to_utf8(root));
    auto read_only = store.create(create_record(false), 100);
    EXPECT_EQ(store.begin_question(read_only.id, 101), "read_only");
    EXPECT_EQ(store.find(read_only.id)->question_count, 0);
    auto r = store.create(create_record(), 100);
    for (int i = 0; i < 30; ++i) {
        EXPECT_TRUE(store.begin_question(r.id, 101).empty());
        EXPECT_EQ(store.begin_question(r.id, 101), "question_busy");
        store.end_question(r.id);
    }
    EXPECT_EQ(store.begin_question(r.id, 101), "daily_limit");
    EXPECT_TRUE(store.begin_question(r.id, 86401).empty());
    store.end_question(r.id);
    EXPECT_TRUE(store.revoke(r.id));
    EXPECT_EQ(store.begin_question(r.id, 86401), "revoked");
}

TEST_F(ShareStoreTest, ConcurrentCreatesDoNotLoseRecords) {
    // 场景：多个存储实例同时创建，不得覆盖彼此的分享记录。
    std::vector<std::thread> writers;
    for (int i = 0; i < 8; ++i) writers.emplace_back([this] {
        ShareStore store(path_to_utf8(root)); store.create(create_record(), 100);
    });
    for (auto& writer : writers) writer.join();
    EXPECT_EQ(ShareStore(path_to_utf8(root)).list().size(), 8u);
}

TEST_F(ShareStoreTest, QuestionLogIsBounded) {
    // 场景：超过 200 条提问后丢弃最早记录，不触碰主会话文件。
    ShareStore store(path_to_utf8(root));
    auto r = store.create(create_record(), 100);
    for (int i = 0; i < 205; ++i) store.append_question(r.id, {{"index", i}});
    auto questions = store.questions(r.id);
    EXPECT_EQ(questions.size(), 200u);
    EXPECT_EQ(questions.front().at("index"), 5);
}

TEST(ShareProjection, ReadOnlyVisibilityRemovesToolPayloadAndSecrets) {
    // 场景：对话档不返回工具正文；两档都脱敏并去掉内部权限元数据。
    nlohmann::json input{{"role", "tool"}, {"content", "api_key=secret-value"},
        {"metadata", {{"tool_name", "file_read"}, {"permission", "private"}, {"tool_summary", "Read file"}}}};
    const auto minimal = project_message(input, false);
    EXPECT_FALSE(minimal.contains("content"));
    EXPECT_FALSE(minimal.at("metadata").contains("permission"));
    const auto full = project_message(input, true).dump();
    EXPECT_EQ(full.find("secret-value"), std::string::npos);
    EXPECT_NE(full.find("[REDACTED]"), std::string::npos);
}

TEST(ShareProjection, StreamingSecretsCannotEscapeChunkBoundaries) {
    // 场景：凭据在任意字符处被分成两段，已发出的拼接内容仍不含凭据。
    const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz1234567890";
    const std::string text = "hello\n" + secret + "\nbye";
    for (std::size_t split = 0; split < text.size(); ++split) {
        ShareStreamRedactor stream;
        auto output = stream.append(text.substr(0, split));
        output += stream.append(text.substr(split)); output += stream.finish();
        EXPECT_EQ(output.find(secret), std::string::npos);
        EXPECT_NE(output.find("[REDACTED]"), std::string::npos);
    }
}

TEST(ShareProjection, SensitiveFilesAreDeniedAcrossPlatforms) {
    // 场景：大小写、Windows 分隔符、密钥目录与常见配置文件都被拒读。
    for (const auto* path : {".env", "dir/.env.local", "C:\\work\\KEY.PEM", ".ssh/config", "id_rsa.pub", ".npmrc", "cert.p12"})
        EXPECT_TRUE(guest_sensitive_path(path)) << path;
    EXPECT_FALSE(guest_sensitive_path("src/environment.cpp"));
}

TEST(ShareProjection, InternalContextsAndNonImagePayloadsStayPrivate) {
    // 场景：两档投影都拒绝系统/隐形上下文，用户只展示原始输入，图片仅暴露受限 id。
    for (const bool tools : {false, true}) {
        EXPECT_TRUE(project_message({{"role", "system"}, {"content", "private"}}, tools).is_null());
        EXPECT_TRUE(project_message({{"role", "user"}, {"is_meta", true}, {"content", "internal prompt"}}, tools).is_null());
        EXPECT_TRUE(project_message({{"role", "assistant"}, {"metadata", {{"hidden", true}}}}, tools).is_null());
        auto projected = project_message({{"role", "user"}, {"content", "expanded context"},
            {"metadata", {{"display_text", "visible input"}}},
            {"content_parts", nlohmann::json::array({
                {{"type", "image"}, {"attachment", {{"id", "image-id"}, {"path", "private-path"}, {"data_url", "data:private"}}}},
                {{"type", "file"}, {"attachment", {{"id", "file-id"}}}}
            })}}, tools);
        EXPECT_EQ(projected.at("content"), "visible input");
        EXPECT_EQ(projected.at("content_parts").size(), 1u);
        EXPECT_EQ(projected.dump().find("private"), std::string::npos);
    }
}
} // namespace acecode::share
