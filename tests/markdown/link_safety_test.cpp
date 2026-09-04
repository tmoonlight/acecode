// 覆盖 src/markdown/link_safety.cpp 的链接防骗校验(add-tui-hyperlinks 4.4/4.6)。
//
// 只比域名(host),不比完整路径。规则(design.md 决策 4):
//   - href 不含 "://"(本地路径/裸域名)→ 放行(本地通道,防骗只针对网页链接)
//   - href 远程但 host 解析失败 → URL 形 label 降级,普通标签放行
//   - label 无 URL 形状(无点号或含空白)→ 放行(标签文字)
//   - label 呈 URL 形状 → host 一致放行(忽略大小写),不一致降级,
//     畸形/非 ASCII 按不匹配(降级)

#include <gtest/gtest.h>

#include "markdown/link_safety.hpp"

#include <optional>
#include <string>

using namespace acecode::markdown;

// ---------- extract_url_host ----------

TEST(LinkSafety, ExtractHostFullUrl) {
    EXPECT_EQ(extract_url_host("https://www.example.com:8080/path?q=1#frag"),
              std::optional<std::string>("www.example.com"));
}

TEST(LinkSafety, ExtractHostBareDomainWithPath) {
    EXPECT_EQ(extract_url_host("github.com/foo/bar"),
              std::optional<std::string>("github.com"));
}

TEST(LinkSafety, ExtractHostUserinfoAndPort) {
    EXPECT_EQ(extract_url_host("user:pass@EXAMPLE.com:8080/x"),
              std::optional<std::string>("example.com"));
    EXPECT_EQ(extract_url_host("https://user:pass@EXAMPLE.com:8080/x"),
              std::optional<std::string>("example.com"));
    EXPECT_EQ(extract_url_host("https://example.com:65535/x"),
              std::optional<std::string>("example.com"));
}

TEST(LinkSafety, ExtractHostStopsAtAuthorityBoundaryBeforeAtSign) {
    EXPECT_EQ(extract_url_host("https://evil.example/@trusted.example"),
              std::optional<std::string>("evil.example"));
    EXPECT_EQ(extract_url_host("https://evil.example/?next=@trusted.example"),
              std::optional<std::string>("evil.example"));
    EXPECT_EQ(extract_url_host("https://evil.example#@trusted.example"),
              std::optional<std::string>("evil.example"));
    EXPECT_EQ(extract_url_host("https://evil.example\\@trusted.example"),
              std::optional<std::string>("evil.example"));
}

TEST(LinkSafety, ExtractBracketedIpv6Host) {
    EXPECT_EQ(extract_url_host("https://[2001:DB8::1]:8443/path"),
              std::optional<std::string>("2001:db8::1"));
    EXPECT_EQ(extract_url_host("https://[::1]/"),
              std::optional<std::string>("::1"));
}

TEST(LinkSafety, ExtractHostTrailingDotStripped) {
    EXPECT_EQ(extract_url_host("example.com."),
              std::optional<std::string>("example.com"));
}

TEST(LinkSafety, ExtractHostLocalPathFails) {
    EXPECT_EQ(extract_url_host("./docs/foo.md"), std::nullopt);
    EXPECT_EQ(extract_url_host("/etc/passwd"), std::nullopt);
    EXPECT_EQ(extract_url_host("docs/说明.md"), std::nullopt);
}

TEST(LinkSafety, ExtractHostOtherSchemeFails) {
    // file:/// 的 authority 是空 → host 解析失败
    EXPECT_EQ(extract_url_host("file:///tmp/x"), std::nullopt);
    // mailto: 单冒号协议无 "://",按 authority 宽松解析出 userinfo 后的 host
    // (安全:比较对象仍是域名;is_safe_link_label 只对含 "://" 的 href 做比较)
    EXPECT_EQ(extract_url_host("mailto:foo@bar.com"),
              std::optional<std::string>("bar.com"));
}

