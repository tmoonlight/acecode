#include "cdp_client.hpp"

#include "daemon/platform.hpp"
#include "desktop/agent_browser_runtime.hpp"
#include "utils/encoding.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace acecode::agent_browser {
namespace {

bool aborted(const std::atomic<bool>* flag) {
    return flag && flag->load();
}

#ifdef _WIN32
HANDLE open_proxy_pipe(const std::wstring& pipe_name,
                       std::chrono::steady_clock::time_point deadline,
                       const std::atomic<bool>* abort_flag,
                       std::string& error) {
    while (std::chrono::steady_clock::now() < deadline &&
           !aborted(abort_flag)) {
        if (::WaitNamedPipeW(pipe_name.c_str(), 100)) {
            HANDLE pipe = ::CreateFileW(
                pipe_name.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                nullptr);
            if (pipe != INVALID_HANDLE_VALUE) return pipe;
            const DWORD open_error = ::GetLastError();
            if (open_error != ERROR_PIPE_BUSY &&
                open_error != ERROR_FILE_NOT_FOUND) {
                error = "failed to open Agent Browser proxy pipe (" +
                        std::to_string(open_error) + ")";
                return INVALID_HANDLE_VALUE;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    error = aborted(abort_flag)
        ? "Agent Browser operation aborted"
        : "Agent Browser proxy is not ready";
    return INVALID_HANDLE_VALUE;
}

bool pipe_transfer(HANDLE pipe,
                   void* buffer,
                   std::size_t size,
                   bool write,
                   std::chrono::steady_clock::time_point deadline,
                   const std::atomic<bool>* abort_flag,
                   std::string& error) {
    std::size_t offset = 0;
    while (offset < size && std::chrono::steady_clock::now() < deadline &&
           !aborted(abort_flag)) {
        OVERLAPPED operation{};
        operation.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) {
            error = "failed to create Agent Browser proxy I/O event";
            return false;
        }
        const DWORD chunk = static_cast<DWORD>((std::min)(
            size - offset, static_cast<std::size_t>(1024u * 1024u)));
        DWORD transferred = 0;
        BOOL started = write
            ? ::WriteFile(pipe,
                          static_cast<const char*>(buffer) + offset,
                          chunk, &transferred, &operation)
            : ::ReadFile(pipe,
                         static_cast<char*>(buffer) + offset,
                         chunk, &transferred, &operation);
        DWORD operation_error = started ? ERROR_SUCCESS : ::GetLastError();
        bool pending_io = false;
        if (!started && operation_error == ERROR_IO_PENDING) {
            pending_io = true;
            while (std::chrono::steady_clock::now() < deadline &&
                   !aborted(abort_flag)) {
                const DWORD wait = ::WaitForSingleObject(operation.hEvent, 50);
                if (wait == WAIT_OBJECT_0) {
                    started = ::GetOverlappedResult(
                        pipe, &operation, &transferred, FALSE);
                    operation_error = started ? ERROR_SUCCESS : ::GetLastError();
                    break;
                }
                if (wait == WAIT_FAILED) {
                    operation_error = ::GetLastError();
                    break;
                }
            }
        }
        if (!started || std::chrono::steady_clock::now() >= deadline ||
            aborted(abort_flag)) {
            if (pending_io) {
                ::CancelIoEx(pipe, &operation);
                ::WaitForSingleObject(operation.hEvent, INFINITE);
            }
            ::CloseHandle(operation.hEvent);
            if (aborted(abort_flag)) {
                error = "Agent Browser operation aborted";
            } else if (std::chrono::steady_clock::now() >= deadline) {
                error = "Agent Browser proxy I/O timed out";
            } else {
                error = std::string("Agent Browser proxy pipe ") +
                        (write ? "write" : "read") +
                        " failed (Windows error " +
                        std::to_string(operation_error) + ")";
            }
            return false;
        }
        ::CloseHandle(operation.hEvent);
        if (transferred == 0) {
            error = "Agent Browser proxy connection closed";
            return false;
        }
        offset += transferred;
    }
    if (offset != size) {
        error = aborted(abort_flag)
            ? "Agent Browser operation aborted"
            : "Agent Browser proxy I/O timed out";
        return false;
    }
    return true;
}
#endif

} // namespace

struct AgentBrowserCdpClient::Impl {
#ifdef _WIN32
    std::wstring pipe_name;
    std::string auth_token;
#endif
    std::string page_id;
    bool ready = false;
};

std::optional<AgentBrowserElementRef> parse_agent_browser_element_ref(
    const std::string& value) {
    if (value.size() < 3 || value[0] != '@' || value[1] != 'e') {
        return std::nullopt;
    }
    int index = 0;
    for (std::size_t offset = 2; offset < value.size(); ++offset) {
        if (!std::isdigit(static_cast<unsigned char>(value[offset]))) {
            return std::nullopt;
        }
        index = index * 10 + (value[offset] - '0');
        if (index > 10000) return std::nullopt;
    }
    return index > 0
        ? std::optional<AgentBrowserElementRef>{{index}}
        : std::nullopt;
}

AgentBrowserCdpClient::AgentBrowserCdpClient() : impl_(new Impl) {}

AgentBrowserCdpClient::~AgentBrowserCdpClient() {
    close();
    delete impl_;
}

bool AgentBrowserCdpClient::connect(
    std::chrono::milliseconds timeout,
    const std::atomic<bool>* abort_flag,
    std::string& error) {
    close();
#ifndef _WIN32
    (void)timeout;
    (void)abort_flag;
    error = "Agent Browser tools are currently available on Windows Desktop only";
    return false;
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string last_error = "ACECode Desktop Agent Browser is not running";
    while (std::chrono::steady_clock::now() < deadline) {
        if (aborted(abort_flag)) {
            error = "Agent Browser operation aborted";
            return false;
        }
        const auto manifest =
            acecode::desktop::read_agent_browser_runtime_manifest();
        if (!manifest) {
            last_error = "Open ACECode Desktop to use Agent Browser tools";
        } else {
            const std::string validation =
                acecode::desktop::validate_agent_browser_runtime_manifest(
                    *manifest,
                    [](std::int64_t pid) {
                        return acecode::daemon::is_pid_alive(pid);
                    });
            if (!validation.empty()) {
                last_error = validation;
            } else {
                const std::wstring pipe_name =
                    acecode::utf8_to_wide(manifest->pipe_name);
                if (::WaitNamedPipeW(pipe_name.c_str(), 100)) {
                    impl_->pipe_name = pipe_name;
                    impl_->auth_token = manifest->auth_token;
                    impl_->ready = true;
                    error.clear();
                    return true;
                }
                last_error = "Agent Browser proxy is still starting";
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    error = last_error;
    return false;
#endif
}

void AgentBrowserCdpClient::close() {
    if (!impl_) return;
#ifdef _WIN32
    impl_->pipe_name.clear();
    impl_->auth_token.clear();
#endif
    impl_->page_id.clear();
    impl_->ready = false;
}

bool AgentBrowserCdpClient::connected() const {
    return impl_ && impl_->ready;
}

const std::string& AgentBrowserCdpClient::page_id() const {
    static const std::string empty;
    return impl_ ? impl_->page_id : empty;
}

bool AgentBrowserCdpClient::create_page(
    std::chrono::milliseconds timeout,
    const std::atomic<bool>* abort_flag,
    std::string& error) {
    request("create_page", "", nlohmann::json::object(), timeout,
            abort_flag, error);
    return error.empty() && !page_id().empty();
}

bool AgentBrowserCdpClient::claim_page(
    std::chrono::milliseconds timeout,
    const std::atomic<bool>* abort_flag,
    std::string& error) {
    request("claim_page", "", nlohmann::json::object(), timeout,
            abort_flag, error);
    return error.empty() && !page_id().empty();
}

bool AgentBrowserCdpClient::select_page(
    const std::string& page_id,
    std::chrono::milliseconds timeout,
    const std::atomic<bool>* abort_flag,
    std::string& error) {
    if (!impl_) {
        error = "Agent Browser proxy is not connected";
        return false;
    }
    impl_->page_id = page_id;
    request("select_page", "", nlohmann::json::object(), timeout,
            abort_flag, error);
    return error.empty();
}

bool AgentBrowserCdpClient::close_page(
    std::chrono::milliseconds timeout,
    const std::atomic<bool>* abort_flag,
    std::string& error) {
    request("close_page", "", nlohmann::json::object(), timeout,
            abort_flag, error);
    return error.empty();
}

nlohmann::json AgentBrowserCdpClient::command(
    const std::string& method,
    const nlohmann::json& params,
    std::chrono::milliseconds timeout,
    const std::atomic<bool>* abort_flag,
    std::string& error) {
    return request("cdp", method, params, timeout, abort_flag, error);
}

nlohmann::json AgentBrowserCdpClient::request(
    const std::string& operation,
    const std::string& method,
    const nlohmann::json& params,
    std::chrono::milliseconds timeout,
    const std::atomic<bool>* abort_flag,
    std::string& error) {
    if (!connected()) {
        error = "Agent Browser proxy is not connected";
        return {};
    }
#ifndef _WIN32
    (void)operation;
    (void)method;
    (void)params;
    (void)timeout;
    (void)abort_flag;
    error = "Agent Browser tools are currently available on Windows Desktop only";
    return {};
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    HANDLE pipe = open_proxy_pipe(
        impl_->pipe_name, deadline, abort_flag, error);
    if (pipe == INVALID_HANDLE_VALUE) return {};

    const auto close_pipe = [&pipe] {
        if (pipe != INVALID_HANDLE_VALUE) {
            ::CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
    };
    const auto timeout_count = (std::max)(
        std::int64_t{100},
        (std::min)(std::int64_t{120000}, timeout.count()));
    std::string request = nlohmann::json{
        {"auth_token", impl_->auth_token},
        {"operation", operation},
        {"page_id", impl_->page_id},
        {"method", method},
        {"params", params.is_object() ? params : nlohmann::json::object()},
        {"timeout_ms", timeout_count},
    }.dump();
    if (request.empty() ||
        request.size() > acecode::desktop::kAgentBrowserProxyMaxRequestBytes) {
        close_pipe();
        error = "Agent Browser proxy request is too large";
        return {};
    }
    std::uint32_t request_size = static_cast<std::uint32_t>(request.size());
    if (!pipe_transfer(pipe, &request_size, sizeof(request_size), true,
                       deadline, abort_flag, error) ||
        !pipe_transfer(pipe, request.data(), request.size(), true,
                       deadline, abort_flag, error)) {
        close_pipe();
        return {};
    }

    std::uint32_t response_size = 0;
    if (!pipe_transfer(pipe, &response_size, sizeof(response_size), false,
                       deadline, abort_flag, error) ||
        response_size == 0 ||
        response_size > acecode::desktop::kAgentBrowserProxyMaxResponseBytes) {
        close_pipe();
        if (error.empty()) error = "Agent Browser proxy response is invalid";
        return {};
    }
    std::string response_text(response_size, '\0');
    if (!pipe_transfer(pipe, response_text.data(), response_text.size(), false,
                       deadline, abort_flag, error)) {
        close_pipe();
        return {};
    }
    std::uint8_t acknowledgement =
        acecode::desktop::kAgentBrowserProxyResponseAck;
    std::string acknowledgement_error;
    pipe_transfer(pipe, &acknowledgement, sizeof(acknowledgement), true,
                  deadline, abort_flag, acknowledgement_error);
    close_pipe();

    auto response = nlohmann::json::parse(response_text, nullptr, false);
    if (!response.is_object()) {
        error = "Agent Browser proxy returned malformed JSON";
        return {};
    }
    if (!response.value("ok", false)) {
        const std::string response_page_id = response.value("page_id", "");
        if (!response_page_id.empty()) impl_->page_id = response_page_id;
        error = response.value("error", "Agent Browser CDP call failed");
        return {};
    }
    const std::string response_page_id = response.value("page_id", "");
    if (!response_page_id.empty()) impl_->page_id = response_page_id;
    error.clear();
    return response.value("result", nlohmann::json::object());
#endif
}

std::optional<std::vector<unsigned char>> decode_agent_browser_base64(
    const std::string& input) {
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> lookup(256, -1);
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        lookup[static_cast<unsigned char>(alphabet[index])] =
            static_cast<int>(index);
    }
    std::vector<unsigned char> output;
    int value = 0;
    int bits = -8;
    for (unsigned char ch : input) {
        if (std::isspace(ch)) continue;
        if (ch == '=') break;
        const int decoded = lookup[ch];
        if (decoded < 0) return std::nullopt;
        value = (value << 6) + decoded;
        bits += 6;
        if (bits >= 0) {
            output.push_back(
                static_cast<unsigned char>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return output;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>>
agent_browser_png_dimensions(const std::vector<unsigned char>& bytes) {
    static constexpr unsigned char kPngSignature[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    if (bytes.size() < 24 ||
        !std::equal(std::begin(kPngSignature), std::end(kPngSignature),
                    bytes.begin()) ||
        bytes[12] != 'I' || bytes[13] != 'H' ||
        bytes[14] != 'D' || bytes[15] != 'R') {
        return std::nullopt;
    }
    const auto read_be32 = [&bytes](std::size_t offset) {
        return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
               (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
               (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
               static_cast<std::uint32_t>(bytes[offset + 3]);
    };
    const std::uint32_t width = read_be32(16);
    const std::uint32_t height = read_be32(20);
    if (width == 0 || height == 0) return std::nullopt;
    return std::make_pair(width, height);
}

} // namespace acecode::agent_browser
