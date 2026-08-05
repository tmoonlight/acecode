// 覆盖 src/tool/web_search/backend_router.{hpp,cpp}。
//
// 用 mock backend(继承 WebSearchBackend)注入受控响应,断言:
//   - resolve_active 在 cfg=auto 下根据 region 选 ddg/bing
//   - 显式 backend(bing_cn)覆盖 region(即使 region=Global 仍选 bing)
//   - bochaai / tavily 占位 → fallback 到 auto 行为(注意 cfg 字段是 string,
//     非法名会被 load_config 挡掉,这里只测占位 backend 的 fallback 行为)
//   - search_with_fallback:Network 错误 → 试对侧;成功后 active 切换 + 缓存更新 + notify
//   - Parse 错误不 fallback,直接返回
//   - 双 fail 返回最后一次错误,不 notify
//   - set_active 拒绝未注册 backend

#include <gtest/gtest.h>

#include "tool/web_search/backend_router.hpp"
#include "config/config.hpp"
#include "utils/state_file.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

using namespace acecode;
using namespace acecode::web_search;

namespace {

struct ParallelGate {
    std::mutex mu;
    std::condition_variable cv;
    int arrived = 0;
};

// 简易 mock backend,可注入 search() 的返回值。同时记录调用次数。
class MockBackend : public WebSearchBackend {
public:
    MockBackend(std::string n, std::variant<SearchResponse, SearchError> resp,
                std::shared_ptr<ParallelGate> gate = nullptr)
        : name_(std::move(n)), resp_(std::move(resp)), gate_(std::move(gate)) {}

    std::string name() const override { return name_; }
    bool requires_api_key() const override { return false; }
    std::variant<SearchResponse, SearchError>
    search(std::string_view, int, const std::atomic<bool>*) override {
        ++call_count;
        if (gate_) {
            std::unique_lock<std::mutex> lk(gate_->mu);
            ++gate_->arrived;
            gate_->cv.notify_all();
            if (!gate_->cv.wait_for(lk, std::chrono::seconds(1),
                                    [&] { return gate_->arrived == 3; })) {
                return SearchError{SearchError::Kind::Network,
                                   "parallel fan-out did not overlap", name_};
            }
        }
        return resp_;
    }

    std::string name_;
    std::variant<SearchResponse, SearchError> resp_;
    std::shared_ptr<ParallelGate> gate_;
    int call_count = 0;
};

// 构造一个简单的成功 response
SearchResponse make_resp(const std::string& backend_name, std::size_t n_hits) {
    SearchResponse r;
    r.backend_name = backend_name;
    for (std::size_t i = 0; i < n_hits; ++i) {
        r.hits.push_back({"title-" + std::to_string(i),
                          "https://x/" + backend_name + "/" + std::to_string(i),
                          "snip"});
    }
    return r;
}

class BackendRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto tmp = fs::temp_directory_path() /
                   ("acecode_router_test_" + std::to_string(std::rand()));
        fs::create_directories(tmp);
        path_ = (tmp / "state.json").string();
        set_state_file_path_for_test(path_);
    }
    void TearDown() override {
        set_state_file_path_for_test("");
        std::error_code ec;
        fs::remove_all(fs::path(path_).parent_path(), ec);
    }
    std::string path_;
};

// 提供两个独立的 mock 给同一个 router(注册后 router 持有 ownership,
// 测试用 raw 指针 alias 来读 call_count)
struct MockPair {
    MockBackend* ddg;
    MockBackend* bing;
};
MockPair install_mocks(BackendRouter& r,
                       std::variant<SearchResponse, SearchError> ddg_resp,
                       std::variant<SearchResponse, SearchError> bing_resp) {
    auto ddg_owned = std::make_unique<MockBackend>("duckduckgo", std::move(ddg_resp));
    auto bing_owned = std::make_unique<MockBackend>("bing_cn", std::move(bing_resp));
    MockBackend* ddg_ptr = ddg_owned.get();
    MockBackend* bing_ptr = bing_owned.get();
    r.register_backend(std::move(ddg_owned));
    r.register_backend(std::move(bing_owned));
    return {ddg_ptr, bing_ptr};
}

} // namespace

// 场景:cfg=auto + region=Global → active = duckduckgo
TEST_F(BackendRouterTest, AutoGlobalSelectsDdg) {
    WebSearchConfig cfg;
    cfg.backend = "auto";
    BackendRouter r(cfg);
    install_mocks(r, make_resp("duckduckgo", 0), make_resp("bing_cn", 0));
    r.resolve_active(Region::Global);
    EXPECT_EQ(r.active_name(), "duckduckgo");
}

