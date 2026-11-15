#include <gtest/gtest.h>
#include "platform/invite_protocol.hpp"
#include "platform/clipboard.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif

TEST(InviteProtocol, PreservesOtherLiveInstallations) {
    // 场景：绿色版只能修复缺失/失效的注册，不覆盖仍存在的安装版。
    using acecode::platform::should_register_invite_protocol;
    EXPECT_TRUE(should_register_invite_protocol("", false, "C:/ACE/app.exe"));
    EXPECT_TRUE(should_register_invite_protocol("C:/old/app.exe", false, "C:/ACE/app.exe"));
    EXPECT_FALSE(should_register_invite_protocol("C:/old/app.exe", true, "C:/ACE/app.exe"));
    EXPECT_TRUE(should_register_invite_protocol("c:\\ace\\app.exe", true, "C:/ACE/app.exe"));
}

TEST(InviteClipboard, PrivacyFlagsPrecedeInvitationExtraction) {
    // 场景：密码管理器标记拒绝监视，或拒绝剪贴板历史，均不提取链接。
    const std::string text = "邀请：acecode://connect/Abc_123-def。 acecode://connect/Second";
    EXPECT_EQ(acecode::extract_clipboard_invitation(text), "acecode://connect/Abc_123-def");
    EXPECT_TRUE(acecode::extract_clipboard_invitation(text, true).empty());
    EXPECT_TRUE(acecode::extract_clipboard_invitation(text, false, false).empty());
    EXPECT_TRUE(acecode::extract_clipboard_invitation(std::string(8192, 'x') + text).empty());
    EXPECT_TRUE(acecode::extract_clipboard_invitation("ordinary private clipboard text").empty());
}

#ifdef _WIN32
TEST(InviteProtocol, CurrentUserSandboxRegistrationAndOwnedRemoval) {
    // 场景：在专用 HKCU 测试键注册、保护另一安装、仅卸载自己拥有的关联。
    const auto root = std::filesystem::temp_directory_path() / ("acecode-invite-" + acecode::generate_uuid());
    std::filesystem::create_directories(root);
    const auto first = root / "first.exe";
    const auto second = root / "second.exe";
    std::ofstream(first).put('x'); std::ofstream(second).put('x');
    const auto key = "Software\\ACECodeInviteTests\\" + acecode::generate_uuid();
    EXPECT_TRUE(acecode::platform::register_invite_protocol(acecode::path_to_utf8(first), key));
    EXPECT_TRUE(acecode::platform::register_invite_protocol(acecode::path_to_utf8(second), key));
    const auto registry_path = acecode::path_from_utf8(key + "\\shell\\open\\command").wstring();
    wchar_t value[4096]{}; DWORD size = sizeof(value);
    EXPECT_EQ(::RegGetValueW(HKEY_CURRENT_USER, registry_path.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &size), ERROR_SUCCESS);
    EXPECT_NE(std::wstring(value).find(first.wstring()), std::wstring::npos);
    EXPECT_TRUE(acecode::platform::unregister_invite_protocol(acecode::path_to_utf8(second), key));
    size = sizeof(value);
    EXPECT_EQ(::RegGetValueW(HKEY_CURRENT_USER, registry_path.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &size), ERROR_SUCCESS);
    EXPECT_TRUE(acecode::platform::unregister_invite_protocol(acecode::path_to_utf8(first), key));
    size = sizeof(value);
    EXPECT_EQ(::RegGetValueW(HKEY_CURRENT_USER, registry_path.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &size), ERROR_FILE_NOT_FOUND);
    std::error_code ec; std::filesystem::remove_all(root, ec);
}
#endif
