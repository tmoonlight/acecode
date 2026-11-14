#include "proxy_resolver.hpp"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <algorithm>
#include <array>

namespace acecode::network {
bool is_direct_local_host(const std::string& host) {
    if (host.empty()) return false;
    auto address = host;
    if (address.front() == '[' && address.back() == ']') address = address.substr(1, address.size() - 2);
    if (const auto zone = address.find('%'); zone != std::string::npos) address.resize(zone);
    std::array<unsigned char, 16> bytes{};
    if (inet_pton(AF_INET, address.c_str(), bytes.data()) == 1) {
        return bytes[0] == 10 || bytes[0] == 127 ||
            (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
            (bytes[0] == 192 && bytes[1] == 168) || (bytes[0] == 169 && bytes[1] == 254);
    }
    if (inet_pton(AF_INET6, address.c_str(), bytes.data()) == 1) {
        if ((bytes[0] & 0xfe) == 0xfc || (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80)) return true;
        if (std::all_of(bytes.begin(), bytes.begin() + 15, [](auto b) { return b == 0; }) && bytes[15] == 1) return true;
        if (std::all_of(bytes.begin(), bytes.begin() + 10, [](auto b) { return b == 0; }) && bytes[10] == 255 && bytes[11] == 255) {
            return is_direct_local_host(std::to_string(bytes[12]) + "." + std::to_string(bytes[13]) + "." +
                std::to_string(bytes[14]) + "." + std::to_string(bytes[15]));
        }
        return false;
    }
    return address.find('.') == std::string::npos && std::all_of(address.begin(), address.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    });
}
} // namespace acecode::network
