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
    AgentBrowserCdpClient();
    ~AgentBrowserCdpClient();

    AgentBrowserCdpClient(const AgentBrowserCdpClient&) = delete;
    AgentBrowserCdpClient& operator=(const AgentBrowserCdpClient&) = delete;

    bool connect(std::chrono::milliseconds timeout,
                 const std::atomic<bool>* abort_flag,
                 std::string& error);
    void close();
    bool connected() const;
    const std::string& page_id() const;

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

std::optional<std::vector<unsigned char>> decode_agent_browser_base64(
    const std::string& input);

std::optional<std::pair<std::uint32_t, std::uint32_t>>
agent_browser_png_dimensions(const std::vector<unsigned char>& bytes);

} // namespace acecode::agent_browser
