// 覆盖 src/web/static_assets.cpp。一旦回归:
//   - MIME map 错配 → 浏览器拒载 .js / .woff2
//   - FileSystemAssetSource 路径穿越 → 服务器读到 ".." 之外的文件
//   - EmbeddedAssetSource 找不到 index.html → 整个前端起不来

#include <gtest/gtest.h>

#include "web/static_assets.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <future>

namespace fs = std::filesystem;
using acecode::web::EmbeddedAssetSource;
using acecode::web::FileSystemAssetSource;
using acecode::web::embedded_asset_map;
using acecode::web::mime_type_for_path;

// 场景: 各扩展名的 MIME 类型必须正确映射 — 浏览器靠 Content-Type 决定 parse 策略
TEST(StaticAssets, MimeTypeMap) {
    EXPECT_EQ(mime_type_for_path("foo.html"), "text/html; charset=utf-8");
    EXPECT_EQ(mime_type_for_path("a/b/foo.css"), "text/css; charset=utf-8");
    EXPECT_EQ(mime_type_for_path("foo.js"), "application/javascript; charset=utf-8");
    EXPECT_EQ(mime_type_for_path("foo.json"), "application/json; charset=utf-8");
    EXPECT_EQ(mime_type_for_path("icon.svg"), "image/svg+xml");
    EXPECT_EQ(mime_type_for_path("icon.png"), "image/png");
    EXPECT_EQ(mime_type_for_path("icon.ico"), "image/x-icon");
    EXPECT_EQ(mime_type_for_path("font.woff2"), "font/woff2");
    EXPECT_EQ(mime_type_for_path("doc.md"), "text/markdown; charset=utf-8");
}

// 场景: 未知扩展名 fallback octet-stream(让浏览器决定下载/打开)
TEST(StaticAssets, UnknownExtFallback) {
    EXPECT_EQ(mime_type_for_path("noext"), "application/octet-stream");
    EXPECT_EQ(mime_type_for_path("foo.xyz"), "application/octet-stream");
}

// 场景: build 期生成的嵌入 map 必须含 index.html(否则前端起不来)
// 注: 此测试假设 web/index.html 已写入(本仓库 add-web-chat-ui 任务 4.1 已落地)
TEST(StaticAssets, EmbeddedMapContainsIndexHtml) {
    const auto& m = embedded_asset_map();
    auto it = m.find("index.html");
    ASSERT_NE(it, m.end()) << "index.html 必须被嵌入 — 检查 web/index.html 与 acecode_embed_assets";
    EXPECT_GT(it->second.size, 0u);
}

// 场景: EmbeddedAssetSource 命中 index.html
TEST(StaticAssets, EmbeddedSourceLookup) {
    EmbeddedAssetSource src;
    auto r = src.lookup("index.html");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->content_type, "text/html; charset=utf-8");
    EXPECT_GT(r->size, 0u);
    EXPECT_NE(r->data, nullptr);
}

// 场景: 不存在的 path → nullopt(404)
TEST(StaticAssets, EmbeddedSourceMisses) {
    EmbeddedAssetSource src;
    EXPECT_FALSE(src.lookup("does-not-exist.js").has_value());
    EXPECT_FALSE(src.lookup("").has_value());
}

TEST(StaticAssets, GzipNegotiationRespectsQualityAndExplicitExclusions) {
    using acecode::web::accepts_gzip;
    for (const auto* header : {"gzip", "br, gzip", "GZip;Q=0.1", "*;q=1", "gzip; q=1.000"}) {
        EXPECT_TRUE(accepts_gzip(header)) << header;
    }
    for (const auto* header : {"", "br", "x-gzip", "gzip;q=0", "gzip;q=0.000, *;q=1",
                               "gzip;q=broken", "gzip;q=1.1", "gzip;q=0.0001", "*;q=0",
                               "gzip;q=0, gzip;q=1", "gzip;q"}) {
        EXPECT_FALSE(accepts_gzip(header)) << header;
    }
}

TEST(StaticAssets, EmbeddedGzipAndIdentityShareTheOriginalContent) {
    EmbeddedAssetSource src;
    const auto identity = src.lookup("index.html");
    const auto encoded = src.lookup("index.html", true);
    ASSERT_TRUE(identity);
    ASSERT_TRUE(encoded);
    const auto& stored = embedded_asset_map().at("index.html");
    EXPECT_EQ(identity->size, stored.decoded_size);
    EXPECT_FALSE(identity->gzip);
    EXPECT_EQ(encoded->gzip, stored.gzip);
    EXPECT_EQ(encoded->size, stored.size);
    EXPECT_EQ(encoded->data, stored.data);
    EXPECT_EQ(encoded->vary_accept_encoding, stored.gzip);
    EXPECT_EQ(identity->content_type, encoded->content_type);
    if (stored.gzip) {
        EXPECT_LT(encoded->size, identity->size);
        EXPECT_EQ(encoded->data[0], 0x1f);
        EXPECT_EQ(encoded->data[1], 0x8b);
        ASSERT_TRUE(identity->owned_buffer);
        EXPECT_NE(identity->owned_buffer->find("<html"), std::string::npos);
        auto concurrent = std::async(std::launch::async, [&src] { return src.lookup("index.html"); });
        EXPECT_EQ(concurrent.get()->owned_buffer, identity->owned_buffer);
    }
}

