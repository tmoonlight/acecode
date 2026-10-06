#include "websocket_client.hpp"

#include "network/proxy_resolver.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace acecode::network {
namespace {

constexpr std::chrono::milliseconds kWaitSlice{100};
constexpr std::size_t kRecvChunk = 64 * 1024;

std::string curl_error_text(CURLcode code, const char* detail) {
    std::string text = curl_easy_strerror(code);
    if (detail && *detail) text += std::string(": ") + detail;
    return text;
}

// 等待 socket 可读或可写。超时返回 false;select/poll 自身出错时返回 true,
// 让后续的 curl 调用报告真实错误,而不是在这里吞掉。
bool wait_socket(curl_socket_t socket, bool for_write, std::chrono::milliseconds timeout) {
    const auto ms = static_cast<long>(std::max<std::int64_t>(0, timeout.count()));
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    const int ready = select(0, for_write ? nullptr : &set, for_write ? &set : nullptr,
                             nullptr, &tv);
#else
    pollfd item{socket, static_cast<short>(for_write ? POLLOUT : POLLIN), 0};
    const int ready = ::poll(&item, 1, static_cast<int>(ms));
#endif
    return ready != 0;
}

} // namespace

struct WebSocketClient::Impl {
    mutable std::mutex mu;  // 串行化对 easy handle 的所有调用
    CURL* curl = nullptr;
    curl_slist* headers = nullptr;
    curl_socket_t socket = CURL_SOCKET_BAD;
    std::size_t max_message_bytes = 4u * 1024u * 1024u;
    std::atomic<bool> aborted{false};
    std::atomic<bool> open{false};
    int close_code = 0;
    std::string close_reason;
    // 正在重组的文本/二进制消息与 close 帧载荷。只在 receive 线程上访问。
    std::string partial;
    bool partial_binary = false;
    std::string close_payload;

    void release_locked() {
        if (curl) curl_easy_cleanup(curl);
        curl = nullptr;
        if (headers) curl_slist_free_all(headers);
        headers = nullptr;
        socket = CURL_SOCKET_BAD;
        open = false;
        partial.clear();
        close_payload.clear();
    }

    void record_close(const std::string& payload) {
        if (payload.size() >= 2) {
            close_code = (static_cast<unsigned char>(payload[0]) << 8) |
                         static_cast<unsigned char>(payload[1]);
            close_reason = payload.substr(2);
        } else {
            close_code = 1005;  // close 帧没有带状态码
            close_reason.clear();
        }
    }
};

WebSocketClient::WebSocketClient() : impl_(std::make_unique<Impl>()) {}

WebSocketClient::~WebSocketClient() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->release_locked();
}

