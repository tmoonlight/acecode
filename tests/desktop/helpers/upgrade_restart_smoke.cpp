// Opt-in runtime regression with an old packaged daemon and the new daemon.
// The caller supplies an isolated USERPROFILE/HOME and temporary run directory.
#include "platform/process/os_process.hpp"
#include "ipc/runtime_files.hpp"
#include "desktop/daemon_pool.hpp"
#include "ipc/daemon_protocol.hpp"
#include "desktop/daemon_supervisor.hpp"
#include "version.hpp"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace acecode::desktop;
namespace fs = std::filesystem;

nlohmann::json health(int port, const std::string& token) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto response = cpr::Get(cpr::Url{"http://127.0.0.1:" + std::to_string(port) + "/api/health"},
            cpr::Header{{"X-ACECode-Token", token}}, cpr::Proxies{{"http", ""}, {"https", ""}},
            cpr::Timeout{1000});
        if (response.status_code == 200) return nlohmann::json::parse(response.text);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("daemon health did not become ready");
}

int main(int argc, char** argv) {
    if (argc != 4) return 64;
    DaemonSupervisor old;
    DaemonPool pool;
    try {
        const std::string run_dir = fs::absolute(fs::u8path(argv[3])).u8string();
        fs::create_directories(fs::u8path(run_dir));
        const auto owner_pid = acecode::daemon::current_pid();
        const std::string owner = "upgrade-smoke-" + std::to_string(owner_pid);
        if (!acecode::daemon::write_desktop_owner_record(run_dir,
            {owner_pid, owner, acecode::daemon::now_unix_ms()})) {
            throw std::runtime_error("cannot write isolated owner record");
        }
        SpawnRequest spawn;
        spawn.daemon_exe_path = fs::absolute(fs::u8path(argv[1])).u8string();
        spawn.cwd = fs::u8path(run_dir).parent_path().u8string();
        spawn.run_dir = run_dir;
        spawn.port = pick_free_loopback_port();
        spawn.token = make_auth_token();
        spawn.desktop_managed = true;
        spawn.guid = "upgrade-smoke-old-" + std::to_string(owner_pid);
        spawn.desktop_protocol_version = kDesktopDaemonProtocolVersion;
        spawn.desktop_owner_pid = owner_pid;
        spawn.desktop_owner_instance = owner;
        auto started = old.spawn(spawn);
        if (!started.ok) throw std::runtime_error(started.error);
        const auto before = health(spawn.port, spawn.token);

        ActivateRequest request;
        request.hash = "upgrade-runtime-smoke";
        request.cwd = spawn.cwd;
        request.run_dir = run_dir;
        request.daemon_exe_path = fs::absolute(fs::u8path(argv[2])).u8string();
        request.desktop_managed = true;
        request.desktop_owner_pid = owner_pid;
        request.desktop_owner_instance = owner;
        pool.set_keep_alive_on_exit(true);
        const auto activated = pool.activate(request, std::chrono::seconds(30));
        if (!activated.ok) throw std::runtime_error(activated.error);
        const auto after = health(activated.port, activated.token);
        if (after.value("version", "") != ACECODE_VERSION ||
            after.at("pid") == before.at("pid") || old.running()) {
            throw std::runtime_error("old backend was not replaced by the installed version");
        }
        const auto new_pid = after.at("pid").get<std::int64_t>();
        if (!pool.shutdown_all().empty() || !acecode::daemon::is_pid_alive(new_pid)) {
            throw std::runtime_error("normal exit did not preserve background continuation");
        }
        const auto reused = pool.activate(request);
        if (!reused.ok || pool.lookup(request.hash).source != DaemonConnectionSource::Attached) {
            throw std::runtime_error("same-version backend was not reused");
        }
        if (!pool.shutdown_all(DaemonShutdownReason::UpgradeRestart).empty() ||
            acecode::daemon::is_pid_alive(new_pid)) {
            throw std::runtime_error("upgrade restart left the backend running");
        }
        std::cout << nlohmann::json{{"old_version", before.at("version")},
            {"new_version", after.at("version")}, {"old_pid", before.at("pid")},
            {"new_pid", new_pid}, {"normal_exit_preserved", true},
            {"same_version_reused", true}, {"upgrade_restart_stopped", true}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        pool.shutdown_all(DaemonShutdownReason::UpgradeRestart);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