TEST(LinkSafety, ExtractHostMalformedFails) {
    EXPECT_EQ(extract_url_host(""), std::nullopt);
    EXPECT_EQ(extract_url_host("https://"), std::nullopt);
    EXPECT_EQ(extract_url_host("https:///path"), std::nullopt);
    EXPECT_EQ(extract_url_host("https://user@/path"), std::nullopt);
    EXPECT_EQ(extract_url_host("https://[2001:db8::1/path"), std::nullopt);
    EXPECT_EQ(extract_url_host("https://example.com:not-a-port/path"),
              std::nullopt);
    EXPECT_EQ(extract_url_host("https://example.com:65536/path"),
              std::nullopt);
    EXPECT_EQ(extract_url_host("https://例子.中国"), std::nullopt);
}

// ---------- is_safe_link_label:伪装 host → 降级 ----------

TEST(LinkSafety, SpoofedHostDowngraded) {
    EXPECT_FALSE(is_safe_link_label("google.com", "https://evil.example.com"));
    EXPECT_FALSE(
        is_safe_link_label("https://www.google.com", "https://evil.example.com"));
    // 子域名陷阱:label host 是 google.com.evil.example.com,不是 google.com
    EXPECT_FALSE(
        is_safe_link_label("google.com", "https://google.com.evil.example.com"));
    EXPECT_FALSE(is_safe_link_label(
        "trusted.example", "https://evil.example/@trusted.example"));
    EXPECT_FALSE(is_safe_link_label(
        "trusted.example", "https://evil.example/?next=@trusted.example"));
    EXPECT_FALSE(is_safe_link_label(
        "trusted.example", "https://evil.example\\@trusted.example"));
}

// ---------- 标签文字(无 URL 形状)→ 放行 ----------

TEST(LinkSafety, PlainLabelPasses) {
    EXPECT_TRUE(is_safe_link_label("我的博客", "https://example.com"));
    EXPECT_TRUE(is_safe_link_label("click here", "https://example.com"));
    EXPECT_TRUE(is_safe_link_label("", "https://example.com"));
    EXPECT_TRUE(is_safe_link_label("入门指南", "https://example.com/docs"));
}

// ---------- host 一致(省略 scheme / 截断路径 / 大小写)→ 放行 ----------

TEST(LinkSafety, MatchingHostPasses) {
    EXPECT_TRUE(is_safe_link_label("https://github.com/foo/bar",
                                   "https://github.com/foo/bar"));
    // 省略 scheme、截断路径
    EXPECT_TRUE(is_safe_link_label("github.com/foo/bar",
                                   "https://github.com/foo"));
    // 域名大小写不敏感
    EXPECT_TRUE(is_safe_link_label("Google.com", "https://google.com/x"));
    // label 带 userinfo/端口,host 仍一致
    EXPECT_TRUE(is_safe_link_label("user@example.com:8080/path",
                                   "https://example.com/x"));
}

// ---------- 畸形 / 非 ASCII → 降级 ----------

TEST(LinkSafety, MalformedUrlDowngraded) {
    EXPECT_FALSE(is_safe_link_label("https://例子.中国", "https://example.com"));
    // label 呈 URL 形状但 host 为空("https://.." 去尾部点后无内容)
    EXPECT_FALSE(is_safe_link_label("https://..", "https://example.com"));
    // label host 是仿冒子域,与目标不符
    EXPECT_FALSE(is_safe_link_label("https://example..com",
                                    "https://example.com"));
    // URL-shaped labels fail closed when the remote href has no valid authority.
    EXPECT_FALSE(is_safe_link_label("example.com", "https:///example.com"));
    EXPECT_FALSE(is_safe_link_label("example.com", "https://user@/path"));
}

// ---------- 本地链接通道(href 无 "://")→ 放行 ----------

TEST(LinkSafety, LocalHrefPasses) {
    // 常见 markdown 写法 [foo.md](./foo.md):label 是文件名,不参与防骗比较
    EXPECT_TRUE(is_safe_link_label("foo.md", "./docs/foo.md"));
    // 无 "./" 前缀的相对路径也放行——防骗只针对含 "://" 的远程 href
    EXPECT_TRUE(is_safe_link_label("foo.md", "docs/foo.md"));
    EXPECT_TRUE(is_safe_link_label("/etc/passwd", "/etc/passwd"));
    EXPECT_TRUE(is_safe_link_label("配置说明", "docs/说明.md"));
    // 裸域名 href(无协议)→ 本地通道,即使 label 伪装也不降级
    // (点击走本地路径打开,不会打开浏览器)
    EXPECT_TRUE(is_safe_link_label("google.com", "docs/evil.md"));
}
