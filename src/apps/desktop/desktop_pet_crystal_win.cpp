#include "desktop_pet_crystal_win.hpp"

#ifdef _WIN32
#  include "utils/encoding.hpp"
#  include "utils/joining_thread.hpp"
#  include "utils/logger.hpp"

#  include <algorithm>
#  include <climits>
#  include <cmath>
#  include <condition_variable>
#  include <cstdint>
#  include <cstdio>
#  include <cstring>
#  include <mutex>
#  include <string>

#  include <nlohmann/json.hpp>
#  include <wrl.h>
#  include <WebView2.h>

#  ifndef WDA_EXCLUDEFROMCAPTURE
#    define WDA_EXCLUDEFROMCAPTURE 0x00000011
#  endif

namespace acecode::desktop {

namespace {

using Microsoft::WRL::ComPtr;

// 抓取半径 = 球半径 × 3.4(3.2 给折射取样,0.2 给漂浮 / 跳动的余量);页面把更远的取样平滑压缩进来。
constexpr double kReach = 3.4;
// 宠物放得很大时也只抓这么大(物理像素半径),超出的部分页面同样压缩。
constexpr int kMaxReach = 900;
// 页面迟迟不回确认(页面卡住、消息丢了)时,过这么久不再等它。
constexpr ULONGLONG kAckTimeoutMs = 1000;

std::string hresult_text(HRESULT hr) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

// GUI 线程与抓屏线程共享;抓屏线程只持有它的 shared_ptr,不碰 CrystalLens 本身。
struct CaptureJob {
    std::mutex mu;
    std::condition_variable cv;
    bool pending = false;
    bool busy = false;
    bool stop = false;
    RECT screen{};                  // 要抓的屏幕矩形(已夹在虚拟屏幕内)
    unsigned char* dest = nullptr;  // 共享内存;GUI 线程保证抓取期间不关闭它
    std::size_t capacity = 0;
    int width = 0;                  // 结果:0 表示这次没抓到
    int height = 0;
    std::uint64_t hash = 0;
    HWND window = nullptr;
    UINT message = 0;
};

// 抓屏线程自己的 GDI 资源。
struct ScreenGrabber {
    HDC screen = ::GetDC(nullptr);
    HDC memory = screen ? ::CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    unsigned char* bits = nullptr;
    int width = 0;
    int height = 0;

    ScreenGrabber() = default;
    ScreenGrabber(const ScreenGrabber&) = delete;
    ScreenGrabber& operator=(const ScreenGrabber&) = delete;
    ~ScreenGrabber() {
        drop_bitmap();
        if (memory) ::DeleteDC(memory);
        if (screen) ::ReleaseDC(nullptr, screen);
    }

    void drop_bitmap() {
        if (!bitmap) return;
        ::SelectObject(memory, previous);
        ::DeleteObject(bitmap);
        bitmap = nullptr;
        bits = nullptr;
        width = height = 0;
    }

    bool ensure(int w, int h) {
        if (!memory) return false;
        if (bitmap && width >= w && height >= h) return true;
        drop_bitmap();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;   // 自上而下
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap || !pixels) {
            if (bitmap) ::DeleteObject(bitmap);
            bitmap = nullptr;
            return false;
        }
        previous = ::SelectObject(memory, bitmap);
        bits = static_cast<unsigned char*>(pixels);
        width = w;
        height = h;
        return true;
    }
};

// 抓一帧:BitBlt 到私有位图,按 w×h 紧密排列拷进共享内存,顺便算哈希(隔一个像素取一次)。
bool grab(ScreenGrabber& grabber, const RECT& rect, unsigned char* dest, std::size_t capacity, std::uint64_t& hash) {
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0 || !dest || static_cast<std::size_t>(w) * h * 4 > capacity || !grabber.ensure(w, h)) return false;
    if (!::BitBlt(grabber.memory, 0, 0, w, h, grabber.screen, rect.left, rect.top, SRCCOPY)) return false;
    ::GdiFlush();
    const std::size_t row = static_cast<std::size_t>(w) * 4, stride = static_cast<std::size_t>(grabber.width) * 4;
    std::uint64_t value = 1469598103934665603ull;
    for (int y = 0; y < h; ++y) {
        const unsigned char* source = grabber.bits + stride * y;
        std::memcpy(dest + row * y, source, row);
        for (std::size_t x = 0; x + 8 <= row; x += 16) {
            std::uint64_t word = 0;
            std::memcpy(&word, source + x, sizeof(word));
            value = (value ^ word) * 1099511628211ull;
        }
    }
    hash = value;
    return true;
}

