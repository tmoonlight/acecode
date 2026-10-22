// 覆盖 src/apps/web/handlers/agent_browser_top_sites.cpp:Agent Browser 新标签页
// 「推荐」(常访问网站)的记录、排序、移除与落盘。一旦回归:
//   - 同一次访问的标题 / 图标补充被当成新访问 → 刷新几次标题就把某个网站顶到第一
//   - 已移除的推荐又出现 → 用户点了 × 没有效果
//   - 损坏的文件让接口报错 → 新标签页整块空白

#include <gtest/gtest.h>

#include "web/handlers/agent_browser_top_sites.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
namespace top_sites = acecode::web::agent_browser_top_sites;
using nlohmann::json;

namespace {

constexpr std::int64_t kDay = 24LL * 60 * 60 * 1000;
constexpr std::int64_t kNow = 1'800'000'000'000LL;

top_sites::Visit visit(const std::string& url,
                       const std::string& title = "",
                       bool new_visit = true,
                       const std::string& favicon = "") {
    top_sites::Visit v;
    v.url = url;
    v.title = title;
    v.new_visit = new_visit;
    v.favicon = favicon;
    return v;
}

std::vector<std::string> displayed_urls(const json& doc, std::size_t limit = 4,
                                        std::int64_t now = kNow) {
    std::vector<std::string> urls;
    for (const auto& site : top_sites::display_sites(doc, limit, now)) {
        urls.push_back(site.at("url").get<std::string>());
    }
    return urls;
}

} // namespace

// 场景:页面地址带 #锚点、协议大写、非 http(s) 地址。
// 期望:锚点去掉、协议转小写;about:blank / file: / javascript: / 带空格的地址一律不记。
TEST(AgentBrowserTopSitesTest, NormalizesOnlyHttpUrlsAndDropsFragment) {
    EXPECT_EQ(top_sites::normalize_url("HTTPS://example.com/a?b=1#top"),
              "https://example.com/a?b=1");
    EXPECT_EQ(top_sites::normalize_url("http://127.0.0.1:5173/"), "http://127.0.0.1:5173/");
    EXPECT_EQ(top_sites::normalize_url("about:blank"), "");
    EXPECT_EQ(top_sites::normalize_url("file:///C:/a.html"), "");
    EXPECT_EQ(top_sites::normalize_url("javascript:alert(1)"), "");
    EXPECT_EQ(top_sites::normalize_url("https://exa mple.com/"), "");
    EXPECT_EQ(top_sites::normalize_url("https://"), "");
    EXPECT_EQ(top_sites::normalize_url(std::string("https://e.com/") + std::string(3000, 'a')), "");
}

// 场景:Desktop 先报一次新导航(只有地址),随后标题、图标陆续到达(new_visit=false)。
// 期望:访问次数只算 1 次;标题与图标补进同一条记录。
// 回归:曾经每次状态事件都计数,标题变化几次就让网站排名虚高。
TEST(AgentBrowserTopSitesTest, MetadataUpdatesDoNotCountAsVisits) {
    auto doc = top_sites::empty_document();
    EXPECT_TRUE(top_sites::record_visit(doc, visit("https://example.com/"), kNow));
    EXPECT_TRUE(top_sites::record_visit(doc, visit("https://example.com/", "Example", false), kNow));
    EXPECT_TRUE(top_sites::record_visit(
        doc, visit("https://example.com/", "", false, "data:image/png;base64,AAAA"), kNow));
    // 内容没变的补充不需要落盘。
    EXPECT_FALSE(top_sites::record_visit(doc, visit("https://example.com/", "Example", false), kNow));

    ASSERT_EQ(doc["sites"].size(), 1u);
    EXPECT_EQ(doc["sites"][0]["visit_count"], 1);
    EXPECT_EQ(doc["sites"][0]["title"], "Example");
    EXPECT_EQ(doc["sites"][0]["favicon"], "data:image/png;base64,AAAA");
}

