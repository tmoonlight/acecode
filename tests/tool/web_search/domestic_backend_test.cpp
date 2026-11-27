#include "tool/web_search/domestic_backend.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <thread>

using namespace acecode;
using namespace acecode::web_search;

namespace {
using Json = nlohmann::json;

std::string results(const Json& items) {
    return Json{{"code", 0}, {"data", {{"results", items}}}}.dump();
}

constexpr auto kPage = R"HTML(
<a href="https://menu.example/">Menu</a>
<script>const example = '<li class="res-list"><h3><a href="https://script/">X</a></h3></li>';</script>
<li class="res-list"><div class="g-mohe"><h3><a href="https://card/">Card</a></h3></div></li>
<li class="res-list"><h3><a href="https://WWW.SO.COM:443/link?m=1">Wrapper</a></h3></li>
<li class="res-list"><h3><a href="javascript:void(0)">Invalid</a></h3></li>
<li class="res-list"><h3><a href="https://ONE.example/#top">Duplicate</a></h3></li>
<li class='res-list result'><h3><a href='https://www.so.com/link?m=2'
 data-mdurl = 'https://two.example/?a=1&amp;b=2'><em>办事</em>指南</a></h3>
 <div class="res-comm-con">公共 <em>服务</em></div></li>
<li class="res-list"><h3><a href=https://three.example/>Third</a></h3>
 <p class="res-desc">Final snippet</p></li>)HTML";

TEST(DomesticSearch, CompletePrimarySkips360) {
    const std::string query = "中文 & \"查询\"";
    int requests = 0;
    DomesticBackend backend(8000, [&](const auto& url, const auto& body, int timeout, auto) {
        ++requests;
        EXPECT_EQ(url, "https://api.anysearch.com/v1/search");
        const Json expected{{"query", query}, {"zone", "cn"}, {"language", "zh-CN"}, {"max_results", 1}};
        EXPECT_EQ(Json::parse(body), expected);
        EXPECT_EQ(timeout, 4000);
        return HttpProbeResult{200, results({{{"title", "  中文  "}, {"url", "https://one.example/"}}})};
    });
    auto outcome = backend.search(query, 1, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(outcome));
    const auto& hits = std::get<SearchResponse>(outcome).hits;
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].title, "中文");
    EXPECT_TRUE(hits[0].snippet.empty());
    EXPECT_EQ(hits[0].backend_name, "anysearch");
    EXPECT_EQ(requests, 1);
}

TEST(DomesticSearch, TopsUpUniqueOrganicResultsInOrder) {
    DomesticBackend backend(8000, [](const auto& url, const auto& body, int timeout, auto) {
        if (body.empty()) {
            EXPECT_EQ(url, "https://www.so.com/s?q=%E4%B8%AD%20%26%20x");
            EXPECT_GT(timeout, 0);
            EXPECT_LE(timeout, 8000);
            return HttpProbeResult{200, kPage};
        }
        return HttpProbeResult{200, results({
            {{"title", "Bad"}, {"url", "https:///"}},
            {{"title", 42}, {"url", "https://ignored.example"}},
            {{"title", "First"}, {"url", "https://one.example"}},
            {{"title", "First again"}, {"url", "https://ONE.example/#top"}}
        })};
    });
    auto outcome = backend.search("中 & x", 3, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(outcome));
    const auto& hits = std::get<SearchResponse>(outcome).hits;
    ASSERT_EQ(hits.size(), 3u);
    EXPECT_EQ(hits[0].title, "First");
    EXPECT_EQ(hits[1].url, "https://two.example/?a=1&b=2");
    EXPECT_EQ(hits[1].title, "办事指南");
    EXPECT_EQ(hits[1].snippet, "公共 服务");
    EXPECT_EQ(hits[1].backend_name, "so360");
    EXPECT_EQ(hits[2].url, "https://three.example/");
    EXPECT_EQ(hits[2].snippet, "Final snippet");
}

TEST(DomesticSearch, FailedPrimaryFallsBackWithoutLeakingResponseBody) {
    for (const int status : {0, 200, 402, 429, 503}) {
        SCOPED_TRACE(status);
        DomesticBackend backend(8000, [status](const auto&, const auto& body, int, auto) {
            return body.empty() ? HttpProbeResult{200, kPage}
                                : HttpProbeResult{status, "credential-sentinel", "secret-diagnostic"};
        });
        auto outcome = backend.search("query", 1, nullptr);
        ASSERT_TRUE(std::holds_alternative<SearchResponse>(outcome));
        const auto& response = std::get<SearchResponse>(outcome);
        ASSERT_EQ(response.hits.size(), 1u);
        EXPECT_EQ(response.hits[0].backend_name, "so360");
        ASSERT_EQ(response.warnings.size(), 1u);
        EXPECT_EQ(response.warnings[0].find("credential"), std::string::npos);
        EXPECT_EQ(response.warnings[0].find("secret"), std::string::npos);
    }
}

TEST(DomesticSearch, FailedSupplementPreservesPrimaryAndReportsChallenge) {
    DomesticBackend backend(8000, [](const auto&, const auto& body, int, auto) {
        return body.empty() ? HttpProbeResult{200, "<title>360搜索_访问验证</title>"}
                            : HttpProbeResult{200, results({{{"title", "First"}, {"url", "https://one.example/"}}})};
    });
    auto outcome = backend.search("query", 2, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(outcome));
    const auto& response = std::get<SearchResponse>(outcome);
    ASSERT_EQ(response.hits.size(), 1u);
    EXPECT_EQ(response.hits[0].backend_name, "anysearch");
    ASSERT_EQ(response.warnings.size(), 1u);
    EXPECT_NE(response.warnings[0].find("verification"), std::string::npos);
}

TEST(DomesticSearch, EmptyPrimaryAndUnparseablePageReturnError) {
    DomesticBackend backend(8000, [](const auto&, const auto& body, int, auto) {
        return HttpProbeResult{200, body.empty() ? "<title>search</title>" : results(Json::array())};
    });
    auto outcome = backend.search("query", 5, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(outcome));
    EXPECT_NE(std::get<SearchError>(outcome).message.find("so360"), std::string::npos);
}

TEST(DomesticSearch, SlowPrimaryConsumesSupplementBudget) {
    int requests = 0;
    DomesticBackend backend(1000, [&](const auto&, const auto& body, int timeout, auto) {
        ++requests;
        if (!body.empty()) {
            EXPECT_EQ(timeout, 500);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            return HttpProbeResult{};
        }
        EXPECT_GT(timeout, 0);
        EXPECT_LE(timeout, 975);
        return HttpProbeResult{200, kPage};
    });
    EXPECT_TRUE(std::holds_alternative<SearchResponse>(backend.search("query", 1, nullptr)));
    EXPECT_EQ(requests, 2);
}

TEST(DomesticSearch, CancellationStopsNewRequests) {
    int requests = 0;
    std::atomic<bool> abort{true};
    DomesticBackend backend(8000, [&](const auto&, const auto&, int, const auto* flag) {
        EXPECT_EQ(flag, &abort);
        ++requests;
        abort.store(true);
        return HttpProbeResult{429, {}};
    });
    EXPECT_TRUE(std::holds_alternative<SearchError>(backend.search("query", 2, &abort)));
    EXPECT_EQ(requests, 0);
    abort.store(false);
    EXPECT_TRUE(std::holds_alternative<SearchError>(backend.search("query", 2, &abort)));
    EXPECT_EQ(requests, 1);
}
} // namespace