void capture_loop(std::shared_ptr<CaptureJob> job) {
    ScreenGrabber grabber;
    for (;;) {
        RECT rect{};
        unsigned char* dest = nullptr;
        std::size_t capacity = 0;
        {
            std::unique_lock<std::mutex> lock(job->mu);
            while (!job->pending && !job->stop) job->cv.wait(lock);
            if (job->stop) return;
            job->pending = false;
            job->busy = true;
            rect = job->screen;
            dest = job->dest;
            capacity = job->capacity;
        }
        std::uint64_t hash = 0;
        const bool ok = grab(grabber, rect, dest, capacity, hash);
        {
            std::lock_guard<std::mutex> lock(job->mu);
            job->busy = false;
            job->width = ok ? rect.right - rect.left : 0;
            job->height = ok ? rect.bottom - rect.top : 0;
            job->hash = hash;
        }
        ::PostMessageW(job->window, job->message, 0, 0);
    }
}

bool read_number(const nlohmann::json& message, const char* key, double& out) {
    const auto field = message.find(key);
    if (field == message.end() || !field->is_number()) return false;
    out = field->get<double>();
    return std::isfinite(out);
}

} // namespace

struct CrystalLens::Impl {
    HWND window = nullptr;
    ComPtr<ICoreWebView2> core;
    ComPtr<ICoreWebView2Environment12> environment;
    ComPtr<ICoreWebView2_17> webview;
    ComPtr<ICoreWebView2SharedBuffer> buffer;
    unsigned char* bytes = nullptr;
    std::size_t capacity = 0;
    std::shared_ptr<CaptureJob> job = std::make_shared<CaptureJob>();
    JoiningThread worker;

    bool lens = false;              // 页面已上报球的位置
    double lens_x = 0.0, lens_y = 0.0, lens_r = 0.0;
    bool capturing = false;         // 已排除出屏幕捕获、允许抓屏
    bool unavailable = false;       // 共享内存 / 显示亲和性用不了:一直用页面内置背景
    bool awaiting = false;          // 已发帧、等页面确认
    std::uint64_t seq = 0;
    ULONGLONG sent_at = 0;
    RECT requested{}, requested_window{};
    bool frame_sent = false;
    std::uint64_t last_hash = 0;
    RECT last_rect{}, last_window{};
    POINT last_cursor{LONG_MIN, LONG_MIN};
    RECT cursor_window{};

    void post(const nlohmann::json& message) {
        if (core) core->PostWebMessageAsJson(utf8_to_wide(message.dump()).c_str());
    }

    // 本机会话排除出捕获并抓屏;远程会话(或切到远程)撤销,页面退回内置背景。
    void update_session() {
        const bool want = !unavailable && ::GetSystemMetrics(SM_REMOTESESSION) == 0;
        if (want == capturing) return;
        if (want) {
            if (!::SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE)) {
                LOG_WARN("[desktop-pet] crystal: SetWindowDisplayAffinity failed: " + std::to_string(::GetLastError()));
                unavailable = true;
                return;
            }
            capturing = true;
            frame_sent = false;
            LOG_INFO("[desktop-pet] crystal: refracting the desktop behind the pet");
        } else {
            ::SetWindowDisplayAffinity(window, WDA_NONE);
            capturing = false;
            awaiting = false;
            post({{"type", "crystal-off"}});
            LOG_INFO("[desktop-pet] crystal: remote session, using the built-in backdrop");
        }
    }

    // 共享内存不够大时换一块更大的,先交给页面再关旧的(此时没有待确认的帧,抓屏线程也空闲)。
    bool ensure_buffer(std::size_t need) {
        if (buffer && capacity >= need) return true;
        ComPtr<ICoreWebView2SharedBuffer> next;
        BYTE* data = nullptr;
        HRESULT hr = environment->CreateSharedBuffer(need, &next);
        if (SUCCEEDED(hr) && next) hr = next->get_Buffer(&data);
        if (SUCCEEDED(hr) && data)
            hr = webview->PostSharedBufferToScript(next.Get(), COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY, nullptr);
        if (FAILED(hr) || !data) {
            LOG_WARN("[desktop-pet] crystal: shared buffer unavailable: " + hresult_text(hr));
            if (next) next->Close();
            unavailable = true;
            update_session();
            return false;
        }
        if (buffer) buffer->Close();
        buffer = next;
        bytes = data;
        capacity = need;
        frame_sent = false;
        return true;
    }

    void post_cursor(const RECT& win) {
        POINT cursor{};
        if (!::GetCursorPos(&cursor)) return;
        if (cursor.x == last_cursor.x && cursor.y == last_cursor.y && ::EqualRect(&win, &cursor_window)) return;
        last_cursor = cursor;
        cursor_window = win;
        const double w = win.right - win.left, h = win.bottom - win.top;
        if (w <= 0 || h <= 0) return;
        post({{"type", "crystal-cursor"}, {"x", (cursor.x - win.left) / w}, {"y", (cursor.y - win.top) / h}});
    }
};

