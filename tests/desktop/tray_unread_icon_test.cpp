#ifdef _WIN32
#include "desktop/tray_unread_icon_win.hpp"
#include "desktop/tray_icon_win.hpp"
#include <gtest/gtest.h>
#include <vector>

using namespace acecode::desktop;

namespace {
struct BitmapDeleter { void operator()(HBITMAP bitmap) const { ::DeleteObject(bitmap); } };
using OwnedBitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, BitmapDeleter>;
struct DcDeleter { void operator()(HDC dc) const { ::DeleteDC(dc); } };
using OwnedDc = std::unique_ptr<std::remove_pointer_t<HDC>, DcDeleter>;
}

TEST(TrayUnreadIcon, RedPixelsStayInLowerRightAtSupportedScales) {
    const auto original = ::LoadIconW(nullptr, MAKEINTRESOURCEW(32516)); // shared information icon
    for (const int size : {16, 20, 24, 32, 48, 64}) {
        auto icon = create_tray_unread_icon(original, size);
        ASSERT_TRUE(icon) << size;
        ICONINFO info{};
        ASSERT_TRUE(::GetIconInfo(icon.get(), &info));
        OwnedBitmap color(info.hbmColor), mask(info.hbmMask);
        BITMAP bitmap{};
        ASSERT_EQ(::GetObjectW(color.get(), sizeof(bitmap), &bitmap), sizeof(bitmap));
        EXPECT_EQ(bitmap.bmWidth, size);
        EXPECT_EQ(bitmap.bmHeight, size);
        std::vector<unsigned char> pixels(size * size * 4);
        BITMAPINFO pixels_info{};
        pixels_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        pixels_info.bmiHeader.biWidth = size;
        pixels_info.bmiHeader.biHeight = -size; // explicitly top-down
        pixels_info.bmiHeader.biPlanes = 1;
        pixels_info.bmiHeader.biBitCount = 32;
        pixels_info.bmiHeader.biCompression = BI_RGB;
        OwnedDc dc(::CreateCompatibleDC(nullptr));
        ASSERT_TRUE(dc);
        ASSERT_EQ(::GetDIBits(dc.get(), color.get(), 0, size, pixels.data(), &pixels_info, DIB_RGB_COLORS), size);
        int red_pixels = 0;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const auto at = (y * size + x) * 4;
                if (pixels[at + 2] > 180 && pixels[at + 1] < 100 && pixels[at] < 120) {
                    ++red_pixels;
                    EXPECT_GE(x, size / 2);
                    EXPECT_GE(y, size / 2);
                }
            }
        }
        EXPECT_GT(red_pixels, 0) << size;
    }
}

TEST(TrayUnreadIcon, ReleasesIconsAndHandlesInvalidInputs) {
    EXPECT_FALSE(create_tray_unread_icon(nullptr, 16));
    const auto original = ::LoadIconW(nullptr, MAKEINTRESOURCEW(32516));
    EXPECT_FALSE(create_tray_unread_icon(original, 0));
    EXPECT_FALSE(create_tray_unread_icon(original, 257));
    { auto warmup = create_tray_unread_icon(original, 24); ASSERT_TRUE(warmup); }
    const auto before = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
    const auto user_before = ::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS);
    for (int i = 0; i < 200; ++i) ASSERT_TRUE(create_tray_unread_icon(original, 24));
    // Process-wide graphics caches can release older objects during this loop.
    // A decrease is harmless; leaked per-icon objects would grow after 200 calls.
    EXPECT_LE(::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS), before);
    EXPECT_LE(::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS), user_before);
}

TEST(TrayUnreadIcon, CanSetClearAndShutDownNativeTray) {
    ASSERT_TRUE(set_tray_icon_unread(true)); // unavailable tray is harmless
    void* window = nullptr;
    if (!init_tray_icon([] {}, [] {}, &window)) GTEST_SKIP() << "Notification area unavailable";
    EXPECT_TRUE(set_tray_icon_unread(true));
    EXPECT_TRUE(set_tray_icon_unread(true));
    EXPECT_TRUE(set_tray_icon_unread(false));
    ::SendMessageW(static_cast<HWND>(window), WM_DISPLAYCHANGE, 0, 0);
    EXPECT_TRUE(set_tray_icon_unread(true));
    shutdown_tray_icon();
    EXPECT_TRUE(set_tray_icon_unread(false));
    MSG message{};
    ::PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE);
}
#endif
