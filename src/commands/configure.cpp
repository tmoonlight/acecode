#include "configure.hpp"
#include "config/config.hpp"
#include "auth/github_auth.hpp"
#include "utils/terminal_input.hpp"

#include <iostream>
#include <string>
#include <algorithm>
#include <cpr/cpr.h>
#include <cpr/ssl_options.h>
#include <nlohmann/json.hpp>

namespace acecode {

// Mask an API key for display: "****" + last 4 chars
static std::string mask_key(const std::string& key) {
    if (key.empty()) return "(not set)";
    if (key.size() > 4) {
        return "****" + key.substr(key.size() - 4);
    }
    return "****";
}

static void configure_copilot(AppConfig& cfg) {
    std::cout << "\n--- Copilot Configuration ---\n" << std::endl;

    // Authentication first (needed to fetch available models)
    std::string github_token = load_github_token();
    if (github_token.empty()) {
        std::cout << "GitHub authentication required." << std::endl;
    } else {
        std::cout << "GitHub authentication: Already authenticated." << std::endl;
        if (read_confirm("Re-authenticate?", false)) {
            github_token.clear(); // force re-auth below
        }
    }

    if (github_token.empty()) {
        std::cout << "\nStarting GitHub authentication..." << std::endl;
        DeviceCodeResponse dc = request_device_code();
        if (dc.device_code.empty()) {
            std::cerr << "Error: Failed to request device code from GitHub." << std::endl;
            return;
        }

        std::cout << "\n  Please open: " << dc.verification_uri << std::endl;
        std::cout << "  Enter code:  " << dc.user_code << std::endl;
        std::cout << std::endl;

        github_token = poll_for_access_token(
            dc.device_code, dc.interval, dc.expires_in,
            [](const std::string& status) {
                std::cout << "  " << status << std::endl;
            }
        );

        if (github_token.empty()) {
            std::cerr << "Authentication failed or timed out." << std::endl;
            return;
        }

        save_github_token(github_token);
        std::cout << "Authentication successful!" << std::endl;
    }

    // Fetch available models from Copilot API
    std::cout << "\nFetching available models..." << std::endl;
    CopilotToken ct = exchange_copilot_token(github_token);

    std::vector<std::string> model_ids;
    if (!ct.token.empty()) {
        cpr::Response r = cpr::Get(
            cpr::Url{"https://api.githubcopilot.com/models"},
            cpr::Header{
                {"Authorization", "Bearer " + ct.token},
                {"Editor-Version", "acecode/0.1.0"},
                {"Editor-Plugin-Version", "acecode/0.1.0"},
                {"Copilot-Integration-Id", "vscode-chat"},
                {"Openai-Intent", "conversation-panel"}
            },
            cpr::Ssl(cpr::ssl::NoRevoke{true}),
            cpr::Timeout{10000}
        );

        if (r.status_code == 200) {
            try {
                auto j = nlohmann::json::parse(r.text);
                if (j.contains("data") && j["data"].is_array()) {
                    for (const auto& m : j["data"]) {
                        if (!m.contains("id") || !m["id"].is_string()) continue;
                        // Filter to chat-capable models only
                        if (m.contains("capabilities")) {
                            const auto& caps = m["capabilities"];
                            if (caps.contains("type") && caps["type"].is_string()
                                && caps["type"].get<std::string>() != "chat") {
                                continue;
                            }
                        }
                        model_ids.push_back(m["id"].get<std::string>());
                    }
                }
            } catch (...) {}
        }
    }

    // Model selection
    if (!model_ids.empty()) {
        // Find default index matching current config
        int default_idx = 0;
        for (size_t i = 0; i < model_ids.size(); ++i) {
            if (model_ids[i] == cfg.copilot.model) {
                default_idx = static_cast<int>(i);
                break;
            }
        }
        int choice = read_choice("Select model:", model_ids, default_idx);
        cfg.copilot.model = model_ids[choice];
    } else {
        std::cerr << "Warning: Could not fetch model list from Copilot API." << std::endl;
        cfg.copilot.model = read_line("Model", cfg.copilot.model);
    }
}

static void configure_openai(AppConfig& cfg) {
    std::cout << "\n--- OpenAI Compatible Configuration ---\n" << std::endl;

    // Base URL
    cfg.openai.base_url = read_line("Base URL", cfg.openai.base_url);

    // API Key (masked)
    cfg.openai.api_key = read_password("API Key", cfg.openai.api_key);

    // Model
    cfg.openai.model = read_line("Model", cfg.openai.model);

    // Optional connection test
    if (read_confirm("\nTest connection?", true)) {
        std::cout << "Testing connection to " << cfg.openai.base_url << "/models ..." << std::endl;

        cpr::Header headers;
        if (!cfg.openai.api_key.empty()) {
            headers["Authorization"] = "Bearer " + cfg.openai.api_key;
        }

        auto r = cpr::Get(
            cpr::Url{cfg.openai.base_url + "/models"},
            headers,
            cpr::Ssl(cpr::ssl::NoRevoke{true}),
            cpr::Timeout{10000}
        );

        if (r.status_code == 200) {
            std::cout << "Connection successful!" << std::endl;
        } else if (r.status_code == 0) {
            std::cerr << "Connection failed: " << r.error.message << std::endl;
            if (!read_confirm("Save configuration anyway?", true)) {
                return;
            }
        } else {
            std::cerr << "Connection failed: HTTP " << r.status_code << std::endl;
            if (!read_confirm("Save configuration anyway?", true)) {
                return;
            }
        }
    }
}

int run_configure(const AppConfig& current_config) {
    AppConfig cfg = current_config;

    std::cout << "\n=== acecode Configuration Wizard ===\n" << std::endl;

    // Provider selection
    int default_provider = (cfg.provider == "openai") ? 1 : 0;
    int choice = read_choice("Select provider:",
        {"Copilot (GitHub)", "OpenAI Compatible"},
        default_provider);

    if (choice == 0) {
        cfg.provider = "copilot";
        configure_copilot(cfg);
    } else {
        cfg.provider = "openai";
        configure_openai(cfg);
    }

    // Configuration summary
    std::cout << "\n--- Configuration Summary ---" << std::endl;
    std::cout << "  Provider: " << cfg.provider << std::endl;
    if (cfg.provider == "copilot") {
        std::cout << "  Model:    " << cfg.copilot.model << std::endl;
    } else {
        std::cout << "  Base URL: " << cfg.openai.base_url << std::endl;
        std::cout << "  API Key:  " << mask_key(cfg.openai.api_key) << std::endl;
        std::cout << "  Model:    " << cfg.openai.model << std::endl;
    }
    std::cout << std::endl;

    if (read_confirm("Save configuration?", true)) {
        save_config(cfg);
        std::cout << "Configuration saved!" << std::endl;
    } else {
        std::cout << "Configuration cancelled." << std::endl;
    }

    return 0;
}

} // namespace acecode