CrystalLens::CrystalLens(HWND window, UINT captured_message, ICoreWebView2Environment* environment,
                         ICoreWebView2* webview)
    : impl_(std::make_unique<Impl>()) {
    Impl& s = *impl_;
    s.window = window;
    s.core = webview;
    s.job->window = window;
    s.job->message = captured_message;
    if (environment) environment->QueryInterface(IID_PPV_ARGS(&s.environment));
    if (webview) webview->QueryInterface(IID_PPV_ARGS(&s.webview));
    if (!s.environment || !s.webview) {
        LOG_WARN("[desktop-pet] crystal: WebView2 shared buffers unavailable; using the built-in backdrop");
        s.unavailable = true;
    } else {
        s.worker = JoiningThread(capture_loop, s.job);
    }
    s.update_session();
}

CrystalLens::~CrystalLens() {
    Impl& s = *impl_;
    {
        std::lock_guard<std::mutex> lock(s.job->mu);
        s.job->stop = true;
    }
    s.job->cv.notify_all();
    s.worker.join();
    if (s.buffer) s.buffer->Close();
    if (s.capturing) ::SetWindowDisplayAffinity(s.window, WDA_NONE);
}

void CrystalLens::handle(const nlohmann::json& message) {
    Impl& s = *impl_;
    const auto type = message.find("type");
    if (type == message.end() || !type->is_string()) return;
    if (*type == "crystal-lens") {
        double x = 0, y = 0, r = 0;
        if (!read_number(message, "x", x) || !read_number(message, "y", y) || !read_number(message, "r", r)) return;
        if (x < -0.5 || x > 1.5 || y < -0.5 || y > 1.5 || r <= 0.0 || r > 1.0) return;
        s.lens = true;
        s.lens_x = x;
        s.lens_y = y;
        s.lens_r = r;
        s.frame_sent = false;
    } else if (*type == "crystal-ack") {
        const auto seq = message.find("seq");
        if (seq != message.end() && seq->is_number_integer() && seq->get<std::uint64_t>() == s.seq) s.awaiting = false;
    }
}

void CrystalLens::tick() {
    Impl& s = *impl_;
    RECT win{};
    if (!::GetWindowRect(s.window, &win)) return;
    s.post_cursor(win);
    s.update_session();
    if (!s.capturing || !s.lens) return;
    if (s.awaiting && ::GetTickCount64() - s.sent_at < kAckTimeoutMs) return;
    s.awaiting = false;
    {
        std::lock_guard<std::mutex> lock(s.job->mu);
        if (s.job->busy || s.job->pending) return;
    }
    const double width = win.right - win.left, height = win.bottom - win.top;
    const double cx = win.left + s.lens_x * width, cy = win.top + s.lens_y * height;
    const int reach = std::min(kMaxReach, static_cast<int>(std::ceil(s.lens_r * width * kReach)) + 4);
    const LONG left = static_cast<LONG>(std::floor(cx)) - reach, top = static_cast<LONG>(std::floor(cy)) - reach;
    const RECT wanted{left, top, left + 2 * reach, top + 2 * reach};
    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN), vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const RECT screen{vx, vy, vx + ::GetSystemMetrics(SM_CXVIRTUALSCREEN), vy + ::GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    RECT clipped{};
    if (!::IntersectRect(&clipped, &wanted, &screen)) return;
    if (!s.ensure_buffer(static_cast<std::size_t>(2 * reach) * (2 * reach) * 4)) return;
    s.requested = clipped;
    s.requested_window = win;
    {
        std::lock_guard<std::mutex> lock(s.job->mu);
        s.job->screen = clipped;
        s.job->dest = s.bytes;
        s.job->capacity = s.capacity;
        s.job->pending = true;
    }
    s.job->cv.notify_one();
}

void CrystalLens::on_captured() {
    Impl& s = *impl_;
    int w = 0, h = 0;
    std::uint64_t hash = 0;
    {
        std::lock_guard<std::mutex> lock(s.job->mu);
        w = s.job->width;
        h = s.job->height;
        hash = s.job->hash;
    }
    if (!s.capturing || w <= 0 || h <= 0) return;
    const RECT& rect = s.requested;
    const RECT& win = s.requested_window;
    // 背景和球的位置都没变:页面手上那帧还能用,不再上传。
    if (s.frame_sent && hash == s.last_hash && ::EqualRect(&rect, &s.last_rect) && ::EqualRect(&win, &s.last_window)) return;
    s.frame_sent = true;
    s.last_hash = hash;
    s.last_rect = rect;
    s.last_window = win;
    s.awaiting = true;
    s.sent_at = ::GetTickCount64();
    s.post({{"type", "crystal-frame"}, {"seq", ++s.seq}, {"x", rect.left - win.left}, {"y", rect.top - win.top},
            {"w", w}, {"h", h}, {"ww", win.right - win.left}, {"wh", win.bottom - win.top}});
}

} // namespace acecode::desktop
#endif
