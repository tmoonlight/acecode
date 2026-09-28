#pragma once

#include "bridge.hpp"
#include <atomic>
#include <chrono>
#include <functional>

namespace acecode::channels {

enum class SetupPhase { Preparing, Installing, Connecting, Pairing, Complete, Failed, Cancelled };
struct SetupUpdate {
    SetupPhase phase = SetupPhase::Preparing;
    std::string detail;
    std::string qr_text;
    std::string account;
};
using SetupProgress = std::function<void(const SetupUpdate&)>;

std::vector<std::string> setup_contacts(const std::string& phone_numbers);
bool supported_node_version(const std::string& version);
void install_whatsapp_dependencies(const std::filesystem::path& directory,
                                  const std::atomic<bool>& cancelled,
                                  const SetupProgress& progress);

struct SetupDependencies {
    std::function<void()> begin;
    std::function<void(const std::atomic<bool>&, const SetupProgress&)> prepare;
    std::function<void()> connect;
    std::function<Json()> status;
    std::function<void(const std::string&, const std::vector<std::string>&)> save;
    std::function<void()> close;
    std::function<void()> wait;
    std::chrono::milliseconds pairing_timeout{std::chrono::minutes(5)};
};
SetupDependencies default_setup_dependencies(std::filesystem::path directory = {},
    std::function<platform::SpawnOptions(const std::filesystem::path&)> spawn = {});
// Runs off the UI thread. Existing configuration survives cancellation/failure.
void run_setup(const std::vector<std::string>& contacts, SetupDependencies deps,
               const std::atomic<bool>& cancelled, const SetupProgress& progress);

} // namespace acecode::channels
