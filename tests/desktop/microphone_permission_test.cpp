#include <gtest/gtest.h>

#include "desktop/microphone_permission.hpp"

using acecode::desktop::desktop_microphone_origin;
using acecode::desktop::may_grant_desktop_microphone;
using acecode::desktop::decide_linux_media_permission;
using acecode::desktop::LinuxMediaPermissionDecision;
using acecode::desktop::LinuxMediaPermissionRequest;

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

// 场景:Linux Desktop(WebKitGTK)上 ACECode 自己的页面点麦克风。
// 期望:native 导航到的那个本机页面发起的纯音频采集请求直接放行,不弹网页确认。
TEST(DesktopMicrophonePermission, LinuxAllowsAudioForTrustedPage) {
    const auto trusted = desktop_microphone_origin("http://127.0.0.1:28080/?token=private");
    LinuxMediaPermissionRequest audio;
    audio.user_media = true;
    audio.audio = true;
    EXPECT_EQ(decide_linux_media_permission(trusted, "http://127.0.0.1:28080/chat", audio),
              LinuxMediaPermissionDecision::Allow);
}

// 场景:摄像头请求、音视频一起请求,或 WebView 里的页面是别的端口 / 别的来源,或 native 尚未导航过。
// 期望:一律拒绝。
// 回归:修复前 Linux 只看当前地址是不是 http://127.0.0.1 / localhost 开头,任何端口的本机页面都能录音;
// Windows / macOS 早已只认 native 导航到的那个来源,现在三端一致(WebKitGTK 2.50 实测:旧版放行、新版拒绝)。
TEST(DesktopMicrophonePermission, LinuxDeniesCameraOtherPagesAndUnknownOrigin) {
    const auto trusted = desktop_microphone_origin("http://127.0.0.1:28080/");
    LinuxMediaPermissionRequest camera;
    camera.user_media = true;
    camera.video = true;
    LinuxMediaPermissionRequest both = camera;
    both.audio = true;
    LinuxMediaPermissionRequest audio;
    audio.user_media = true;
    audio.audio = true;
    const auto page = "http://127.0.0.1:28080/";
    EXPECT_EQ(decide_linux_media_permission(trusted, page, camera), LinuxMediaPermissionDecision::Deny);
    EXPECT_EQ(decide_linux_media_permission(trusted, page, both), LinuxMediaPermissionDecision::Deny);
    for (const auto* other : {"http://127.0.0.1:28081/", "http://localhost:28080/",
            "https://example.com/", "about:blank", ""}) {
        EXPECT_EQ(decide_linux_media_permission(trusted, other, audio), LinuxMediaPermissionDecision::Deny) << other;
    }
    EXPECT_EQ(decide_linux_media_permission("", page, audio), LinuxMediaPermissionDecision::Deny);
}

// 场景:通知、地理位置、设备信息等不是采集的权限请求。
// 期望:返回 Unhandled,交回 WebKit 默认处理,与修复前一致。
TEST(DesktopMicrophonePermission, LinuxLeavesOtherPermissionKindsToWebKit) {
    const auto trusted = desktop_microphone_origin("http://127.0.0.1:28080/");
    EXPECT_EQ(decide_linux_media_permission(trusted, "http://127.0.0.1:28080/", LinuxMediaPermissionRequest{}),
              LinuxMediaPermissionDecision::Unhandled);
}