bool WebSocketClient::connect(const WebSocketConnectOptions& options, std::string* error) {
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mu);
    s.release_locked();
    s.aborted = false;
    s.close_code = 0;
    s.close_reason.clear();
    s.max_message_bytes = std::max<std::size_t>(1024, options.max_message_bytes);

    s.curl = curl_easy_init();
    if (!s.curl) {
        if (error) *error = "curl_easy_init failed";
        return false;
    }
    char detail[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(s.curl, CURLOPT_ERRORBUFFER, detail);
    curl_easy_setopt(s.curl, CURLOPT_URL, options.url.c_str());
    curl_easy_setopt(s.curl, CURLOPT_CONNECT_ONLY, 2L);  // 2 = 完成 WebSocket Upgrade 后交还控制
    curl_easy_setopt(s.curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(s.curl, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(options.connect_timeout.count()));
    // 只约束握手阶段;握手完成后清零,后续收发由各自的超时控制。
    curl_easy_setopt(s.curl, CURLOPT_TIMEOUT_MS, static_cast<long>(options.connect_timeout.count()));
    curl_easy_setopt(s.curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NO_REVOKE));

    // 代理:与 cpr 调用点同源。直连时必须显式置空,否则 libcurl 会自行读取环境变量。
    std::string proxy;
    if (options.use_proxy) proxy = proxy_resolver().effective(websocket_proxy_lookup_url(options.url)).url;
    curl_easy_setopt(s.curl, CURLOPT_PROXY, proxy.c_str());

    for (const auto& [name, value] : options.headers) {
        const auto line = name + ": " + value;
        s.headers = curl_slist_append(s.headers, line.c_str());
    }
    if (s.headers) curl_easy_setopt(s.curl, CURLOPT_HTTPHEADER, s.headers);

    const CURLcode rc = curl_easy_perform(s.curl);
    if (rc != CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(s.curl, CURLINFO_RESPONSE_CODE, &status);
        if (error) {
            *error = curl_error_text(rc, detail);
            if (status > 0) *error += " (HTTP " + std::to_string(status) + ")";
        }
        s.release_locked();
        return false;
    }
    curl_easy_setopt(s.curl, CURLOPT_ERRORBUFFER, nullptr);
    curl_easy_setopt(s.curl, CURLOPT_TIMEOUT_MS, 0L);
    // connect-only 模式下服务端拒绝升级(401/404/200…)时 perform 仍返回 OK,
    // 必须确认拿到的是 101 Switching Protocols。
    long status = 0;
    curl_easy_getinfo(s.curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != 101) {
        if (error) *error = "WebSocket upgrade rejected (HTTP " + std::to_string(status) + ")";
        s.release_locked();
        return false;
    }
    curl_socket_t socket = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(s.curl, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK ||
        socket == CURL_SOCKET_BAD) {
        if (error) *error = "WebSocket connected without an active socket";
        s.release_locked();
        return false;
    }
    s.socket = socket;
    s.open = true;
    return true;
}

bool WebSocketClient::send_text(const std::string& payload, std::chrono::milliseconds timeout,
                                std::string* error) {
    return send_frame(payload, timeout, error, false);
}

bool WebSocketClient::send_binary(const std::string& payload, std::chrono::milliseconds timeout,
                                  std::string* error) {
    return send_frame(payload, timeout, error, true);
}

