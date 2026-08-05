#include <gtest/gtest.h>

#include "tool/web_search/rss_search_backend.hpp"
#include "utils/http_url_validation.hpp"

#include <atomic>
#include <cstdlib>
#include <string>

using namespace acecode;
using namespace acecode::web_search;

namespace {

HttpFetchFn canned(long status, std::string body, std::string error = "") {
    return [status, body = std::move(body), error = std::move(error)](
               const std::string&, int) {
        return HttpProbeResult{status, body, error};
    };
}

const char* kValidPayload = R"JSON({
  "query": "Kubernetes",
  "backend": "rss",
  "took_ms": 8.5,
  "results": [
    {
      "title": "Kubernetes v1.37 Sneak Peek",
      "url": "https://kubernetes.io/blog/2026/07/31/kubernetes-v1-37-sneak-peek/",
      "snippet": "Release preview",
      "source": "Kubernetes Blog",
      "author": null,
      "published_at": "2026-07-31T16:00:00Z"
    },
    {
      "title": "Unsafe entry",
      "url": "file:///etc/passwd",
      "snippet": "must be discarded",
      "source": "bad",
      "published_at": null
    }
  ]
})JSON";

} // namespace

TEST(RssSearchParser, ParsesMetadataAndRejectsUnsafeUrls) {
    auto parsed = parse_rss_search_json(kValidPayload, 5);
    ASSERT_TRUE(std::holds_alternative<std::vector<SearchHit>>(parsed));
    const auto& hits = std::get<std::vector<SearchHit>>(parsed);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].title, "Kubernetes v1.37 Sneak Peek");
    EXPECT_EQ(hits[0].snippet, "Release preview");
    EXPECT_EQ(hits[0].source, "Kubernetes Blog");
    EXPECT_EQ(hits[0].published_at, "2026-07-31T16:00:00Z");
}

TEST(RssSearchParser, ValidEmptyResultsAreSuccessful) {
    auto parsed = parse_rss_search_json(R"({"query":"none","results":[]})", 5);
    ASSERT_TRUE(std::holds_alternative<std::vector<SearchHit>>(parsed));
    EXPECT_TRUE(std::get<std::vector<SearchHit>>(parsed).empty());
}

TEST(RssSearchParser, MalformedOrWrongShapeReturnsParseError) {
    for (const std::string& payload : {
             "not json", "[]", R"({"results":{}})", R"({"query":"x"})"}) {
        auto parsed = parse_rss_search_json(payload, 5);
        ASSERT_TRUE(std::holds_alternative<SearchError>(parsed)) << payload;
        EXPECT_EQ(std::get<SearchError>(parsed).kind, SearchError::Kind::Parse);
        EXPECT_EQ(std::get<SearchError>(parsed).backend_name, "rss");
    }
}

TEST(RssSearchParser, SkipsMalformedEntriesAndRespectsLimit) {
    const std::string payload = R"({"results":[
      {"title":12,"url":"https://bad.example"},
      {"title":"one","url":"https://one.example","snippet":3},
      {"title":"two","url":"http://two.example"},
      {"title":"three","url":"https://three.example"}
    ]})";
    auto parsed = parse_rss_search_json(payload, 2);
    ASSERT_TRUE(std::holds_alternative<std::vector<SearchHit>>(parsed));
    const auto& hits = std::get<std::vector<SearchHit>>(parsed);
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].title, "one");
    EXPECT_TRUE(hits[0].snippet.empty());
    EXPECT_EQ(hits[1].title, "two");
}

TEST(RssSearchBackend, BuildsEncodedUrlAndNormalizesTrailingSlash) {
    std::string captured_url;
    int captured_timeout = 0;
    auto fetch = [&](const std::string& url, int timeout) {
        captured_url = url;
        captured_timeout = timeout;
        return HttpProbeResult{200, R"({"results":[]})", ""};
    };
    RssSearchBackend backend("https://search.example/base/", 4321, fetch);
    auto out = backend.search(u8"Claude Code 更新", 7, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(out));
    EXPECT_EQ(captured_url,
              "https://search.example/base/v1/search?q=Claude%20Code%20%E6%9B%B4%E6%96%B0&limit=7");
    EXPECT_EQ(captured_timeout, 4321);
}