// 场景:没有记录过的地址只收到元数据补充(new_visit=false)。
// 期望:不新建记录 —— 只有真正打开过的页面才能成为推荐。
TEST(AgentBrowserTopSitesTest, MetadataForUnknownUrlIsIgnored) {
    auto doc = top_sites::empty_document();
    EXPECT_FALSE(top_sites::record_visit(doc, visit("https://ghost.example/", "Ghost", false), kNow));
    EXPECT_TRUE(doc["sites"].empty());
}

// 场景:A 访问 3 次但都在 100 天前,B 今天访问 1 次。
// 期望:B 排在 A 前(3×0.1 < 1×1.0);同为近期时访问多的在前。
TEST(AgentBrowserTopSitesTest, RanksByVisitCountWeightedByRecency) {
    auto doc = top_sites::empty_document();
    for (int i = 0; i < 3; ++i) {
        top_sites::record_visit(doc, visit("https://old.example/"), kNow - 100 * kDay);
    }
    top_sites::record_visit(doc, visit("https://fresh.example/"), kNow);
    for (int i = 0; i < 2; ++i) {
        top_sites::record_visit(doc, visit("https://busy.example/"), kNow - kDay);
    }

    EXPECT_EQ(displayed_urls(doc),
              (std::vector<std::string>{"https://busy.example/", "https://fresh.example/",
                                        "https://old.example/"}));
}

// 场景:记录了 6 个网站,新标签页只要 4 个;其中一个没有标题。
// 期望:只返回前 4 个;没有标题的显示主机名(含端口,去掉用户信息)。
TEST(AgentBrowserTopSitesTest, DisplayLimitsCountAndFallsBackToHost) {
    auto doc = top_sites::empty_document();
    for (int i = 0; i < 6; ++i) {
        top_sites::record_visit(doc, visit("https://site" + std::to_string(i) + ".example/"),
                                kNow - i);
    }
    top_sites::record_visit(doc, visit("http://user@127.0.0.1:8080/app"), kNow + 1);

    const auto sites = top_sites::display_sites(doc, 4, kNow + 1);
    ASSERT_EQ(sites.size(), 4u);
    EXPECT_EQ(sites[0]["url"], "http://user@127.0.0.1:8080/app");
    EXPECT_EQ(sites[0]["title"], "127.0.0.1:8080");
    EXPECT_FALSE(sites[0].contains("favicon"));
}

// 场景:用户点 × 移除推荐,之后又访问该网站;随后点「撤销」。
// 期望:移除期间无论再访问几次都不出现在推荐里;撤销后恢复,访问次数没有丢。
TEST(AgentBrowserTopSitesTest, DismissedSiteStaysHiddenUntilRestored) {
    auto doc = top_sites::empty_document();
    top_sites::record_visit(doc, visit("https://a.example/"), kNow);
    top_sites::record_visit(doc, visit("https://b.example/"), kNow);

    EXPECT_TRUE(top_sites::dismiss(doc, "https://a.example/#ignored"));
    EXPECT_FALSE(top_sites::dismiss(doc, "https://a.example/"));  // 重复移除无变化
    top_sites::record_visit(doc, visit("https://a.example/"), kNow);
    EXPECT_EQ(displayed_urls(doc), (std::vector<std::string>{"https://b.example/"}));

    EXPECT_TRUE(top_sites::restore(doc, "https://a.example/"));
    EXPECT_FALSE(top_sites::restore(doc, "https://a.example/"));
    EXPECT_EQ(displayed_urls(doc),
              (std::vector<std::string>{"https://a.example/", "https://b.example/"}));
}

