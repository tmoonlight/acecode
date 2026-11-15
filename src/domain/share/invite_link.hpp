#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace acecode::share {

inline constexpr std::size_t kMaxInviteLength = 1024;
inline constexpr std::string_view kInvitePrefix = "acecode://connect/";

struct InviteLink {
    std::string owner;
    std::string title;
    std::vector<std::string> addresses;
    int port = 0;
    std::string credential;
    std::int64_t expires_at = 0;
};

struct InviteDecodeResult {
    InviteLink value;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};

bool valid_invite_address(std::string_view address);
bool valid_share_credential(std::string_view credential);
std::string extract_invite_link(std::string_view text);
InviteDecodeResult decode_invite_link(std::string_view text, std::int64_t now);
// Throws invalid_argument for invalid payloads or links exceeding the limit.
std::string encode_invite_link(const InviteLink& invite);

} // namespace acecode::share