// 场景:cfg=auto + region=Cn → active = bing_cn
TEST_F(BackendRouterTest, AutoCnSelectsBing) {
    WebSearchConfig cfg;
    cfg.backend = "auto";
    BackendRouter r(cfg);
    install_mocks(r, make_resp("duckduckgo", 0), make_resp("bing_cn", 0));
    r.resolve_active(Region::Cn);
    EXPECT_EQ(r.active_name(), "bing_cn");
}

// 场景:cfg=auto + region=Unknown → 悲观默认 bing_cn
TEST_F(BackendRouterTest, AutoUnknownSelectsBing) {
    WebSearchConfig cfg;
    cfg.backend = "auto";
    BackendRouter r(cfg);
    install_mocks(r, make_resp("duckduckgo", 0), make_resp("bing_cn", 0));
    r.resolve_active(Region::Unknown);
    EXPECT_EQ(r.active_name(), "bing_cn");
}

TEST_F(BackendRouterTest, DefaultConfigSelectsParallel) {
    WebSearchConfig cfg;
    BackendRouter r(cfg);
    install_mocks(r, make_resp("duckduckgo", 0), make_resp("bing_cn", 0));
    r.register_backend(std::make_unique<MockBackend>("rss", make_resp("rss", 2)));
    r.resolve_active(Region::Global);
    EXPECT_EQ(r.active_name(), "parallel");
}

TEST_F(BackendRouterTest, EmptyRssFallsBackByRegionWithoutChangingActive) {
    WebSearchConfig cfg;
    cfg.backend = "rss";
    BackendRouter r(cfg);
    auto mocks = install_mocks(r, make_resp("duckduckgo", 2), make_resp("bing_cn", 2));
    auto rss = std::make_unique<MockBackend>("rss", make_resp("rss", 0));
    auto* rss_raw = rss.get();
    r.register_backend(std::move(rss));
    r.resolve_active(Region::Global);

    auto out = r.search_with_fallback("broad query", 2, nullptr, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(out));
    EXPECT_EQ(std::get<SearchResponse>(out).backend_name, "duckduckgo");
    EXPECT_EQ(rss_raw->call_count, 1);
    EXPECT_EQ(mocks.ddg->call_count, 1);
    EXPECT_EQ(mocks.bing->call_count, 0);
    EXPECT_EQ(r.active_name(), "rss");
}

TEST_F(BackendRouterTest, RssNetworkErrorFallsBackToBingInChinaWithoutChangingActive) {
    WebSearchConfig cfg;
    cfg.backend = "rss";
    BackendRouter r(cfg);
    auto mocks = install_mocks(r, make_resp("duckduckgo", 2), make_resp("bing_cn", 2));
    r.register_backend(std::make_unique<MockBackend>(
        "rss", SearchError{SearchError::Kind::Network, "service down", "rss"}));
    r.resolve_active(Region::Cn);

    auto out = r.search_with_fallback("query", 2, nullptr, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(out));
    EXPECT_EQ(std::get<SearchResponse>(out).backend_name, "bing_cn");
    EXPECT_EQ(mocks.ddg->call_count, 0);
    EXPECT_EQ(mocks.bing->call_count, 1);
    EXPECT_EQ(r.active_name(), "rss");
}