// 场景:页面给出超大的 data URL 图标、javascript: 图标、超长且带换行的标题。
// 期望:图标不存(前端退回地球图标);标题去掉控制字符并按 UTF-8 安全截断。
TEST(AgentBrowserTopSitesTest, RejectsOversizedFaviconsAndCleansTitles) {
    EXPECT_EQ(top_sites::normalize_favicon("javascript:alert(1)"), "");
    EXPECT_EQ(top_sites::normalize_favicon(
                  "data:image/png;base64," + std::string(top_sites::kMaxFaviconDataUrlBytes, 'A')),
              "");
    EXPECT_EQ(top_sites::normalize_favicon("https://cdn.example/favicon.ico"),
              "https://cdn.example/favicon.ico");

    auto doc = top_sites::empty_document();
    std::string title = "第一行\n第二行";
    for (int i = 0; i < 200; ++i) title += "汉";
    top_sites::record_visit(doc, visit("https://t.example/", title), kNow);
    const auto stored = doc["sites"][0]["title"].get<std::string>();
    EXPECT_EQ(stored.find('\n'), std::string::npos);
    EXPECT_LE(stored.size(), top_sites::kMaxTitleBytes);
    EXPECT_EQ(stored.rfind("第一行 第二行", 0), 0u);
}

// 场景:记录超过上限(kMaxStoredSites)。
// 期望:丢掉分数最低(最旧)的,文件大小有界。
TEST(AgentBrowserTopSitesTest, TrimsToStoredSiteLimit) {
    auto doc = top_sites::empty_document();
    for (std::size_t i = 0; i < top_sites::kMaxStoredSites + 5; ++i) {
        top_sites::record_visit(doc, visit("https://s" + std::to_string(i) + ".example/"),
                                kNow - 200 * kDay + static_cast<std::int64_t>(i) * kDay);
    }
    ASSERT_EQ(doc["sites"].size(), top_sites::kMaxStoredSites);
    for (const auto& site : doc["sites"]) {
        EXPECT_NE(site["url"], "https://s0.example/");
    }
}

// 场景:手改 / 旧版本留下的文件里有重复 URL(一个带锚点)、坏条目与非字符串的移除项。
// 期望:合并重复项的访问次数,丢弃坏条目,结果仍是合法文档。
TEST(AgentBrowserTopSitesTest, NormalizeDocumentMergesDuplicatesAndDropsJunk) {
    const json raw = {
        {"sites", json::array({
            {{"url", "https://dup.example/"}, {"visit_count", 2}, {"last_visit_ms", kNow - 10},
             {"title", "Old"}},
            {{"url", "https://dup.example/#x"}, {"visit_count", 3}, {"last_visit_ms", kNow},
             {"title", "New"}},
            {{"url", "about:blank"}, {"visit_count", 9}},
            "not-an-object",
        })},
        {"blocked", json::array({"https://gone.example/", 42, "ftp://x"})},
    };
    const auto doc = top_sites::normalize_document(raw);
    ASSERT_EQ(doc["sites"].size(), 1u);
    EXPECT_EQ(doc["sites"][0]["visit_count"], 5);
    EXPECT_EQ(doc["sites"][0]["title"], "New");
    EXPECT_EQ(doc["blocked"], json::array({"https://gone.example/"}));
}

// 场景:写盘后重新读取;文件被写坏(不是 JSON)。
// 期望:往返内容一致;损坏时返回空文档并给出 warning,接口照常可用。
TEST(AgentBrowserTopSitesTest, SaveLoadRoundTripAndCorruptFileFallsBack) {
    std::random_device rd;
    const auto dir = fs::temp_directory_path() /
                     ("acecode_top_sites_test_" + std::to_string(rd()));
    fs::create_directories(dir);
    const auto path = top_sites::file_path(dir.string());

    auto doc = top_sites::empty_document();
    top_sites::record_visit(doc, visit("https://round.example/", "Round"), kNow);
    top_sites::dismiss(doc, "https://hidden.example/");
    ASSERT_TRUE(top_sites::save(path, doc));

    std::string warning;
    EXPECT_EQ(top_sites::load(path, &warning), doc);
    EXPECT_TRUE(warning.empty());

    {
        std::ofstream out(fs::path(path), std::ios::binary | std::ios::trunc);
        out << "{broken";
    }
    EXPECT_EQ(top_sites::load(path, &warning), top_sites::empty_document());
    EXPECT_FALSE(warning.empty());

    std::error_code ec;
    fs::remove_all(dir, ec);
}
