#include <gtest/gtest.h>

#include "session/session_storage.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using acecode::SessionMeta;
using acecode::SessionStorage;

namespace {

fs::path make_unique_tmp_dir(const std::string& hint) {
    auto base = fs::temp_directory_path() /
                ("acecode_metadata_page_" + hint + "_" +
                 std::to_string(::testing::UnitTest::GetInstance()
                     ->current_test_info()->line()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

// 写一条会话元数据,并把文件 mtime 显式退到 age_seconds 秒前。
// 分页实现按 mtime 决定「先打开谁」,所以测试必须自己掌控 mtime,不能依赖
// 写入顺序 —— 同一批写入的文件在低精度文件系统上可能拿到相同时间戳。
void write_meta_at(const fs::path& dir,
                   const std::string& id,
                   const std::string& updated_at,
                   int age_seconds,
                   bool archived = false,
                   const std::string& parent_session_id = {}) {
    SessionMeta m;
    m.id = id;
    m.cwd = "/tmp/x";
    m.created_at = updated_at;
    m.updated_at = updated_at;
    m.message_count = 1;
    m.provider = "openai";
    m.model = "test";
    m.archived = archived;
    m.parent_session_id = parent_session_id;
    const auto path = dir / (id + ".meta.json");
    ASSERT_TRUE(SessionStorage::write_meta(path.string(), m));

    std::error_code ec;
    fs::last_write_time(
        path,
        fs::file_time_type::clock::now() - std::chrono::seconds(age_seconds),
        ec);
    ASSERT_FALSE(ec) << ec.message();
}

// 便捷构造:第 index 条(0 = 最旧)。updated_at 与 mtime 同向递增,对应真实
// 数据的形态 —— meta 每次落盘都重写,所以 mtime 与文件里的 updated_at 一致。
std::string seeded_id(int index) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "20260101-0000%02d-a%03d", index % 100, index);
    return std::string(buf);
}

std::string seeded_updated_at(int index) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "2026-01-01T00:%02d:00Z", index);
    return std::string(buf);
}

std::vector<std::string> ids_of(const std::vector<SessionMeta>& metas) {
    std::vector<std::string> out;
    out.reserve(metas.size());
    for (const auto& m : metas) out.push_back(m.id);
    return out;
}

} // namespace

// 触发场景:侧边栏折叠列表只要 5 行,但项目目录里有 40 个会话。
// 期望行为:返回最新的 5 条,并且明确报告自己没读完(exhausted=false)。
// 这里的 accepted 上限是这个优化的核心不变量 —— 实现按 mtime 降序只打开
// 「够用」的那几个文件,读到 limit + margin(margin = max(8, limit))就停,
// 所以 accepted 必须远小于 candidate_files。若哪天有人把 limit 退化成
// 「读全部再截断」,accepted 会等于 40,这条断言会立刻抓住。
TEST(SessionMetadataPage, BoundedPageStopsReadingOnceItHasEnough) {
    auto dir = make_unique_tmp_dir("bounded_stop");
    constexpr int kTotal = 40;
    for (int i = 0; i < kTotal; ++i) {
        write_meta_at(dir, seeded_id(i), seeded_updated_at(i), /*age_seconds=*/kTotal - i);
    }

    const auto page = SessionStorage::list_session_metadata_page(dir.string(), /*limit=*/5);

    EXPECT_EQ(page.sessions.size(), 5u);
    EXPECT_FALSE(page.exhausted);
    EXPECT_EQ(page.candidate_files, static_cast<std::size_t>(kTotal));
    // limit(5) + margin(max(8,5)=8) = 13。允许等于,不允许接近 40。
    EXPECT_LE(page.accepted, 13u);

    // 返回的必须是 updated_at 最新的 5 条,且自身按新→旧排列。
    EXPECT_EQ(ids_of(page.sessions),
              (std::vector<std::string>{seeded_id(39), seeded_id(38), seeded_id(37),
                                        seeded_id(36), seeded_id(35)}));
}

// 触发场景:目录里的会话数少于一页。
// 期望行为:读完整个目录,exhausted=true 且 accepted 是精确总数 —— 前端据此
// 判断「这个 workspace 已全部加载」,不再在展开时发第二次全量请求。
TEST(SessionMetadataPage, ShortDirectoryReportsExhaustedAndExactCount) {
    auto dir = make_unique_tmp_dir("short_dir");
    for (int i = 0; i < 3; ++i) {
        write_meta_at(dir, seeded_id(i), seeded_updated_at(i), /*age_seconds=*/3 - i);
    }

    const auto page = SessionStorage::list_session_metadata_page(dir.string(), /*limit=*/5);

    EXPECT_EQ(page.sessions.size(), 3u);
    EXPECT_TRUE(page.exhausted);
    EXPECT_EQ(page.accepted, 3u);
    EXPECT_EQ(page.candidate_files, 3u);
}

