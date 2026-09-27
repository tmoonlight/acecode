#include "hook_payload.hpp"

#include "hook_config.hpp"
#include "platform/process/os_process.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"

namespace acecode {

nlohmann::json build_startup_before_model_load_payload(const std::string& cwd) {
    std::string config_path = path_to_utf8(path_from_utf8(get_acecode_dir()) / "config.json");
    return {
        {"schema_version", 1},
        {"event", kHookEventStartupBeforeModelLoad},
        {"timestamp", iso_timestamp()},
        {"process", {
            {"pid", static_cast<int>(daemon::current_pid())},
            {"cwd", cwd},
        }},
        {"config", {
            {"path", config_path},
        }},
    };
}

} // namespace acecode
