#include "invite_link.hpp"
#include "utils/base64.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <algorithm>
#include <array>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace acecode::share {
namespace {

bool ascii_alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9');
}

bool url64(unsigned char c) { return ascii_alnum(c) || c == '-' || c == '_'; }

std::uint32_t checksum(std::string_view bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (unsigned char c : bytes) {
        crc ^= c;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::string encode64(std::string_view bytes) {
    auto result = base64_encode(bytes);
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    while (!result.empty() && result.back() == '=') result.pop_back();
    return result;
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string unescape(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() &&
            hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>((hex(text[i + 1]) << 4) | hex(text[i + 2])));
            i += 2;
        } else out.push_back(text[i]);
    }
    return out;
}

std::string validate(const InviteLink& invite) {
    if (invite.owner.empty() || invite.owner.size() > 256 ||
        invite.title.size() > 512 || invite.addresses.empty() ||
        invite.addresses.size() > 32 || invite.port < 1 || invite.port > 65535 ||
        !valid_share_credential(invite.credential) || invite.expires_at <= 0 ||
        invite.expires_at > 9007199254740991LL) return "invalid_fields";
    for (const auto& address : invite.addresses)
        if (!valid_invite_address(address)) return "invalid_address";
    return {};
}
} // namespace

bool valid_share_credential(std::string_view credential) {
    if (credential.size() != 49 || credential[16] != '.') return false;
    for (std::size_t i = 0; i < 16; ++i)
        if (!((credential[i] >= '0' && credential[i] <= '9') ||
              (credential[i] >= 'a' && credential[i] <= 'f'))) return false;
    return std::all_of(credential.begin() + 17, credential.end(), url64);
}

bool valid_invite_address(std::string_view address) {
    if (address.empty() || address.size() > 253) return false;
    std::string value(address);
    std::array<unsigned char, 16> parsed{};
    if (value.find(':') != std::string::npos) {
        const auto zone = value.find('%');
        if (zone != std::string::npos) {
            const auto suffix = value.substr(zone + 1);
            if (suffix.empty() || !std::all_of(suffix.begin(), suffix.end(),
                [](unsigned char c) { return ascii_alnum(c) || c == '_' || c == '-'; })) return false;
            value.resize(zone);
        }
        return inet_pton(AF_INET6, value.c_str(), parsed.data()) == 1;
    }
    if (std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || c == '.';
        })) return inet_pton(AF_INET, value.c_str(), parsed.data()) == 1;
    std::size_t start = 0;
    while (start < value.size()) {
        auto end = value.find('.', start);
        if (end == std::string::npos) end = value.size();
        if (end == start || end - start > 63 || !ascii_alnum(value[start]) ||
            !ascii_alnum(value[end - 1])) return false;
        for (auto i = start; i < end; ++i)
            if (!ascii_alnum(value[i]) && value[i] != '-') return false;
        start = end + 1;
    }
    return value.back() != '.';
}

std::string extract_invite_link(std::string_view text) {
    // Bound clipboard/chat input before allocating or decoding it.
    const auto decoded = unescape(text.substr(0, 8192));
    const auto start = decoded.find(kInvitePrefix);
    if (start == std::string::npos) return {};
    auto end = start + kInvitePrefix.size();
    while (end < decoded.size() && url64(decoded[end])) ++end;
    return decoded.substr(start, end - start);
}

InviteDecodeResult decode_invite_link(std::string_view text, std::int64_t now) {
    InviteDecodeResult result;
    const auto link = extract_invite_link(text);
    if (link.empty()) { result.error = "not_invite"; return result; }
    if (link.size() > kMaxInviteLength) { result.error = "too_long"; return result; }
    const auto encoded = link.substr(kInvitePrefix.size());
    const auto raw = base64_decode(encoded);
    if (!raw || raw->size() < 5 || encode64(*raw) != encoded) {
        result.error = "checksum"; return result;
    }
    const auto body = raw->substr(0, raw->size() - 4);
    std::uint32_t expected = 0;
    for (auto i = raw->size() - 4; i < raw->size(); ++i)
        expected = (expected << 8) | static_cast<unsigned char>((*raw)[i]);
    if (checksum(body) != expected) { result.error = "checksum"; return result; }
    try {
        const auto json = nlohmann::json::parse(body);
        if (!json.is_object() || !json.contains("v") || !json["v"].is_number_integer() ||
            !json.contains("n") || !json["n"].is_string() ||
            !json.contains("t") || !json["t"].is_string() ||
            !json.contains("a") || !json["a"].is_array() ||
            !json.contains("p") || !json["p"].is_number_integer() ||
            !json.contains("c") || !json["c"].is_string() ||
            !json.contains("e") || !json["e"].is_number_integer()) {
            result.error = "invalid_fields"; return result;
        }
        if (json["v"] != 1) { result.error = "unsupported_version"; return result; }
        if (json["p"] < 1 || json["p"] > 65535 || json["e"] <= 0 ||
            json["e"] > 9007199254740991LL) { result.error = "invalid_fields"; return result; }
        result.value = {json["n"].get<std::string>(), json["t"].get<std::string>(),
            json["a"].get<std::vector<std::string>>(), json["p"].get<int>(),
            json["c"].get<std::string>(), json["e"].get<std::int64_t>()};
        result.error = validate(result.value);
        if (result.error.empty() && result.value.expires_at <= now) result.error = "expired";
    } catch (const nlohmann::json::exception&) { result.error = "invalid_fields"; }
    return result;
}

std::string encode_invite_link(const InviteLink& invite) {
    const auto error = validate(invite);
    if (!error.empty()) throw std::invalid_argument(error);
    nlohmann::json json{{"v", 1}, {"n", invite.owner}, {"t", invite.title},
        {"a", invite.addresses}, {"p", invite.port}, {"c", invite.credential},
        {"e", invite.expires_at}};
    auto bytes = json.dump();
    const auto crc = checksum(bytes);
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<char>(crc >> shift));
    auto link = std::string(kInvitePrefix) + encode64(bytes);
    if (link.size() > kMaxInviteLength) throw std::invalid_argument("too_long");
    return link;
}

} // namespace acecode::share
