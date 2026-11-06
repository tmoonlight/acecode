#include "open_in_explorer_bridge.hpp"

#include "web_host.hpp"

#include "platform/native_ui/open_in_explorer.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <string>

namespace acecode::desktop {
namespace {

std::string error_json(const std::string& error) {
    return nlohmann::json{{"ok", false}, {"error", error}}.dump();
}

std::string open_result_json(const std::string& path) {
    try {
        const auto result = open_path_in_file_manager(path);
        return result.ok ? nlohmann::json{{"ok", true}}.dump() : error_json(result.error);
    } catch (const std::exception& e) {
        return error_json(e.what());
    } catch (...) {
        return error_json("failed to open path in file manager");
    }
}

} // namespace

OpenInExplorerBridge::OpenInExplorerBridge(WebHost& host) {
    host.bind_async("aceDesktop_openInExplorer",
        [self = lifetime_.ref(*this)](const std::string& req, WebHost::BindingReply reply) {
            std::string path;
            try {
                const auto args = nlohmann::json::parse(req);
                if (!args.is_array() || args.empty() || !args[0].is_string()) {
                    reply(error_json("expect [path]"));
                    return;
                }
                path = args[0].get<std::string>();
            } catch (const std::exception& e) {
                reply(error_json(std::string("parse: ") + e.what()));
                return;
            }
            bool started = false;
            self.with([&started, &path, &reply](OpenInExplorerBridge& bridge) {
                try {
                    started = bridge.workers_.spawn([path, reply] {
                        try {
                            reply(open_result_json(path));
                        } catch (...) {
                            // The page only loses this toast; never let a worker terminate.
                        }
                    });
                } catch (const std::exception&) {
                    started = false;
                }
            });
            if (!started) reply(error_json("desktop is shutting down"));
        });
}

OpenInExplorerBridge::~OpenInExplorerBridge() {
    lifetime_.revoke();
    workers_.shutdown();
}

} // namespace acecode::desktop