// 触发场景:浏览器带 If-None-Match 重新验证缓存。RFC 7232 §3.2 规定用弱比较:
// 列表里任意一项命中即可、`W/` 前缀忽略、`*` 匹配任何表示;没有验证器时不能回 304。
// 期望行为:只有 ETag 完全一致(忽略 W/)才算命中;gzip 表示的 `-gzip` 后缀不与
// identity 表示混淆,空 ETag 永远不命中。
TEST(StaticAssets, IfNoneMatchUsesWeakComparisonOverTheWholeList) {
    using acecode::web::etag_matches_if_none_match;
    EXPECT_TRUE(etag_matches_if_none_match("\"abc\"", "\"abc\""));
    EXPECT_TRUE(etag_matches_if_none_match("W/\"abc\"", "\"abc\""));
    EXPECT_TRUE(etag_matches_if_none_match("\"other\", \"abc\"", "\"abc\""));
    EXPECT_TRUE(etag_matches_if_none_match("  \"abc\"  ,\"other\"", "\"abc\""));
    EXPECT_TRUE(etag_matches_if_none_match("*", "\"abc\""));
    EXPECT_FALSE(etag_matches_if_none_match("\"abc-gzip\"", "\"abc\""));
    EXPECT_FALSE(etag_matches_if_none_match("\"abc\"", "\"abc-gzip\""));
    EXPECT_FALSE(etag_matches_if_none_match("", "\"abc\""));
    EXPECT_FALSE(etag_matches_if_none_match("\"other\"", "\"abc\""));
    EXPECT_FALSE(etag_matches_if_none_match("*", ""));
    EXPECT_FALSE(etag_matches_if_none_match("\"abc\"", ""));
}

// 触发场景:同一个嵌入资源被反复查询,且按 Accept-Encoding 分别取 gzip / identity 表示。
// 期望行为:ETag 是带引号的强验证器,同一表示多次查询恒定,两种表示只差 `-gzip` 后缀
// (浏览器按 Vary: Accept-Encoding 分开缓存,各自重新验证都能对上)。
TEST(StaticAssets, EmbeddedEtagIsStableAndDistinguishesEncodings) {
    EmbeddedAssetSource src;
    const auto identity = src.lookup("index.html");
    const auto again = src.lookup("index.html");
    const auto encoded = src.lookup("index.html", true);
    ASSERT_TRUE(identity);
    ASSERT_TRUE(again);
    ASSERT_TRUE(encoded);
    ASSERT_GE(identity->etag.size(), 3u);
    EXPECT_EQ(identity->etag.front(), '"');
    EXPECT_EQ(identity->etag.back(), '"');
    EXPECT_EQ(identity->etag, again->etag);
    if (encoded->gzip) {
        EXPECT_NE(encoded->etag, identity->etag);
        const std::string expected = identity->etag.substr(0, identity->etag.size() - 1) + "-gzip\"";
        EXPECT_EQ(encoded->etag, expected);
    } else {
        EXPECT_EQ(encoded->etag, identity->etag);
    }
}

namespace {

class FsSourceTest : public ::testing::Test {
protected:
    fs::path root;
    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        root = fs::temp_directory_path() / ("acecode_static_test_" + std::to_string(gen()));
        fs::create_directories(root);
        write_file(root / "foo.txt", "hello");
        fs::create_directories(root / "sub");
        write_file(root / "sub" / "bar.css", "body{}");
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    static void write_file(const fs::path& p, const std::string& content) {
        std::ofstream(p, std::ios::binary).write(content.data(), content.size());
    }
};

} // namespace

// 场景: FileSystemAssetSource 命中扁平 + 嵌套文件
TEST_F(FsSourceTest, LookupFiles) {
    FileSystemAssetSource src(root.string());
    auto r1 = src.lookup("foo.txt");
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(r1->data), r1->size), "hello");

    auto r2 = src.lookup("sub/bar.css");
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->content_type, "text/css; charset=utf-8");
}

// 触发场景:开发模式(web.static_dir)下文件被重新构建,大小或修改时间变化。
// 期望行为:磁盘资源的 ETag 带引号、同一文件未改时恒定、内容改写后变化,
// 否则浏览器拿旧 ETag 来问会错误地收到 304、一直看到旧前端。
TEST_F(FsSourceTest, EtagFollowsFileContent) {
    FileSystemAssetSource src(root.string());
    const auto before = src.lookup("foo.txt");
    const auto same = src.lookup("foo.txt");
    ASSERT_TRUE(before);
    ASSERT_TRUE(same);
    ASSERT_GE(before->etag.size(), 3u);
    EXPECT_EQ(before->etag.front(), '"');
    EXPECT_EQ(before->etag.back(), '"');
    EXPECT_EQ(before->etag, same->etag);

    write_file(root / "foo.txt", "hello, rebuilt");
    const auto after = src.lookup("foo.txt");
    ASSERT_TRUE(after);
    EXPECT_NE(after->etag, before->etag);
}

// 场景: 不存在的文件 → nullopt
TEST_F(FsSourceTest, Missing) {
    FileSystemAssetSource src(root.string());
    EXPECT_FALSE(src.lookup("nope.js").has_value());
}

// 场景: 路径穿越尝试(.. /etc/passwd 之类)→ nullopt
TEST_F(FsSourceTest, PathTraversalRejected) {
    FileSystemAssetSource src(root.string());
    EXPECT_FALSE(src.lookup("../foo.txt").has_value());
    EXPECT_FALSE(src.lookup("../../etc/passwd").has_value());
}
