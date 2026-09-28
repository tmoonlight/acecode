#include "native_backend.hpp"

#include <iostream>
#include <cwchar>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#ifdef __APPLE__
int computer_use_protocol_main() {
#else
int main() {
#endif
#ifdef _WIN32
    // Keep the protocol byte-oriented even when a Windows console is attached.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    wchar_t desktop[256] = {};
    DWORD needed = 0;
    if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                                  desktop, sizeof(desktop), &needed)) {
        std::wcscpy(desktop, L"Default");
    }
    const std::wstring name = L"Local\\ACECode.ComputerUse." + std::to_wstring(session) + L"." + desktop;
    HANDLE lease = CreateMutexW(nullptr, FALSE, name.c_str());
    DWORD lease_result = lease ? WaitForSingleObject(lease, 0) : WAIT_FAILED;
    const bool owned = lease_result == WAIT_OBJECT_0 || lease_result == WAIT_ABANDONED;
    if (!owned) {
        std::string ignored;
        std::getline(std::cin, ignored);
        std::cout << nlohmann::json({{"protocol_version", 1}, {"success", false}, {"error", "desktop_busy"},
            {"output", "Another ACECode session owns this interactive desktop. Retry after it finishes."}}).dump() << '\n' << std::flush;
        if (lease) CloseHandle(lease);
        return 2;
    }
#endif
    int exit_code = 0;
    try {
        acecode::computer_use::NativeBackend backend;
        std::string line;
        // A finite per-request limit also bounds Unicode typing and JSON parsing.
        constexpr std::size_t max_request = 256 * 1024;
        while (std::cin.good()) {
            line.clear();
            char c = 0;
            while (std::cin.get(c) && c != '\n') {
                if (line.size() >= max_request) {
                    std::cout << "{\"protocol_version\":1,\"success\":false,\"error\":\"request_too_large\",\"output\":\"Computer Use request exceeds 256 KiB.\"}\n" << std::flush;
                    exit_code = 3;
                    break;
                }
                line.push_back(c);
            }
            if (exit_code || (line.empty() && !std::cin.good())) break;
            if (line.empty()) continue;
            auto request = nlohmann::json::parse(line, nullptr, false);
            auto response = request.is_discarded()
                ? nlohmann::json({{"success", false}, {"error", "invalid_json"}, {"output", "Expected one JSON object per line."}})
                : (!request.is_object() || !request.contains("protocol_version") || request["protocol_version"] != 1)
                    ? nlohmann::json({{"success", false}, {"error", "protocol_mismatch"}, {"output", "Computer Use protocol version 1 is required."}})
                    : backend.dispatch(request);
            response["protocol_version"] = 1;
            std::cout << response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n' << std::flush;
        }
    } catch (const std::exception& error) {
        std::cout << nlohmann::json({{"protocol_version", 1}, {"success", false}, {"error", "native_initialization_failed"}, {"output", error.what()}}).dump() << '\n' << std::flush;
        exit_code = 1;
    }
#ifdef _WIN32
    ReleaseMutex(lease);
    CloseHandle(lease);
#endif
    return exit_code;
}
