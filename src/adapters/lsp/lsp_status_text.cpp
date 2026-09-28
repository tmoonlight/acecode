#include "lsp_status_text.hpp"

#include <sstream>

namespace acecode {

std::string format_lsp_status(const lsp::LspService::Status& status) {
    std::ostringstream oss;
    oss << "LSP:\n";
    oss << "  Enabled    : " << (status.enabled ? "yes" : "no") << "\n";
    if (!status.enabled) {
        oss << "  Enable with config.json: {\"lsp\": {\"enabled\": true}}\n";
        return oss.str();
    }

    oss << "  Connected  : ";
    if (status.connected.empty()) {
        oss << "(none — servers start lazily on first matching file)\n";
    } else {
        oss << "\n";
        for (const auto& entry : status.connected) {
            oss << "    * " << entry.server_id << "  root=" << entry.root
                << "  files=" << entry.open_files << "\n";
        }
    }
    if (!status.broken.empty()) {
        oss << "  Broken     : ";
        for (std::size_t i = 0; i < status.broken.size(); ++i) {
            if (i) oss << ", ";
            oss << status.broken[i].server_id << " (root=" << status.broken[i].root << ")";
        }
        oss << "  — failed to start; restart acecode to retry\n";
    }
    if (!status.not_installed.empty()) {
        oss << "  Not found  : ";
        for (std::size_t i = 0; i < status.not_installed.size(); ++i) {
            if (i) oss << ", ";
            oss << status.not_installed[i];
        }
        oss << "  — executable not on PATH; install to enable\n";
    }
    return oss.str();
}

std::string dispatch_lsp_subcommand(const std::string& sub) {
    if (!lsp::is_initialized()) {
        return "LSP runtime is not initialized in this process.";
    }
    if (sub.empty() || sub == "show" || sub == "status") {
        return format_lsp_status(lsp::service().status_snapshot());
    }
    return "Unknown subcommand. Usage:\n"
           "  /lsp    Show LSP integration status";
}

} // namespace acecode
