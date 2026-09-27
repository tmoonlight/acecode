// 覆盖 src/utils/open_url.cpp 的 URL 打开(add-tui-hyperlinks 5.1/5.4)。
//
// 关键保证:
//   - 仅 http/https 放行;控制字符(ESC 注入)/前导空白/其它 scheme 拒绝;
//   - launcher 可注入(mock 断言 URL 原样传递,不真实打开浏览器);
//   - 校验失败时不触碰 launcher,返回错误而非崩溃。

#include <gtest/gtest.h>

#include "utils/open_url.hpp"

#include <string>

using namespace acecode;

namespace {

// 记录收到的 URL 的 mock launcher。
class UrlRecorder {
public:
    explicit UrlRecorder(bool succeed) : succeed_(succeed) {}

    OpenUrlLauncher launcher() {
        return [this](const std::string& url, std::string& error) {
            received_ = url;
            if (!succeed_) {
                error = "mock open failed";
                return false;
            }
            return true;
        };
    }

    const std::string& received() const { return received_; }

private:
    bool succeed_;
    std::string received_;
};

} // namespace

// ---------- is_openable_http_url ----------

TEST(OpenUrl, AcceptsHttpAndHttps) {
    EXPECT_TRUE(is_openable_http_url("https://example.com"));
    EXPECT_TRUE(is_openable_http_url("http://example.com/path?q=1"));
    // scheme 大小写不敏感
    EXPECT_TRUE(is_openable_http_url("HTTP://EXAMPLE.COM"));
    // URL 中段空格合法(参数原样传递,不经 shell)
    EXPECT_TRUE(is_openable_http_url("https://exa mple.com/x"));
}

TEST(OpenUrl, RejectsOtherSchemes) {
    EXPECT_FALSE(is_openable_http_url("ftp://example.com"));
    EXPECT_FALSE(is_openable_http_url("file:///tmp/x"));
    EXPECT_FALSE(is_openable_http_url("javascript:alert(1)"));
    EXPECT_FALSE(is_openable_http_url("mailto:foo@bar.com"));
}

TEST(OpenUrl, RejectsControlAndEdgeCases) {
    EXPECT_FALSE(is_openable_http_url(""));
    EXPECT_FALSE(is_openable_http_url("   https://example.com"));  // 前导空白
    EXPECT_FALSE(is_openable_http_url("https://example.com  "));   // 尾部空白
    // ESC 终端转义注入必须拦
    EXPECT_FALSE(is_openable_http_url(
        std::string("https://example.com") + std::string("\x1b]8;;")));
    // https 前缀不完整
    EXPECT_FALSE(is_openable_http_url("https:/example.com"));
    EXPECT_FALSE(is_openable_http_url("http://"));
}

// ---------- open_url_in_browser ----------

TEST(OpenUrl, LauncherReceivesUrl) {
    UrlRecorder recorder(true);
    const auto result = open_url_in_browser("https://example.com/a", recorder.launcher());
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(recorder.received(), "https://example.com/a");
}

TEST(OpenUrl, InvalidUrlNeverReachesLauncher) {
    UrlRecorder recorder(true);
    const auto result = open_url_in_browser("ftp://example.com", recorder.launcher());
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
    EXPECT_TRUE(recorder.received().empty());  // launcher 未被调用
}

TEST(OpenUrl, LauncherFailurePropagatesError) {
    UrlRecorder recorder(false);
    const auto result = open_url_in_browser("https://example.com", recorder.launcher());
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "mock open failed");
}