// 触发场景:最新的一批会话全是归档 / 后台子会话,调用方用 accept 把它们滤掉。
// 期望行为:被拒的条目不占 limit 名额,否则一串归档会话就能把整页挤空,
// 侧边栏会显示成「这个 workspace 没有会话」。
TEST(SessionMetadataPage, RejectedEntriesDoNotConsumeThePage) {
    auto dir = make_unique_tmp_dir("filter_no_slot");
    // 最新的 6 条:3 条归档 + 3 条子会话;更旧的 4 条才是可见会话。
    write_meta_at(dir, "archived-a", "2026-01-01T00:20:00Z", 1, /*archived=*/true);
    write_meta_at(dir, "archived-b", "2026-01-01T00:19:00Z", 2, /*archived=*/true);
    write_meta_at(dir, "archived-c", "2026-01-01T00:18:00Z", 3, /*archived=*/true);
    write_meta_at(dir, "child-a", "2026-01-01T00:17:00Z", 4, false, "parent-1");
    write_meta_at(dir, "child-b", "2026-01-01T00:16:00Z", 5, false, "parent-1");
    write_meta_at(dir, "child-c", "2026-01-01T00:15:00Z", 6, false, "parent-1");
    write_meta_at(dir, "visible-a", "2026-01-01T00:14:00Z", 7);
    write_meta_at(dir, "visible-b", "2026-01-01T00:13:00Z", 8);
    write_meta_at(dir, "visible-c", "2026-01-01T00:12:00Z", 9);
    write_meta_at(dir, "visible-d", "2026-01-01T00:11:00Z", 10);

    const auto accept = [](const SessionMeta& m) {
        return !m.archived && m.parent_session_id.empty();
    };
    const auto page = SessionStorage::list_session_metadata_page(
        dir.string(), /*limit=*/3, accept);

    EXPECT_EQ(ids_of(page.sessions),
              (std::vector<std::string>{"visible-a", "visible-b", "visible-c"}));
}

// 触发场景:mtime 与文件内 updated_at 出现小幅错位(时钟回拨、外部工具
// touch 过文件、同秒写入的多个会话在文件系统上顺序不定)。
// 期望行为:实现按 mtime 挑候选、按 updated_at 定顺序,并且刻意多读一段
// margin,所以 margin 以内的错位不会让真正最新的会话掉出这一页。
TEST(SessionMetadataPage, ToleratesMtimeDriftWithinTheReadMargin) {
    auto dir = make_unique_tmp_dir("mtime_drift");
    for (int i = 0; i < 30; ++i) {
        write_meta_at(dir, seeded_id(i), seeded_updated_at(i), /*age_seconds=*/30 - i);
    }
    // updated_at 上最新的一条,mtime 却被压到第 10 老的位置(仍在
    // limit 5 + margin 8 = 13 的扫描窗口内)。
    write_meta_at(dir, "late-writer", "2026-01-01T09:00:00Z", /*age_seconds=*/9);

    const auto page = SessionStorage::list_session_metadata_page(dir.string(), /*limit=*/5);

    ASSERT_FALSE(page.sessions.empty());
    EXPECT_EQ(page.sessions.front().id, "late-writer");
    EXPECT_EQ(page.sessions.size(), 5u);
}

// 触发场景:limit <= 0(全局搜索、/api/sessions 等仍要全量的调用方)。
// 期望行为:与 list_session_metadata() 完全等价 —— 读全部、按 updated_at
// 降序、报精确计数。这条是给全量调用方的回归保护:分页实现现在是
// list_session_metadata 的唯一底座,退化会同时打穿两条路径。
TEST(SessionMetadataPage, UnlimitedPageMatchesFullListing) {
    auto dir = make_unique_tmp_dir("unlimited");
    for (int i = 0; i < 12; ++i) {
        write_meta_at(dir, seeded_id(i), seeded_updated_at(i), /*age_seconds=*/12 - i);
    }

    const auto page = SessionStorage::list_session_metadata_page(dir.string(), /*limit=*/0);
    const auto full = SessionStorage::list_session_metadata(dir.string());

    EXPECT_TRUE(page.exhausted);
    EXPECT_EQ(page.accepted, 12u);
    EXPECT_EQ(ids_of(page.sessions), ids_of(full));
    ASSERT_EQ(full.size(), 12u);
    EXPECT_EQ(full.front().id, seeded_id(11));
    EXPECT_EQ(full.back().id, seeded_id(0));
}

// 触发场景:目录里混着 PID 后缀的旧实验数据、非会话的邻居文件,以及
// headless `-p --session-id` 允许的自定义 id。
// 回归背景:这次把文件名判定从 std::regex 换成手写字符扫描(上千会话的
// 目录里每项跑两次正则的开销已经能量到),等价性必须逐类守住 —— 漏掉
// PID 排除会让 `<id>-<pid>.meta.json` 冒充成独立会话,收紧字符集则会让
// 自定义 id 的会话在列表里整个消失。
TEST(SessionMetadataPage, FilenameClassificationMatchesTheOldRegexRules) {
    auto dir = make_unique_tmp_dir("filenames");
    write_meta_at(dir, "20260101-000001-a001", "2026-01-01T00:01:00Z", 1);
    write_meta_at(dir, "custom_headless-id", "2026-01-01T00:02:00Z", 2);
    // PID 后缀的旧实验数据:文件名落在 session id 的宽字符集里,必须排除。
    write_meta_at(dir, "20260101-000003-a003-4242", "2026-01-01T00:03:00Z", 3);

    // 非会话的邻居文件,不带 .meta.json 后缀。
    std::ofstream(dir / "workspace.json") << "{}";
    std::ofstream(dir / "model_override.json") << "{}";
    // 后缀合法但 stem 越界(> 64 字符),同样不是会话。
    std::ofstream(dir / (std::string(65, 'a') + ".meta.json")) << "{}";

    const auto full = SessionStorage::list_session_metadata(dir.string());

    EXPECT_EQ(ids_of(full),
              (std::vector<std::string>{"custom_headless-id", "20260101-000001-a001"}));
}
