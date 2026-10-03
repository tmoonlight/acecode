#include "swarm_command.hpp"

#include <cctype>

namespace acecode {

namespace {

std::string trim_lower(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    std::string out = text.substr(begin, end - begin);
    for (auto& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

const char* describe(SwarmMode mode) {
    switch (mode) {
    case SwarmMode::Star: return "star (spawn_subagent fan-out)";
    case SwarmMode::Mesh: return "mesh (agent_* collaboration tree)";
    case SwarmMode::Off: break;
    }
    return "off";
}

} // namespace

SwarmCommandRequest parse_swarm_command(const std::string& args) {
    SwarmCommandRequest request;
    const std::string value = trim_lower(args);
    if (value.empty()) {
        request.show = true;
        return request;
    }
    // 旧布尔字面量只给消息接口兼容用,命令只认三个规范名。
    if (value != "star" && value != "mesh" && value != "off") {
        request.error = "Invalid swarm mode: " + value + " (allowed: star, mesh, off)";
        return request;
    }
    request.mode = parse_swarm_mode(value);
    return request;
}

std::string swarm_command_status_text(SwarmMode current) {
    return std::string("Swarm mode: ") + describe(current) + "\nUsage: /swarm [star|mesh|off]";
}

std::string swarm_command_applied_text(SwarmMode mode) {
    return std::string("Swarm mode set to ") + describe(mode) + ".";
}

} // namespace acecode