TEST_F(BackendRouterTest, RssParseErrorDoesNotFallback) {
    WebSearchConfig cfg;
    cfg.backend = "rss";
    BackendRouter r(cfg);
    auto mocks = install_mocks(r, make_resp("duckduckgo", 1), make_resp("bing_cn", 1));
    r.register_backend(std::make_unique<MockBackend>(
        "rss", SearchError{SearchError::Kind::Parse, "bad payload", "rss"}));
    r.resolve_active(Region::Global);

    auto out = r.search_with_fallback("q", 5, nullptr, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    EXPECT_EQ(std::get<SearchError>(out).kind, SearchError::Kind::Parse);
    EXPECT_EQ(mocks.ddg->call_count, 0);
}

TEST_F(BackendRouterTest, RssRateLimitFallsBackWithoutChangingActive) {
    WebSearchConfig cfg;
    cfg.backend = "rss";
    BackendRouter r(cfg);
    auto mocks = install_mocks(r, make_resp("duckduckgo", 1), make_resp("bing_cn", 1));
    r.register_backend(std::make_unique<MockBackend>(
        "rss", SearchError{SearchError::Kind::RateLimited, "limited", "rss"}));
    r.resolve_active(Region::Global);

    auto out = r.search_with_fallback("q", 5, nullptr, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(out));
    EXPECT_EQ(std::get<SearchResponse>(out).backend_name, "duckduckgo");
    EXPECT_EQ(mocks.ddg->call_count, 1);
    EXPECT_EQ(r.active_name(), "rss");
}

TEST_F(BackendRouterTest, StickyLegacyFallbackUpdatesRssFallbackRegion) {
    WebSearchConfig cfg;
    cfg.backend = "auto";
    BackendRouter r(cfg);
    auto mocks = install_mocks(
        r,
        SearchError{SearchError::Kind::Network, "blocked", "duckduckgo"},
        make_resp("bing_cn", 1));
    r.register_backend(std::make_unique<MockBackend>("rss", make_resp("rss", 0)));
    r.resolve_active(Region::Global);

    auto legacy = r.search_with_fallback("q", 5, nullptr, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(legacy));
    ASSERT_EQ(r.active_name(), "bing_cn");
    ASSERT_TRUE(r.set_active("rss"));

    auto rss = r.search_with_fallback("q", 5, nullptr, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(rss));
    EXPECT_EQ(std::get<SearchResponse>(rss).backend_name, "bing_cn");
    EXPECT_EQ(mocks.ddg->call_count, 1);
    EXPECT_EQ(mocks.bing->call_count, 2);
}

TEST_F(BackendRouterTest, ParallelStartsAllThreeBackendsConcurrently) {
    WebSearchConfig cfg;
    BackendRouter r(cfg);
    auto gate = std::make_shared<ParallelGate>();
    auto rss = std::make_unique<MockBackend>("rss", make_resp("rss", 1), gate);
    auto ddg = std::make_unique<MockBackend>(
        "duckduckgo", make_resp("duckduckgo", 1), gate);
    auto bing = std::make_unique<MockBackend>("bing_cn", make_resp("bing_cn", 1), gate);
    auto* rss_raw = rss.get();
    auto* ddg_raw = ddg.get();
    auto* bing_raw = bing.get();
    r.register_backend(std::move(rss));
    r.register_backend(std::move(ddg));
    r.register_backend(std::move(bing));
    r.resolve_active(Region::Global);

    auto out = r.search_with_fallback("query", 3, nullptr, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(out));
    const auto& response = std::get<SearchResponse>(out);
    EXPECT_EQ(response.backend_name, "parallel");
    EXPECT_EQ(response.hits.size(), 3u);
    EXPECT_EQ(gate->arrived, 3);
    EXPECT_EQ(rss_raw->call_count, 1);
    EXPECT_EQ(ddg_raw->call_count, 1);
    EXPECT_EQ(bing_raw->call_count, 1);
}

TEST_F(BackendRouterTest, ParallelInterleavesDeduplicatesAndEnforcesTotalLimit) {
    WebSearchConfig cfg;
    BackendRouter r(cfg);

    SearchResponse rss;
    rss.backend_name = "rss";
    rss.hits = {
        {"rss-shared", "https://Example.com/shared/", "rss shared"},
        {"rss-second", "https://rss.example/second", "rss second"},
    };
    SearchResponse ddg;
    ddg.backend_name = "duckduckgo";
    ddg.hits = {
        {"ddg-duplicate", "https://example.com/shared#top", "duplicate"},
        {"ddg-second", "https://ddg.example/second", "ddg second"},
    };
    SearchResponse bing;
    bing.backend_name = "bing_cn";
    bing.hits = {
        {"bing-first", "https://bing.example/first", "bing first"},
        {"bing-second", "https://bing.example/second", "bing second"},
    };
    r.register_backend(std::make_unique<MockBackend>("rss", std::move(rss)));
    r.register_backend(std::make_unique<MockBackend>("duckduckgo", std::move(ddg)));
    r.register_backend(std::make_unique<MockBackend>("bing_cn", std::move(bing)));
    r.resolve_active(Region::Global);

    auto out = r.search_with_fallback("query", 5, nullptr, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(out));
    const auto& hits = std::get<SearchResponse>(out).hits;
    ASSERT_EQ(hits.size(), 5u);
    EXPECT_EQ(hits[0].title, "rss-shared");
    EXPECT_EQ(hits[1].title, "ddg-second");
    EXPECT_EQ(hits[2].title, "bing-first");
    EXPECT_EQ(hits[3].title, "rss-second");
    EXPECT_EQ(hits[4].title, "bing-second");
    EXPECT_EQ(hits[0].backend_name, "rss");
    EXPECT_EQ(hits[1].backend_name, "duckduckgo");
    EXPECT_EQ(hits[2].backend_name, "bing_cn");
}

TEST_F(BackendRouterTest, ParallelPartialFailureReturnsResultsAndWarnings) {
    WebSearchConfig cfg;
    BackendRouter r(cfg);
    r.register_backend(std::make_unique<MockBackend>(
        "rss", SearchError{SearchError::Kind::Network, "rss down", "rss"}));
    r.register_backend(std::make_unique<MockBackend>(
        "duckduckgo", make_resp("duckduckgo", 2)));
    r.register_backend(std::make_unique<MockBackend>(
        "bing_cn", SearchError{SearchError::Kind::RateLimited, "bing 429", "bing_cn"}));
    r.resolve_active(Region::Global);

    std::string notification;
    auto out = r.search_with_fallback(
        "query", 5, nullptr,
        [&](const std::string& message) { notification = message; });
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(out));
    const auto& response = std::get<SearchResponse>(out);
    EXPECT_EQ(response.hits.size(), 2u);
    ASSERT_EQ(response.warnings.size(), 2u);
    EXPECT_NE(response.warnings[0].find("rss"), std::string::npos);
    EXPECT_NE(response.warnings[1].find("bing_cn"), std::string::npos);
    EXPECT_NE(notification.find("rss"), std::string::npos);
    EXPECT_NE(notification.find("bing_cn"), std::string::npos);
    EXPECT_EQ(r.active_name(), "parallel");
}

