#include "setup.hpp"
#include "runtime.hpp"
#include "hooks/hook_runner.hpp"
#include "lsp/lsp_which.hpp"
#include "remote_control/rc_session_navigation.hpp"
#include "remote_control/remote_control_service.hpp"
#include "utils/utf8_path.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace acecode::channels {
std::vector<std::string> setup_contacts(const std::string& phone_numbers) {
    if (phone_numbers.size() > 4096) throw std::runtime_error("Too many phone numbers");
    std::string value = phone_numbers;
    std::replace(value.begin(), value.end(), '\n', ',');
    std::replace(value.begin(), value.end(), ';', ',');
    std::vector<std::string> contacts;
    std::istringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        std::string digits;
        for (unsigned char c : item) {
            if (c >= '0' && c <= '9') digits += static_cast<char>(c);
            else if (!std::isspace(c) && c != '+' && c != '-' && c != '(' && c != ')')
                throw std::runtime_error("Use phone numbers with country codes, separated by commas");
        }
        if (digits.empty()) continue;
        if (digits.size() < 5 || digits.size() > 15)
            throw std::runtime_error("Phone numbers must contain 5 to 15 digits including the country code");
        const auto jid = digits + "@s.whatsapp.net";
        if (std::find(contacts.begin(), contacts.end(), jid) == contacts.end()) contacts.push_back(jid);
    }
    if (contacts.size() > 127) throw std::runtime_error("Too many phone numbers");
    return contacts;
}
bool supported_node_version(const std::string& version) {
    std::istringstream input(version);
    char prefix = 0, dot = 0;
    int major = 0;
    return bool(input >> prefix >> major >> dot) && prefix == 'v' && dot == '.' && major >= 22;
}
void install_whatsapp_dependencies(const std::filesystem::path& directory,
                                  const std::atomic<bool>& cancelled,
                                  const SetupProgress& progress) {
    const auto node = lsp::which("node");
    if (!node) throw std::runtime_error("Node.js 22+ was not found. Install Node.js, then retry.");
    HookProcessOptions options;
    options.timeout_ms = 5000; options.abort_flag = &cancelled;
    options.terminate_process_tree = true;
    const auto version = run_hook_process({*node, {"--version"}}, "", "", options);
    if (version.aborted || cancelled) throw std::runtime_error("Setup cancelled");
    if (!version.started || version.exit_code != 0 || !supported_node_version(version.stdout_text))
        throw std::runtime_error("Node.js 22+ is required. Update Node.js, then retry.");
    const auto installed = prepare_whatsapp_bridge(directory);
    if (whatsapp_dependencies_ready(installed)) return;
    const auto npm = lsp::which("npm");
    if (!npm) throw std::runtime_error("npm was not found. Install Node.js with npm, then retry.");
    if (progress) progress({SetupPhase::Installing});
    options.timeout_ms = 300000;
    options.max_stdout_bytes = 8192; options.max_stderr_bytes = 8192;
    HookCommandSpec install{*npm, {"ci", "--omit=dev", "--no-fund", "--no-audit", "--progress=false"}};
#ifdef _WIN32
    const auto cli = path_from_utf8(*npm).parent_path() / "node_modules/npm/bin/npm-cli.js";
    if (!std::filesystem::is_regular_file(cli))
        throw std::runtime_error("npm installation is incomplete. Reinstall Node.js with npm, then retry.");
    install.command = *node;
    install.args.insert(install.args.begin(), path_to_utf8(cli));
#endif
    const auto result = run_hook_process(install, "", path_to_utf8(installed), options);
    if (result.aborted || cancelled) throw std::runtime_error("Setup cancelled");
    if (!result.started || result.exit_code != 0 || result.timed_out || !whatsapp_dependencies_ready(installed)) {
        auto detail = result.error.empty() ? result.stderr_text : result.error;
        if (!detail.empty()) detail = rc::chunk_rc_session_output(detail, 2000).back();
        throw std::runtime_error("Could not install WhatsApp dependencies. Check the network and retry.\n" + detail);
    }
}
namespace {
std::string saved_account(const std::filesystem::path& directory) {
    const auto path = directory / "auth" / "creds.json";
    if (!std::filesystem::exists(path)) return {};
    if (std::filesystem::file_size(path) > 1024 * 1024)
        throw std::runtime_error("Saved WhatsApp login is too large");
    std::ifstream file(path, std::ios::binary);
    const auto credentials = Json::parse(file, nullptr, false);
    if (!credentials.is_object()) throw std::runtime_error("Cannot read saved WhatsApp login. Retry saving.");
    const auto me = credentials.find("me");
    if (me == credentials.end() || me->is_null()) return {};
    if (!me->is_object() || !me->contains("id") || !me->at("id").is_string())
        throw std::runtime_error("Invalid saved WhatsApp account");
    const auto id = me->at("id").get<std::string>();
    const auto separator = id.find('@');
    if (separator == std::string::npos) throw std::runtime_error("Invalid saved WhatsApp account");
    // Baileys normalizes user[_agent][:device]@server to the account JID.
    const auto user = id.substr(0, std::min(separator, id.find_first_of("_:")));
    const auto server = id.substr(separator + 1);
    const auto account = user + "@" + (server == "c.us" ? "s.whatsapp.net" : server);
    if (!valid_peer(account)) throw std::runtime_error("Invalid saved WhatsApp account");
    return account;
}
struct LocalSetup {
    std::filesystem::path directory;
    Runtime::SpawnOptions spawn;
    State state;
    Bridge bridge;
    std::filesystem::path pairing_directory;
    std::string account, profile;
    bool created_profile = false, committed = false;

