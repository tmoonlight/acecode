#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::agent_browser {

struct AgentBrowserElementRef {
    int index = 0;
};

std::optional<AgentBrowserElementRef> parse_agent_browser_element_ref(
    const std::string& value);

class AgentBrowserCdpClient {
public:
    explicit AgentBrowserCdpClient(std::string acecode_dir = {});
    ~AgentBrowserCdpClient();

    AgentBrowserCdpClient(const AgentBrowserCdpClient&) = delete;
    AgentBrowserCdpClient& operator=(const AgentBrowserCdpClient&) = delete;

    bool connect(std::chrono::milliseconds timeout,
                 const std::atomic<bool>* abort_flag,
                 std::string& error);
    void close();
    bool connected() const;
    const std::string& page_id() const;

    // 调用方会话身份(见 agent_browser_owner_from_context)。设置后每个代理请求
    // 都带 `owner`,Desktop 据此把新页归到该会话、并按会话解析默认目标页。
    // 非对象输入视为未绑定(旧式请求)。
    void set_owner(nlohmann::json owner);
    const nlohmann::json& owner() const;

    bool create_page(std::chrono::milliseconds timeout,
                     const std::atomic<bool>* abort_flag,
                     std::string& error);
    bool claim_page(std::chrono::milliseconds timeout,
                    const std::atomic<bool>* abort_flag,
                    std::string& error);
    bool select_page(const std::string& page_id,
                     std::chrono::milliseconds timeout,
                     const std::atomic<bool>* abort_flag,
                     std::string& error);
    bool close_page(std::chrono::milliseconds timeout,
                    const std::atomic<bool>* abort_flag,
                    std::string& error);

    nlohmann::json command(
        const std::string& method,
        const nlohmann::json& params,
        std::chrono::milliseconds timeout,
        const std::atomic<bool>* abort_flag,
        std::string& error);

private:
    nlohmann::json request(
        const std::string& operation,
        const std::string& method,
        const nlohmann::json& params,
        std::chrono::milliseconds timeout,
        const std::atomic<bool>* abort_flag,
        std::string& error);

    struct Impl;
    Impl* impl_ = nullptr;
};

// 代理请求报文的唯一构造点。timeout_ms 钳制到 [100, 120000];owner 只在是对象时
// 写入。抽成纯函数是为了让 owner 透传可以离线单测。
nlohmann::json build_agent_browser_proxy_request(
    const std::string& auth_token,
    const std::string& operation,
    const std::string& page_id,
    const std::string& method,
    const nlohmann::json& params,
    std::int64_t timeout_ms,
    const nlohmann::json& owner);

std::optional<std::vector<unsigned char>> decode_agent_browser_base64(
    const std::string& input);

std::optional<std::pair<std::uint32_t, std::uint32_t>>
agent_browser_png_dimensions(const std::vector<unsigned char>& bytes);

} // namespace acecode::agent_browser