TEST_F(BackendRouterTest, ParallelAllFailuresReturnAggregateError) {
    WebSearchConfig cfg;
    BackendRouter r(cfg);
    r.register_backend(std::make_unique<MockBackend>(
        "rss", SearchError{SearchError::Kind::Parse, "bad json", "rss"}));
    r.register_backend(std::make_unique<MockBackend>(
        "duckduckgo", SearchError{SearchError::Kind::Network, "ddg down", "duckduckgo"}));
    r.register_backend(std::make_unique<MockBackend>(
        "bing_cn", SearchError{SearchError::Kind::RateLimited, "bing 429", "bing_cn"}));
    r.resolve_active(Region::Cn);

    auto out = r.search_with_fallback("query", 5, nullptr, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    const auto& error = std::get<SearchError>(out);
    EXPECT_EQ(error.backend_name, "parallel");
    EXPECT_NE(error.message.find("rss"), std::string::npos);
    EXPECT_NE(error.message.find("duckduckgo"), std::string::npos);
    EXPECT_NE(error.message.find("bing_cn"), std::string::npos);
    EXPECT_EQ(r.active_name(), "parallel");
}

// 场景:显式 backend=bing_cn,region=Global → 仍选 bing_cn(显式覆盖 region)
TEST_F(BackendRouterTest, ExplicitBackendOverridesRegion) {
    WebSearchConfig cfg;
    cfg.backend = "bing_cn";
    BackendRouter r(cfg);
    install_mocks(r, make_resp("duckduckgo", 0), make_resp("bing_cn", 0));
    r.resolve_active(Region::Global);
    EXPECT_EQ(r.active_name(), "bing_cn");
}

// 场景:配置 backend=tavily(未实现),fallback 到 auto 行为
TEST_F(BackendRouterTest, UnimplementedBackendFallsBackToAuto) {
    WebSearchConfig cfg;
    cfg.backend = "tavily";
    BackendRouter r(cfg);
    install_mocks(r, make_resp("duckduckgo", 0), make_resp("bing_cn", 0));
    r.resolve_active(Region::Global);
    EXPECT_EQ(r.active_name(), "duckduckgo"); // tavily 未实现,看作 auto
}

// 场景:Network 错误触发 fallback,成功后 active 切到对侧 + 缓存更新 + notify
TEST_F(BackendRouterTest, NetworkErrorFallbackToOpposite) {
    WebSearchConfig cfg;
    cfg.backend = "auto";
    BackendRouter r(cfg);
    auto mocks = install_mocks(
        r,
        SearchError{SearchError::Kind::Network, "ddg unreachable", "duckduckgo"},
        make_resp("bing_cn", 3));
    r.resolve_active(Region::Global); // active=ddg

    std::string captured_notify;
    auto notify = [&](const std::string& m) { captured_notify = m; };

    auto out = r.search_with_fallback("rust", 3, nullptr, notify);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(out));
    EXPECT_EQ(std::get<SearchResponse>(out).backend_name, "bing_cn");
    EXPECT_EQ(mocks.ddg->call_count, 1);
    EXPECT_EQ(mocks.bing->call_count, 1);
    EXPECT_EQ(r.active_name(), "bing_cn");
    EXPECT_NE(captured_notify.find("Switched to bing_cn"), std::string::npos);

    auto cache = read_web_search_region_cache();
    ASSERT_TRUE(cache.has_value());
    EXPECT_EQ(cache->region, "cn");
}

