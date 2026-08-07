#include "daemon/platform.hpp"
#include "web/remote_web_proxy.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

std::string value_after(const std::string& value, const char* prefix) {
    const std::string marker(prefix);
    return value.rfind(marker, 0) == 0
        ? value.substr(marker.size())
        : std::string{};
}

int run_parent_death_probe(int argc, char* argv[]) {
    int target_port = 0;
    std::string runtime_dir;
    std::string state_file;
    for (int index = 2; index < argc; ++index) {
        const std::string token = argv[index] ? argv[index] : "";
        if (const auto value = value_after(token, "--target-port=");
            !value.empty()) {
            target_port = std::stoi(value);
        } else if (const auto value = value_after(token, "--runtime-dir=");
                   !value.empty()) {
            runtime_dir = value;
        } else if (const auto value = value_after(token, "--state-file=");
                   !value.empty()) {
            state_file = value;
        }
    }
    acecode::web::ManagedRemoteWebProxyController controller(
        argv[0] ? argv[0] : "",
        runtime_dir,
        acecode::daemon::current_pid());
    const auto proxy = controller.start(0, target_port);
    std::ofstream output(state_file, std::ios::binary | std::ios::trunc);
    output << nlohmann::json{
        {"running", proxy.running},
        {"pid", proxy.pid},
        {"port", proxy.port},
        {"error", proxy.error},
    }.dump();
    output.close();
    // Deliberately bypass controller destruction to simulate an owning daemon
    // disappearing without its graceful stop path.
    std::_Exit(proxy.running ? 0 : 1);
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc >= 2 &&
        std::string(argv[1] ? argv[1] : "") == "--parent-death-probe") {
        return run_parent_death_probe(argc, argv);
    }
    std::vector<std::string> tokens;
    for (int index = 1; index < argc; ++index) {
        tokens.emplace_back(argv[index] ? argv[index] : "");
    }
    if (!tokens.empty() && tokens.front() == "--remote-web-proxy") {
        tokens.erase(tokens.begin());
    }
    return acecode::web::run_remote_web_proxy_command(
        tokens, std::cout, std::cerr);
}
