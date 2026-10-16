#pragma once

// Framing shared by the daemon and the acecode-ios-sim helper. Header-only so
// the helper executable can use it without linking the adapters library.
//
// Frame := u32le body_length | u32le json_length | json | binary
// body_length counts everything after the first field. Requests, responses,
// events and video frames all use the same frame; a JSON object identifies the
// kind ("id" for requests/responses, "event" for unsolicited messages) and the
// optional binary tail carries image bytes.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace acecode::ios_simulator::wire {

inline constexpr int kProtocolVersion = 1;
inline constexpr std::size_t kHeaderBytes = 8;
inline constexpr std::size_t kMaxBodyBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaxJsonBytes = 4u * 1024u * 1024u;

struct Frame {
    nlohmann::json header;
    std::string binary;
};

inline void put_u32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<char>((value >> shift) & 0xffu));
}

inline std::uint32_t get_u32(const char* bytes) {
    std::uint32_t value = 0;
    for (int index = 3; index >= 0; --index)
        value = (value << 8) | static_cast<unsigned char>(bytes[index]);
    return value;
}

// Throws std::length_error when the frame exceeds the protocol limits.
inline std::string encode(const nlohmann::json& header, std::string_view binary = {}) {
    const auto json = header.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (json.size() > kMaxJsonBytes || json.size() + binary.size() + 4 > kMaxBodyBytes)
        throw std::length_error("ios simulator frame exceeds protocol limits");
    std::string out;
    out.reserve(kHeaderBytes + json.size() + binary.size());
    put_u32(out, static_cast<std::uint32_t>(4 + json.size() + binary.size()));
    put_u32(out, static_cast<std::uint32_t>(json.size()));
    out.append(json);
    out.append(binary.data(), binary.size());
    return out;
}

// Incremental decoder. Feed bytes as they arrive and pop complete frames.
// A protocol violation latches error() and no further frames are produced.
class Decoder {
public:
    void feed(const char* bytes, std::size_t size) {
        if (!error_.empty()) return;
        buffer_.append(bytes, size);
    }

    std::optional<Frame> next() {
        if (!error_.empty()) return std::nullopt;
        if (buffer_.size() - offset_ < kHeaderBytes) { compact(); return std::nullopt; }
        const char* head = buffer_.data() + offset_;
        const auto body = get_u32(head);
        const auto json_length = get_u32(head + 4);
        if (body < 4 || body > kMaxBodyBytes || json_length > body - 4 || json_length > kMaxJsonBytes) {
            error_ = "invalid frame header";
            return std::nullopt;
        }
        if (buffer_.size() - offset_ < 4 + static_cast<std::size_t>(body)) {
            compact();
            return std::nullopt;
        }
        Frame frame;
        frame.header = nlohmann::json::parse(head + 8, head + 8 + json_length, nullptr, false);
        if (frame.header.is_discarded() || !frame.header.is_object()) {
            error_ = "invalid frame json";
            return std::nullopt;
        }
        frame.binary.assign(head + 8 + json_length, body - 4 - json_length);
        offset_ += 4 + static_cast<std::size_t>(body);
        return frame;
    }

    const std::string& error() const { return error_; }
    std::size_t buffered() const { return buffer_.size() - offset_; }

private:
    void compact() {
        if (offset_ == 0) return;
        buffer_.erase(0, offset_);
        offset_ = 0;
    }

    std::string buffer_;
    std::size_t offset_ = 0;
    std::string error_;
};

} // namespace acecode::ios_simulator::wire
