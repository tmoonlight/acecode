// 覆盖 src/apps/web/handlers/html_site_handler.cpp:文件详情预览 .html 时,页面里相对路径的
// CSS / 图片 / 脚本经 GET /api/html-site/<站点号>/<相对路径> 加载。这条路由不走 token 鉴权,
// 所以路径限定、类型白名单与跨源许可都靠这里的纯函数把关。

#include <gtest/gtest.h>

#include "web/handlers/html_site_handler.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace acecode::web;

namespace {

class HtmlSiteTempDir : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                fs::path("acecode_html_site_" + generate_html_site_id());
        fs::create_directories(root_ / "site" / "css");
        std::ofstream(root_ / "site" / "index.html") << "<html></html>";
        std::ofstream(root_ / "site" / "css" / "a.css") << "body{}";
        std::ofstream(root_ / "secret.css") << "x";
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    fs::path root_;
};

} // namespace

// 场景:前端为同一个工作区里的多个 HTML 反复登记站点。
// 期望:同一根目录拿到同一个站点号;超过容量时最久没用的被淘汰,查不到。
TEST(HtmlSiteRegistry, ReusesIdPerRootAndEvictsLeastRecentlyUsed) {
    HtmlSiteRegistry registry(2);
    const auto a = registry.register_root(fs::path("/tmp/a"));
    EXPECT_TRUE(is_html_site_id(a));
    EXPECT_EQ(registry.register_root(fs::path("/tmp/a")), a);
    const auto b = registry.register_root(fs::path("/tmp/b"));
    EXPECT_NE(a, b);
    ASSERT_TRUE(registry.lookup(a).has_value());  // a 变成最近使用
    registry.register_root(fs::path("/tmp/c"));   // 淘汰 b
    EXPECT_FALSE(registry.lookup(b).has_value());
    EXPECT_TRUE(registry.lookup(a).has_value());
    EXPECT_FALSE(registry.lookup("0123456789abcdef0123456789abcdef").has_value());
}

// 场景:站点号出现在 URL 里充当凭证。期望:32 位小写十六进制,连续生成不重复;
// 大写、长度不对、带路径符号的都不认。
TEST(HtmlSiteRegistry, IdsAreRandomLowercaseHex) {
    std::set<std::string> seen;
    for (int i = 0; i < 64; ++i) {
        const auto id = generate_html_site_id();
        EXPECT_TRUE(is_html_site_id(id)) << id;
        seen.insert(id);
    }
    EXPECT_EQ(seen.size(), 64u);
    EXPECT_FALSE(is_html_site_id("0123456789ABCDEF0123456789abcdef"));
    EXPECT_FALSE(is_html_site_id("0123"));
    EXPECT_FALSE(is_html_site_id("../3456789abcdef0123456789abcdef"));
}

// 场景:页面引用各类资源。期望:网页资源按扩展名给出类型(大小写无关);.env、私钥、
// 无扩展名文件不在白名单,直接 404,页面拿不到它们的字节。
TEST(HtmlSiteMime, ServesOnlyWebResourceTypes) {
    EXPECT_EQ(html_site_mime("css/a.CSS").value_or(""), "text/css; charset=utf-8");
    EXPECT_EQ(html_site_mime("app.mjs").value_or(""), "text/javascript; charset=utf-8");
    EXPECT_EQ(html_site_mime("fonts/x.woff2").value_or(""), "font/woff2");
    EXPECT_EQ(html_site_mime("logo.svg").value_or(""), "image/svg+xml");
    EXPECT_FALSE(html_site_mime(".env").has_value());
    EXPECT_FALSE(html_site_mime("keys/id_rsa").has_value());
    EXPECT_FALSE(html_site_mime("server.pem").has_value());
    EXPECT_FALSE(html_site_mime("dir.v2/README").has_value());
    EXPECT_TRUE(html_site_mime_is_document("text/html; charset=utf-8"));
    EXPECT_TRUE(html_site_mime_is_document("image/svg+xml"));
    EXPECT_FALSE(html_site_mime_is_document("text/css; charset=utf-8"));
}

// 场景:隔离框是不透明源,模块脚本与字体按跨源规则加载,fetch 也会带 Origin: null。
// 期望:只有子资源(script/style/font/image/媒体/worker)拿到跨源许可;fetch/XHR(empty)、
// JSON 模块与不带 Sec-Fetch-Dest 的请求拿不到 —— 页面脚本读不到文件内容。
TEST(HtmlSiteCors, AllowsSubresourcesButNotFetch) {
    for (const char* dest : {"script", "style", "font", "image", "video", "audio", "worker"}) {
        EXPECT_TRUE(html_site_allows_cors(dest)) << dest;
    }
    for (const char* dest : {"empty", "json", "document", "iframe", ""}) {
        EXPECT_FALSE(html_site_allows_cors(dest)) << dest;
    }
}