TEST(RssSearchBackend, MapsNetworkRateLimitAndServerErrors) {
    RssSearchBackend network("https://x", 1000, canned(0, "", "timed out"));
    auto n = network.search("x", 5, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(n));
    EXPECT_EQ(std::get<SearchError>(n).kind, SearchError::Kind::Network);

    RssSearchBackend limited("https://x", 1000, canned(429, ""));
    auto l = limited.search("x", 5, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(l));
    EXPECT_EQ(std::get<SearchError>(l).kind, SearchError::Kind::RateLimited);

    RssSearchBackend server("https://x", 1000, canned(503, ""));
    auto s = server.search("x", 5, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(s));
    EXPECT_EQ(std::get<SearchError>(s).kind, SearchError::Kind::Network);
}

TEST(RssSearchBackend, AbortBeforeFetchDoesNotCallNetwork) {
    bool called = false;
    auto fetch = [&](const std::string&, int) {
        called = true;
        return HttpProbeResult{200, kValidPayload, ""};
    };
    RssSearchBackend backend("https://x", 1000, fetch);
    std::atomic<bool> abort{true};
    auto out = backend.search("x", 5, &abort);
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    EXPECT_NE(std::get<SearchError>(out).message.find("aborted"), std::string::npos);
    EXPECT_FALSE(called);
}

TEST(RssSearchBackend, EnforcesHostedQueryLengthByUnicodeCodepoint) {
    int calls = 0;
    auto fetch = [&](const std::string&, int) {
        ++calls;
        return HttpProbeResult{200, R"({"results":[]})", ""};
    };
    RssSearchBackend backend("https://x", 1000, fetch);

    const std::string exactly_200(200, 'a');
    EXPECT_TRUE(std::holds_alternative<SearchResponse>(
        backend.search(exactly_200, 5, nullptr)));
    EXPECT_EQ(calls, 1);

    std::string chinese_201;
    for (int i = 0; i < 201; ++i) chinese_201 += u8"搜";
    auto out = backend.search(chinese_201, 5, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    EXPECT_NE(std::get<SearchError>(out).message.find("200"), std::string::npos);
    EXPECT_EQ(calls, 1);
}

TEST(RssSearchBackend, RejectsUnsafeBaseUrlsBeforeFetch) {
    EXPECT_TRUE(utils::is_valid_http_base_url("https://search.example/base"));
    EXPECT_TRUE(utils::is_valid_http_base_url("http://127.0.0.1:8000/base"));
    EXPECT_FALSE(utils::is_valid_http_base_url("http://search.example/base"));
    EXPECT_FALSE(utils::is_valid_http_base_url("https://user:secret@example.com/base"));
    EXPECT_FALSE(utils::is_valid_http_base_url("https://example.com/base?token=x"));
    EXPECT_FALSE(utils::is_valid_http_base_url("https://example.com/base#fragment"));

    bool called = false;
    RssSearchBackend backend("https://example.com/base?token=x", 1000,
                             [&](const std::string&, int) {
                                 called = true;
                                 return HttpProbeResult{200, R"({"results":[]})", ""};
                             });
    auto out = backend.search("x", 5, nullptr);
    EXPECT_TRUE(std::holds_alternative<SearchError>(out));
    EXPECT_FALSE(called);
}

TEST(RssSearchBackend, OversizedResponseIsRejected) {
    std::string huge(2 * 1024 * 1024 + 1, 'x');
    RssSearchBackend backend("https://x", 1000, canned(200, std::move(huge)));
    auto out = backend.search("x", 5, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    EXPECT_EQ(std::get<SearchError>(out).kind, SearchError::Kind::Parse);
    EXPECT_NE(std::get<SearchError>(out).message.find("too large"), std::string::npos);
}

TEST(RssSearchBackendLive, HostedEndpointReturnsSearchableResults) {
    if (std::getenv("ACECODE_RUN_LIVE_RSS_TEST") == nullptr) {
        GTEST_SKIP() << "set ACECODE_RUN_LIVE_RSS_TEST=1 to call the hosted service";
    }
    RssSearchBackend backend("https://ge.bigjuan.xyz/rss-search", 8000);
    auto out = backend.search("Kubernetes", 3, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(out))
        << std::get<SearchError>(out).message;
    const auto& response = std::get<SearchResponse>(out);
    EXPECT_EQ(response.backend_name, "rss");
    ASSERT_FALSE(response.hits.empty());
    EXPECT_TRUE(response.hits.front().url.rfind("http", 0) == 0);
}
