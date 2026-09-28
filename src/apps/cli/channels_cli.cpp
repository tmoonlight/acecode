#include "channels_cli.hpp"
#include "channels/runtime.hpp"
#include "tui/channels_setup.hpp"
#include "utils/utf8_path.hpp"
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace acecode::channels {
std::string command_help() {
    return "acecode channels [setup]  - configure WhatsApp; no daemon is started\n"
           "acecode channels status  - show connection or saved configuration\n"
           "Runtime commands (require an already running daemon/Desktop):\n"
           "acecode channels on | off | qr | reconnect\n"
           "acecode channels pending | approve <pairing-code>\n"
           "acecode channels allow <peer-or-group-jid> | revoke <jid>\n"
           "acecode channels sessions | show <session-id> | stop <session-id>\n"
           "acecode channels send <session-id> <text, /aq answer, /approve id or /deny id>\n"
           "acecode channels file <session-id> <local-path>\n"
           "After configuration, start acecode daemon or Desktop to connect. The first host owns the channel.\n"
           "WhatsApp personal account; Node.js 22+ required. Groups require an allowlisted sender and a mention.";
}
Json parse_command(const std::string& arguments) {
    std::istringstream input(arguments);
    std::string op; input >> op;
    if (op.empty()) op = "setup";
    Json result{{"op", op}};
    static const std::set<std::string> simple{"setup", "status", "help", "on", "off", "qr", "reconnect", "pending", "sessions"};
    if (op == "allow" || op == "revoke" || op == "approve") {
        std::string value; input >> value;
        if (value.empty()) throw std::runtime_error(command_help());
        result[op == "approve" ? "code" : "jid"] = value;
    } else if (op == "show" || op == "stop" || op == "send" || op == "file") {
        std::string id; input >> id;
        if (id.empty()) throw std::runtime_error("Session id required");
        result["session_id"] = id;
        if (op == "send" || op == "file") {
            std::string text;
            input >> std::ws;
            if (op == "file" && input.peek() == '"') {
                // Backslashes in Windows paths are literal, not shell escapes.
                input >> std::quoted(text, '"', '\0');
                if (input.fail()) throw std::runtime_error("Unclosed file path quote");
            } else std::getline(input, text);
            if (text.empty()) throw std::runtime_error(op == "file" ? "File path required" : "Message required");
            result[op == "file" ? "path" : "text"] = text;
        }
    } else if (!simple.count(op)) throw std::runtime_error(command_help());
    std::string extra;
    if (input >> extra) throw std::runtime_error("Unexpected channel command argument");
    return result;
}
std::string format_result(const Json& result) {
    if (result.contains("text")) return result.at("text").get<std::string>();
    std::ostringstream text;
    if (result.contains("state")) {
        text << "WhatsApp: " << result.value("state", "unknown");
        if (!result.value("account", std::string{}).empty()) text << "\nAccount: " << result.at("account").get<std::string>();
        if (!result.value("error", std::string{}).empty()) text << "\nError: " << result.at("error").get<std::string>();
        if (!result.value("qr_text", std::string{}).empty()) text << "\n```text\n" << result.at("qr_text").get<std::string>() << "\n```";
        if (result.contains("access")) text << "\nAccess: " << result.at("access").dump();
        return text.str();
    }
    if (result.contains("sessions")) {
        for (const auto& session : result.at("sessions")) {
            text << session.at("session_id").get<std::string>() << "  "
                 << session.at("address").at("chat").get<std::string>();
            if (session.at("address").value("group", false)) text << " / " << session.at("address").at("sender").get<std::string>();
            text << "  failed=" << session.value("failed", 0u) << " dropped=" << session.value("dropped", 0u) << '\n';
        }
        return text.str().empty() ? "No channel sessions." : text.str();
    }
    if (result.contains("messages")) {
        for (const auto& message : result.at("messages"))
            text << message.value("role", "message") << ": " << message.value("content", "") << "\n\n";
        for (const auto& p : result.value("permissions", Json::array()))
            text << "Permission " << p.at("request_id").get<std::string>() << ": " << p.value("tool", "") << "\n" << p.value("args", Json::object()).dump() << "\n";
        for (const auto& q : result.value("questions", Json::array())) text << q.get<std::string>() << '\n';
        return text.str().empty() ? "No messages or pending requests." : text.str();
    }
    return result.dump(2);
}
int run_cli(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& error) {
    try {
        std::string joined;
        for (const auto& part : arguments) { if (!joined.empty()) joined += ' '; joined += part; }
        if (joined == "--help") joined = "help";
        auto command = parse_command(joined);
        const auto op = command.at("op").get<std::string>();
        if (op == "setup") return tui::run_channels_setup();
        if (op == "help") out << command_help() << '\n';
        else {
            if (op == "file") command["path"] = path_to_utf8(std::filesystem::absolute(path_from_utf8(command.at("path").get<std::string>())));
            out << format_result(request_control(command)) << '\n';
        }
        return 0;
    } catch (const std::exception& e) { error << "Channels: " << e.what() << '\n'; return 1; }
}
} // namespace acecode::channels
