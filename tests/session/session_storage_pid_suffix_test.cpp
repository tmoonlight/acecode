#include <gtest/gtest.h>

#include "session/session_storage.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using acecode::SessionMeta;
using acecode::SessionStorage;

namespace {

fs::path make_unique_tmp_dir(const std::string& hint) {
    auto base = fs::temp_directory_path() /
                ("acecode_canonical_storage_" + hint + "_" +
                 std::to_string(::testing::UnitTest::GetInstance()
                     ->current_test_info()->line()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

void write_meta(const fs::path& path, const std::string& id,
                const std::string& updated_at) {
    SessionMeta m;
    m.id = id;
    m.cwd = "/tmp/x";
    m.created_at = updated_at;
    m.updated_at = updated_at;
    m.message_count = 1;
    m.provider = "openai";
    m.model = "test";
    SessionStorage::write_meta(path.string(), m);
}

} // namespace

// 场景:默认 session_path/meta_path 必须写 canonical 文件名,不再带本进程 PID。
TEST(SessionStoragePidSuffix, DefaultPathsAreCanonical) {
    auto dir = make_unique_tmp_dir("default_canonical");
    const std::string id = "20260426-100000-abcd";

    EXPECT_EQ(SessionStorage::session_path(dir.string(), id),
              (dir / (id + ".jsonl")).string());
    EXPECT_EQ(SessionStorage::meta_path(dir.string(), id),
              (dir / (id + ".meta.json")).string());
}

// 场景:显式 pid<=0 也必须保持 canonical,避免 runtime 调用旧默认语义时
// 意外重新生成 `<id>-<pid>.jsonl` 多副本。
TEST(SessionStoragePidSuffix, NonPositivePidPathsAreCanonical) {
    auto dir = make_unique_tmp_dir("non_positive");
    const std::string id = "20260426-100000-abcd";

    EXPECT_EQ(SessionStorage::session_path(dir.string(), id, 0),
              (dir / (id + ".jsonl")).string());
    EXPECT_EQ(SessionStorage::session_path(dir.string(), id, -1),
              (dir / (id + ".jsonl")).string());
    EXPECT_EQ(SessionStorage::meta_path(dir.string(), id, 0),
              (dir / (id + ".meta.json")).string());
}

// 场景:pid>0 helper 只用于测试/诊断识别旧数据。正常 find_session_files
// 必须忽略这类旧 PID 后缀文件,不能再把它当成 resume 候选。
TEST(SessionStoragePidSuffix, FindSessionFilesIgnoresPidSuffixedOldData) {
    auto dir = make_unique_tmp_dir("ignore_old");
    const std::string id = "20260426-100000-abcd";

    std::ofstream(SessionStorage::session_path(dir.string(), id, 1234)) << "{}\n";

    auto candidates = SessionStorage::find_session_files(dir.string(), id);
    EXPECT_TRUE(candidates.empty());
    EXPECT_TRUE(SessionStorage::has_incompatible_pid_session_files(dir.string(), id));
}

// 场景:canonical 和旧 PID 后缀文件并存时,find_session_files 只返回 canonical。
TEST(SessionStoragePidSuffix, FindSessionFilesReturnsOnlyCanonicalWhenBothExist) {
    auto dir = make_unique_tmp_dir("canonical_wins");
    const std::string id = "20260426-100000-abcd";

    std::ofstream(SessionStorage::session_path(dir.string(), id)) << "{}\n";
    std::ofstream(SessionStorage::session_path(dir.string(), id, 1234)) << "{}\n";

    auto candidates = SessionStorage::find_session_files(dir.string(), id);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].pid, 0);
    EXPECT_EQ(candidates[0].jsonl_path, SessionStorage::session_path(dir.string(), id));
    EXPECT_EQ(candidates[0].meta_path, SessionStorage::meta_path(dir.string(), id));
}

// 场景:list_sessions 只枚举 canonical meta。旧 PID 后缀 meta 是不兼容旧数据,
// 不参与列表、不去重、不补齐。
TEST(SessionStoragePidSuffix, ListSessionsIgnoresPidSuffixedOldData) {
    auto dir = make_unique_tmp_dir("list_ignore_old");
    const std::string old_id = "20260426-100000-abcd";
    const std::string new_id = "20260426-100001-abcd";

    write_meta(SessionStorage::meta_path(dir.string(), old_id, 9999),
               old_id, "2026-04-26T10:00:00Z");
    write_meta(SessionStorage::meta_path(dir.string(), new_id),
               new_id, "2026-04-26T10:05:00Z");

    auto sessions = SessionStorage::list_sessions(dir.string());
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].id, new_id);
    EXPECT_TRUE(SessionStorage::has_incompatible_pid_session_files(dir.string(), old_id));
    EXPECT_TRUE(SessionStorage::has_incompatible_pid_session_files(dir.string()));
}