// 场景:静态兜底路由把 /api/html-site/... 原样转给站点处理。
// 期望:拆出站点号与剩余路径(不解码);前缀不对、没有站点号或只有站点号的都不认。
TEST(HtmlSiteUrl, SplitsIdAndRest) {
    const auto parts = split_html_site_url("/api/html-site/abc/css/a%20b.css");
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->id, "abc");
    EXPECT_EQ(parts->rest, "css/a%20b.css");
    EXPECT_FALSE(split_html_site_url("/api/html-site/abc").has_value());
    EXPECT_FALSE(split_html_site_url("/api/html-site//a.css").has_value());
    EXPECT_FALSE(split_html_site_url("/api/files/blob").has_value());
}

// 场景:中文文件名、空格经百分号编码后请求。期望:按 UTF-8 字节还原;
// 截断的转义、非十六进制、解码出 NUL 的请求整条拒绝。
TEST(HtmlSiteUrl, DecodesPercentEscapes) {
    EXPECT_EQ(decode_html_site_path("a%20b/%E9%A1%B5.css").value_or(""),
              std::string("a b/\xE9\xA1\xB5.css"));
    EXPECT_FALSE(decode_html_site_path("a%2").has_value());
    EXPECT_FALSE(decode_html_site_path("a%zz").has_value());
    EXPECT_FALSE(decode_html_site_path("a%00.css").has_value());
    // 入口路径编码后能原样解回来。
    const std::string entry = "docs/\xE6\x8A\xA5\xE5\x91\x8A #1.html";
    EXPECT_EQ(decode_html_site_path(encode_html_site_path(entry)).value_or(""), entry);
    EXPECT_EQ(encode_html_site_path("a b/c.html"), "a%20b/c.html");
}

// 场景:页面(或恶意页面)用 ../、编码过的 %2e%2e、反斜杠、盘符跳出根目录。
// 期望:根目录内的文件能解析;所有越界写法与根目录本身都返回空。
TEST_F(HtmlSiteTempDir, ResolvesOnlyFilesInsideRoot) {
    const fs::path site = root_ / "site";
    EXPECT_TRUE(resolve_html_site_file(site, "css/a.css").has_value());
    EXPECT_TRUE(resolve_html_site_file(site, "index.html").has_value());
    EXPECT_FALSE(resolve_html_site_file(site, "../secret.css").has_value());
    EXPECT_FALSE(resolve_html_site_file(site, "css/../../secret.css").has_value());
    EXPECT_FALSE(resolve_html_site_file(site, "./index.html").has_value());
    EXPECT_FALSE(resolve_html_site_file(site, "css//a.css").has_value());
    EXPECT_FALSE(resolve_html_site_file(site, "..\\secret.css").has_value());
    EXPECT_FALSE(resolve_html_site_file(site, "C:/Windows/win.ini").has_value());
    EXPECT_FALSE(resolve_html_site_file(site, "").has_value());
    // 解码后的 ".." 同样拒绝(serve_html_site 先解码再交给这里)。
    EXPECT_FALSE(resolve_html_site_file(
        site, decode_html_site_path("%2e%2e/secret.css").value_or("")).has_value());
}

// 场景:打开工作区里的 HTML,以及工作区外的 HTML。
// 期望:前者以工作区为根(页面能引用 ../css),入口是相对工作区的路径;后者只开放所在目录。
TEST_F(HtmlSiteTempDir, PlansWorkspaceRootOrFileDirectory) {
    const auto inside = plan_html_site(root_ / "site" / "index.html", root_);
    ASSERT_TRUE(inside.has_value());
    EXPECT_EQ(inside->entry, "site/index.html");
    EXPECT_TRUE(fs::equivalent(inside->root, root_));

    const auto outside = plan_html_site(root_ / "site" / "index.html", std::nullopt);
    ASSERT_TRUE(outside.has_value());
    EXPECT_EQ(outside->entry, "index.html");
    EXPECT_TRUE(fs::equivalent(outside->root, root_ / "site"));

    // 工作区与文件不相干时退回文件所在目录。
    const auto unrelated = plan_html_site(root_ / "site" / "index.html", root_ / "site" / "css");
    ASSERT_TRUE(unrelated.has_value());
    EXPECT_EQ(unrelated->entry, "index.html");
}
