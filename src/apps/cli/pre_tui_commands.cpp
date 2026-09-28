#include "pre_tui_commands.hpp"
#include "interactive_options.hpp"
#include "cli/configure/configure.hpp"
#include "skills/default_skill_startup.hpp"
#include "provider/models_dev_registry.hpp"
#include "config/config.hpp"
#include <iostream>

namespace acecode::cli {

int validate_models_registry_command(const std::string& argv0_dir) {
    AppConfig config = load_config();
    reconcile_default_skills_on_startup(argv0_dir);
    initialize_registry(config, argv0_dir);
    const auto& src = current_registry_source();
    auto registry = current_registry();
    if (!registry || registry->empty()) {
        std::cerr << "models.dev registry not found or empty\n";
        return 1;
    }
    size_t actual_models = 0;
    for (auto it = registry->begin(); it != registry->end(); ++it) {
        if (!it->is_object()) continue;
        auto m = it->find("models");
        if (m == it->end()) continue;
        if (m->is_object()) actual_models += m->size();
        else if (m->is_array()) actual_models += m->size();
    }
    std::cout << "models.dev registry OK: " << registry->size() << " providers, "
              << actual_models << " models, source=" << src.path_or_url << "\n";
    if (src.manifest && src.manifest->is_object()) {
        const auto& m = *src.manifest;
        if (m.contains("model_count") && m["model_count"].is_number_integer()) {
            size_t expected = static_cast<size_t>(m["model_count"].get<int>());
            if (expected != actual_models) {
                std::cerr << "MANIFEST.json model_count=" << expected
                          << " disagrees with actual " << actual_models << "\n";
                return 1;
            }
        }
    }
    return 0;
}

std::optional<int> run_pre_tui_command(const InteractiveCliOptions& cli,
                                              const std::string& argv0_dir) {
    if (cli.run_configure_cmd) {
        AppConfig config = load_config();
        reconcile_default_skills_on_startup(argv0_dir);
        initialize_registry(config, argv0_dir);
        return run_configure(config);
    }

    if (cli.validate_models_registry_cmd) {
        return validate_models_registry_command(argv0_dir);
    }

    return std::nullopt;
}


} // namespace acecode::cli