bool WebSocketClient::send_frame(const std::string& payload, std::chrono::milliseconds timeout,
                                 std::string* error, bool binary) {
    auto& s = *impl_;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t offset = 0;
    while (true) {
        if (s.aborted) {
            if (error) *error = "WebSocket aborted";
            return false;
        }
        curl_socket_t socket = CURL_SOCKET_BAD;
        {
            std::lock_guard<std::mutex> lock(s.mu);
            if (!s.curl || !s.open) {
                if (error) *error = "WebSocket is not connected";
                return false;
            }
            std::size_t sent = 0;
            const CURLcode rc = curl_ws_send(s.curl, payload.data() + offset, payload.size() - offset,
                                             &sent, 0, binary ? CURLWS_BINARY : CURLWS_TEXT);
            offset += sent;
            if (rc == CURLE_OK && offset >= payload.size()) return true;
            if (rc != CURLE_OK && rc != CURLE_AGAIN) {
                if (error) *error = curl_error_text(rc, nullptr);
                s.open = false;
                return false;
            }
            socket = s.socket;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            // 部分写出的帧无法撤回:连接已不可信,直接标记关闭。
            std::lock_guard<std::mutex> lock(s.mu);
            s.open = false;
            if (error) *error = "WebSocket send timed out";
            return false;
        }
        wait_socket(socket, true,
                    (std::min)(kWaitSlice,
                             std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
    }
}

WebSocketRecv WebSocketClient::receive(WebSocketMessage& out, std::chrono::milliseconds timeout,
                                       std::string* error) {
    auto& s = *impl_;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<char> buffer(kRecvChunk);
    while (true) {
        if (s.aborted) {
            if (error) *error = "WebSocket aborted";
            return WebSocketRecv::Error;
        }
        curl_socket_t socket = CURL_SOCKET_BAD;
        {
            std::lock_guard<std::mutex> lock(s.mu);
            if (!s.curl || !s.open) {
                if (s.close_code != 0) return WebSocketRecv::Closed;
                if (error) *error = "WebSocket is not connected";
                return WebSocketRecv::Error;
            }
            std::size_t received = 0;
            const curl_ws_frame* meta = nullptr;
            const CURLcode rc = curl_ws_recv(s.curl, buffer.data(), buffer.size(), &received, &meta);
            if (rc == CURLE_OK && meta) {
                const int flags = meta->flags;
                if (flags & CURLWS_CLOSE) {
                    s.close_payload.append(buffer.data(), received);
                    if (meta->bytesleft == 0) {
                        s.record_close(s.close_payload);
                        s.close_payload.clear();
                        // 回一个 close 帧完成关闭握手;失败也无所谓,连接马上释放。
                        std::size_t sent = 0;
                        curl_ws_send(s.curl, "", 0, &sent, 0, CURLWS_CLOSE);
                        s.release_locked();
                        return WebSocketRecv::Closed;
                    }
                    continue;
                }
                if (flags & (CURLWS_PING | CURLWS_PONG)) continue;  // libcurl 已自动回 pong
                if (flags & (CURLWS_TEXT | CURLWS_BINARY)) {
                    if (s.partial.size() + received + static_cast<std::size_t>(meta->bytesleft) >
                        s.max_message_bytes) {
                        if (error) *error = "WebSocket message exceeds limit";
                        s.close_code = 1009;
                        s.close_reason = "message too big";
                        std::size_t sent = 0;
                        const char code[2] = {static_cast<char>(0x03), static_cast<char>(0xF1)};
                        curl_ws_send(s.curl, code, sizeof(code), &sent, 0, CURLWS_CLOSE);
                        s.release_locked();
                        return WebSocketRecv::Error;
                    }
                    s.partial.append(buffer.data(), received);
                    s.partial_binary = (flags & CURLWS_BINARY) != 0;
                    if (meta->bytesleft == 0 && !(flags & CURLWS_CONT)) {
                        out.binary = s.partial_binary;
                        out.data = std::move(s.partial);
                        s.partial.clear();
                        return WebSocketRecv::Message;
                    }
                }
                continue;
            }
            if (rc == CURLE_GOT_NOTHING || rc == CURLE_RECV_ERROR) {
                // 对端没发 close 帧就断开了。
                if (s.close_code == 0) {
                    s.close_code = 1006;
                    s.close_reason = curl_easy_strerror(rc);
                }
                s.release_locked();
                return WebSocketRecv::Closed;
            }
            if (rc != CURLE_AGAIN) {
                if (error) *error = curl_error_text(rc, nullptr);
                s.release_locked();
                return WebSocketRecv::Error;
            }
            socket = s.socket;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return WebSocketRecv::Timeout;
        wait_socket(socket, false,
                    (std::min)(kWaitSlice,
                             std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
    }
}

void WebSocketClient::close(std::uint16_t code, const std::string& reason) {
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.curl && s.open) {
        std::string payload;
        payload.push_back(static_cast<char>((code >> 8) & 0xFF));
        payload.push_back(static_cast<char>(code & 0xFF));
        payload += reason.substr(0, 120);
        std::size_t sent = 0;
        curl_ws_send(s.curl, payload.data(), payload.size(), &sent, 0, CURLWS_CLOSE);
    }
    s.release_locked();
}

void WebSocketClient::abort() { impl_->aborted = true; }

bool WebSocketClient::connected() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->curl && impl_->open;
}

int WebSocketClient::close_code() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->close_code;
}

std::string WebSocketClient::close_reason() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->close_reason;
}

std::string websocket_proxy_lookup_url(const std::string& ws_url) {
    auto lower_prefix = [&](const char* prefix) {
        const std::string p(prefix);
        if (ws_url.size() < p.size()) return false;
        for (std::size_t i = 0; i < p.size(); ++i) {
            const char c = ws_url[i];
            if (static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c) != p[i]) return false;
        }
        return true;
    };
    if (lower_prefix("wss://")) return "https://" + ws_url.substr(6);
    if (lower_prefix("ws://")) return "http://" + ws_url.substr(5);
    return ws_url;
}

} // namespace acecode::network