// 场景:Parse 错误不 fallback,直接返回 — 对侧 backend 不应被调用
TEST_F(BackendRouterTest, ParseErrorDoesNotFallback) {
    WebSearchConfig cfg;
    cfg.backend = "auto";
    BackendRouter r(cfg);
    auto mocks = install_mocks(
        r,
        SearchError{SearchError::Kind::Parse, "html changed", "duckduckgo"},
        make_resp("bing_cn", 3));
    r.resolve_active(Region::Global); // active=ddg

    bool notify_fired = false;
    auto out = r.search_with_fallback("x", 3, nullptr,
                                       [&](const std::string&) { notify_fired = true; });
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    EXPECT_EQ(std::get<SearchError>(out).kind, SearchError::Kind::Parse);
    EXPECT_EQ(mocks.ddg->call_count, 1);
    EXPECT_EQ(mocks.bing->call_count, 0); // 没被调
    EXPECT_FALSE(notify_fired);
    EXPECT_EQ(r.active_name(), "duckduckgo"); // 没切
}

// 场景:RateLimited 同样不 fallback
TEST_F(BackendRouterTest, RateLimitedDoesNotFallback) {
    WebSearchConfig cfg;
    cfg.backend = "auto";
    BackendRouter r(cfg);
    auto mocks = install_mocks(
        r,
        SearchError{SearchError::Kind::RateLimited, "429", "duckduckgo"},
        make_resp("bing_cn", 3));
    r.resolve_active(Region::Global);
    auto out = r.search_with_fallback("x", 3, nullptr, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    EXPECT_EQ(std::get<SearchError>(out).kind, SearchError::Kind::RateLimited);
    EXPECT_EQ(mocks.bing->call_count, 0);
}

// 场景:双 fail 返回最后一次错误(fallback 的错误),active 不变,无 notify
TEST_F(BackendRouterTest, BothFailReturnsLastError) {
    WebSearchConfig cfg;
    cfg.backend = "auto";
    BackendRouter r(cfg);
    auto mocks = install_mocks(
        r,
        SearchError{SearchError::Kind::Network, "ddg-fail", "duckduckgo"},
        SearchError{SearchError::Kind::Network, "bing-fail", "bing_cn"});
    r.resolve_active(Region::Global);

    bool notify_fired = false;
    auto out = r.search_with_fallback("x", 3, nullptr,
                                       [&](const std::string&) { notify_fired = true; });
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    EXPECT_EQ(std::get<SearchError>(out).backend_name, "bing_cn");
    EXPECT_EQ(std::get<SearchError>(out).message, "bing-fail");
    EXPECT_FALSE(notify_fired);
    EXPECT_EQ(r.active_name(), "duckduckgo"); // 没切
    EXPECT_EQ(mocks.ddg->call_count, 1);
    EXPECT_EQ(mocks.bing->call_count, 1);
}

// 场景:set_active 接受已注册名和虚拟 parallel 模式,拒绝未知名
TEST_F(BackendRouterTest, SetActiveValidatesRegistration) {
    WebSearchConfig cfg;
    cfg.backend = "auto";
    BackendRouter r(cfg);
    install_mocks(r, make_resp("duckduckgo", 0), make_resp("bing_cn", 0));
    r.resolve_active(Region::Global);
    EXPECT_TRUE(r.set_active("bing_cn"));
    EXPECT_EQ(r.active_name(), "bing_cn");
    EXPECT_TRUE(r.set_active("parallel"));
    EXPECT_EQ(r.active_name(), "parallel");
    EXPECT_FALSE(r.set_active("yandex")); // 未注册
    EXPECT_EQ(r.active_name(), "parallel"); // 不变
}

// 场景:status_snapshot 包含必要字段
TEST_F(BackendRouterTest, StatusSnapshotShape) {
    WebSearchConfig cfg;
    cfg.backend = "auto";
    BackendRouter r(cfg);
    install_mocks(r, make_resp("duckduckgo", 0), make_resp("bing_cn", 0));
    r.resolve_active(Region::Global);
    auto j = r.status_snapshot(Region::Global);
    EXPECT_EQ(j["active_backend"], "duckduckgo");
    EXPECT_EQ(j["config_backend"], "auto");
    EXPECT_EQ(j["region"], "global");
    EXPECT_TRUE(j["enabled"].get<bool>());
    ASSERT_TRUE(j["registered"].is_array());
    EXPECT_EQ(j["registered"].size(), 2u);
}
