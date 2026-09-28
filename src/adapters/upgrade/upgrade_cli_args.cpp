#include "upgrade_cli_args.hpp"
#include <ostream>

namespace acecode::upgrade {

std::optional<int> parse_upgrade_cli_args(int argc, char* argv[], bool& force_update,
    std::optional<std::string>& server_override, std::ostream& error) {
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i] ? std::string(argv[i]) : std::string();
        if (arg == "--force") {
            force_update = true;
            continue;
        }
        constexpr const char* kServerPrefix = "--server=";
        if (arg.rfind(kServerPrefix, 0) == 0) {
            server_override = arg.substr(std::char_traits<char>::length(kServerPrefix));
            continue;
        }
        if (arg == "--server") {
            error << "acecode " << argv[1] << ": missing value for --server\n"
                      << "usage: acecode " << argv[1]
                      << " [--force] [--server=<url>]\n";
            return 64;
        }
        error << "acecode " << argv[1] << ": unknown option: " << arg << "\n"
                  << "usage: acecode " << argv[1]
                  << " [--force] [--server=<url>]\n";
        return 64;
    }
    return std::nullopt;
}

} // namespace acecode::upgrade