    LocalSetup(std::filesystem::path directory, Runtime::SpawnOptions spawn)
        : directory(std::move(directory)), spawn(std::move(spawn)), state(this->directory) {}
    ~LocalSetup() { close(); }

    void begin() {
        close();
        committed = false;
        state.load();
        profile = state.snapshot().at("profile").get<std::string>();
        account = saved_account(state.transport_directory());
        if (!account.empty()) return;
        // Pair into a new device profile; never open an existing host's credentials.
        std::filesystem::create_directories(directory / "profiles");
        do {
            profile = rc::generate_remote_control_token();
            pairing_directory = directory / "profiles" / profile;
        } while (!std::filesystem::create_directory(pairing_directory));
        created_profile = true;
    }
    void prepare(const std::atomic<bool>& cancel, const SetupProgress& progress) {
        if (account.empty()) install_whatsapp_dependencies(pairing_directory, cancel, progress);
    }
    void connect() {
        if (!account.empty()) return;
        auto options = spawn(pairing_directory);
        options.argv.push_back("--setup-only");
        bridge.start(options);
        bridge.request("connect", Json::object(), std::chrono::seconds(10));
    }
    Json status() {
        if (!account.empty()) return {{"state", "linked"}, {"account", account}};
        // Pairing has no Gateway or SessionClient, and never consumes messages.
        for (const auto& event : bridge.take_events())
            if (event.value("event", "") == "error")
                throw std::runtime_error(event.value("error", "WhatsApp pairing failed"));
        auto result = bridge.request("status", Json::object(), std::chrono::seconds(10));
        result.erase("qr");
        return result;
    }
    void save(const std::string& account, const std::vector<std::string>& peers) {
        const auto current = status();
        if ((current.value("state", "") != "connected" && current.value("state", "") != "linked") || !valid_peer(account) ||
            current.value("account", "") != account)
            throw std::runtime_error("WhatsApp account changed before saving. Retry setup.");
        if (bridge.running()) {
            // Disconnect waits for this setup's credential writes, never a host's.
            bridge.request("disconnect", Json::object(), std::chrono::seconds(10));
            bridge.stop();
        }
        state.enable_with_access(account, peers, profile);
        committed = true;
    }
    void close() {
        bridge.stop();
        if (created_profile && !committed && path_inside(pairing_directory, directory / "profiles")) {
            std::error_code ec;
            std::filesystem::remove_all(pairing_directory, ec);
        }
        created_profile = false;
    }
};
} // namespace

SetupDependencies default_setup_dependencies(std::filesystem::path directory, Runtime::SpawnOptions spawn) {
    if (directory.empty()) directory = channel_directory();
    if (!spawn) spawn = whatsapp_bridge_options;
    auto session = std::make_shared<LocalSetup>(std::move(directory), std::move(spawn));
    SetupDependencies deps;
    deps.begin = [session] { session->begin(); };
    deps.prepare = [session](const std::atomic<bool>& cancel, const SetupProgress& progress) {
        session->prepare(cancel, progress);
    };
    deps.connect = [session] { session->connect(); };
    deps.status = [session] { return session->status(); };
    deps.save = [session](const std::string& account, const std::vector<std::string>& peers) { session->save(account, peers); };
    deps.close = [session] { session->close(); };
    deps.wait = [] { std::this_thread::sleep_for(std::chrono::milliseconds(250)); };
    return deps;
}
void run_setup(const std::vector<std::string>& contacts, SetupDependencies deps,
               const std::atomic<bool>& cancelled, const SetupProgress& progress) {
    bool started = false;
    auto report = [&](SetupUpdate update) { if (progress) progress(update); };
    auto check_cancel = [&] { if (cancelled) throw std::runtime_error("Setup cancelled"); };
    try {
        check_cancel();
        if (!deps.begin || !deps.prepare || !deps.connect || !deps.status || !deps.save || !deps.close)
            throw std::runtime_error("Setup services unavailable");
        report({SetupPhase::Preparing});
        started = true;
        deps.begin();
        check_cancel();
        deps.prepare(cancelled, progress);
        check_cancel();
        report({SetupPhase::Connecting});
        deps.connect();
        const auto deadline = std::chrono::steady_clock::now() + deps.pairing_timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            check_cancel();
            const auto status = deps.status();
            const auto state = status.value("state", "connecting");
            if (state == "connected" || state == "linked") {
                const auto account = status.at("account").get<std::string>();
                auto peers = contacts;
                if (std::find(peers.begin(), peers.end(), account) == peers.end()) peers.push_back(account);
                check_cancel();
                deps.save(account, peers);
                deps.close();
                started = false;
                report({SetupPhase::Complete, {}, {}, account});
                return;
            }
            if (state == "logged_out" || state == "error") {
                auto error = status.value("error", std::string{});
                throw std::runtime_error(error.empty() ? "WhatsApp connection failed. Retry pairing." : error);
            }
            report({SetupPhase::Pairing, {}, status.value("qr_text", "")});
            if (deps.wait) deps.wait();
        }
        throw std::runtime_error("Pairing timed out. Retry to display a new QR code.");
    } catch (const std::exception& e) {
        if (started && deps.close) { try { deps.close(); } catch (...) {} }
        report({cancelled ? SetupPhase::Cancelled : SetupPhase::Failed, cancelled ? "" : e.what()});
    }
}
} // namespace acecode::channels
