#include <gtest/gtest.h>

#include "desktop/microphone_permission.hpp"

using acecode::desktop::desktop_microphone_origin;
using acecode::desktop::may_grant_desktop_microphone;

TEST(DesktopMicrophonePermission, NormalizesLoopbackOriginsWithoutKeepingTokens) {
    EXPECT_EQ(desktop_microphone_origin("http://127.0.0.1:28080/?token=private#chat"),
              "http://127.0.0.1:28080");
    EXPECT_EQ(desktop_microphone_origin("HTTP://LOCALHOST:028080/chat"), "http://localhost:28080");
    EXPECT_EQ(desktop_microphone_origin("http://[::1]:28080/"), "http://[::1]:28080");
    EXPECT_EQ(desktop_microphone_origin("http://localhost/"), "http://localhost:80");
    EXPECT_EQ(desktop_microphone_origin("https://localhost"), "https://localhost:443");
}

TEST(DesktopMicrophonePermission, RejectsExternalAndMalformedOrigins) {
    for (const auto* url : {"", "http://", "https://", "https://example.com/",
            "http://192.168.1.2:8080/", "http://localhost.evil.test:8080/",
            "http://localhost@evil.test:8080/", "http://user@localhost:8080/",
            "http://localhost:8080@evil.test/", "http://localhost:0/",
            "http://localhost:65536/", "http://localhost:/", "http://localhost:12x/",
            "http://localhost:999999999999999999/", "http://[::1", "file:///tmp/page.html",
            "http://localhost:8080\\@evil.test/", "http://localhost:8080/\n"}) {
        EXPECT_TRUE(desktop_microphone_origin(url).empty()) << url;
    }
}

TEST(DesktopMicrophonePermission, GrantsOnlyTheConfiguredOriginAndCurrentPage) {
    const auto trusted = desktop_microphone_origin("http://127.0.0.1:28080/?token=private");
    EXPECT_TRUE(may_grant_desktop_microphone(trusted,
        "http://127.0.0.1:28080/chat", "http://127.0.0.1:28080/"));
    for (const auto* other : {"http://127.0.0.1:28081/", "http://localhost:28080/",
            "https://127.0.0.1:28080/", "https://example.com/", "about:blank"}) {
        EXPECT_FALSE(may_grant_desktop_microphone(trusted, other, trusted)) << other;
        EXPECT_FALSE(may_grant_desktop_microphone(trusted, trusted, other)) << other;
    }
    EXPECT_FALSE(may_grant_desktop_microphone("", trusted, trusted));
    EXPECT_FALSE(may_grant_desktop_microphone("https://example.com:443",
        "https://example.com/", "https://example.com/"));
}

TEST(DesktopMicrophonePermission, NativeNavigationReplacesOrRevokesTheGrant) {
    auto trusted = desktop_microphone_origin("http://127.0.0.1:28080/");
    const auto previous = trusted;
    trusted = desktop_microphone_origin("http://127.0.0.1:28081/");
    EXPECT_FALSE(may_grant_desktop_microphone(trusted, previous, previous));
    EXPECT_TRUE(may_grant_desktop_microphone(trusted, trusted, trusted));
    trusted = desktop_microphone_origin("https://example.com/");
    EXPECT_FALSE(may_grant_desktop_microphone(trusted, previous, previous));
}
