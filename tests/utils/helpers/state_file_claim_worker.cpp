#include "utils/state_file.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    if (argc < 6) {
        std::cerr
            << "usage: state_file_claim_worker "
            << "<state-path> <key> <ready-path> <go-path> <result-path>\n";
        return 99;
    }

    const std::string state_path = argv[1];
    const std::string key = argv[2];
    const fs::path ready_path = fs::path(argv[3]);
    const fs::path go_path = fs::path(argv[4]);
    const fs::path result_path = fs::path(argv[5]);

    acecode::set_state_file_path_for_test(state_path);
    {
        std::ofstream ready(ready_path, std::ios::binary | std::ios::trunc);
        if (!ready) return 98;
        ready << "ready\n";
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!fs::exists(go_path)) {
        if (std::chrono::steady_clock::now() >= deadline) return 97;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto claim = acecode::try_claim_state_flag(key);
    std::ofstream result(result_path, std::ios::binary | std::ios::trunc);
    if (!result) return 96;
    result << (claim.claimed ? 1 : 0) << " "
           << (claim.persisted ? 1 : 0) << "\n";
    return 0;
}
