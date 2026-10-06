#include "im/feishu/feishu_frame.hpp"

namespace acecode::im::feishu {
namespace {

constexpr int kWireVarint = 0;
constexpr int kWire64 = 1;
constexpr int kWireBytes = 2;
constexpr int kWire32 = 5;

void put_varint(std::string& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

void put_tag(std::string& out, int field, int wire) {
    put_varint(out, (static_cast<std::uint64_t>(field) << 3) | static_cast<std::uint64_t>(wire));
}

void put_bytes(std::string& out, int field, std::string_view value) {
    put_tag(out, field, kWireBytes);
    put_varint(out, value.size());
    out.append(value.data(), value.size());
}

// int32 按 protobuf 规则先符号扩展到 64 位,负数因此是 10 字节 varint。
std::uint64_t int32_wire(std::int32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    bool done() const { return pos_ >= data_.size(); }

    bool varint(std::uint64_t* value) {
        std::uint64_t result = 0;
        for (int i = 0; i < 10; ++i) {
            if (pos_ >= data_.size()) return false;
            const auto byte = static_cast<std::uint8_t>(data_[pos_++]);
            result |= static_cast<std::uint64_t>(byte & 0x7F) << (7 * i);
            if ((byte & 0x80) == 0) {
                *value = result;
                return true;
            }
        }
        return false;  // 超过 10 字节
    }

    bool bytes(std::string_view* value) {
        std::uint64_t length = 0;
        if (!varint(&length)) return false;
        if (length > data_.size() - pos_) return false;
        *value = data_.substr(pos_, static_cast<std::size_t>(length));
        pos_ += static_cast<std::size_t>(length);
        return true;
    }

    bool skip(int wire) {
        switch (wire) {
            case kWireVarint: {
                std::uint64_t ignored = 0;
                return varint(&ignored);
            }
            case kWire64:
                if (data_.size() - pos_ < 8) return false;
                pos_ += 8;
                return true;
            case kWireBytes: {
                std::string_view ignored;
                return bytes(&ignored);
            }
            case kWire32:
                if (data_.size() - pos_ < 4) return false;
                pos_ += 4;
                return true;
            default:
                return false;  // group 或非法 wire type
        }
    }

private:
    std::string_view data_;
    std::size_t pos_ = 0;
};

bool fail(std::string* error, const char* reason) {
    if (error) *error = reason;
    return false;
}

bool decode_header(std::string_view bytes, std::pair<std::string, std::string>* out) {
    Reader reader(bytes);
    while (!reader.done()) {
        std::uint64_t tag = 0;
        if (!reader.varint(&tag)) return false;
        const auto field = tag >> 3;
        const int wire = static_cast<int>(tag & 0x07);
        if ((field == 1 || field == 2) && wire == kWireBytes) {
            std::string_view value;
            if (!reader.bytes(&value)) return false;
            (field == 1 ? out->first : out->second) = std::string(value);
        } else if (field == 0 || !reader.skip(wire)) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string Frame::header(std::string_view key) const {
    for (const auto& [name, value] : headers) {
        if (name == key) return value;
    }
    return {};
}

bool Frame::has_header(std::string_view key) const {
    for (const auto& item : headers) {
        if (item.first == key) return true;
    }
    return false;
}

std::string encode_frame(const Frame& frame) {
    std::string out;
    put_tag(out, 1, kWireVarint);
    put_varint(out, frame.seq_id);
    put_tag(out, 2, kWireVarint);
    put_varint(out, frame.log_id);
    put_tag(out, 3, kWireVarint);
    put_varint(out, int32_wire(frame.service));
    put_tag(out, 4, kWireVarint);
    put_varint(out, int32_wire(frame.method));
    for (const auto& [key, value] : frame.headers) {
        std::string header;
        put_bytes(header, 1, key);
        put_bytes(header, 2, value);
        put_bytes(out, 5, header);
    }
    if (frame.payload_encoding) put_bytes(out, 6, *frame.payload_encoding);
    if (frame.payload_type) put_bytes(out, 7, *frame.payload_type);
    if (frame.payload) put_bytes(out, 8, *frame.payload);
    if (frame.log_id_new) put_bytes(out, 9, *frame.log_id_new);
    return out;
}

bool decode_frame(std::string_view bytes, Frame* out, std::string* error) {
    Frame frame;
    Reader reader(bytes);
    while (!reader.done()) {
        std::uint64_t tag = 0;
        if (!reader.varint(&tag)) return fail(error, "truncated tag");
        const auto field = tag >> 3;
        const int wire = static_cast<int>(tag & 0x07);
        if (field == 0) return fail(error, "invalid field number");
        if (field >= 1 && field <= 4) {
            if (wire != kWireVarint) return fail(error, "unexpected wire type for scalar field");
            std::uint64_t value = 0;
            if (!reader.varint(&value)) return fail(error, "truncated varint");
            if (field == 1) frame.seq_id = value;
            else if (field == 2) frame.log_id = value;
            // int32 取低 32 位(负数的 10 字节编码也落在这里)。
            else if (field == 3) frame.service = static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
            else frame.method = static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
            continue;
        }
        if (field >= 5 && field <= 9) {
            if (wire != kWireBytes) return fail(error, "unexpected wire type for bytes field");
            std::string_view value;
            if (!reader.bytes(&value)) return fail(error, "truncated length-delimited field");
            switch (field) {
                case 5: {
                    std::pair<std::string, std::string> header;
                    if (!decode_header(value, &header)) return fail(error, "malformed header");
                    frame.headers.push_back(std::move(header));
                    break;
                }
                case 6: frame.payload_encoding = std::string(value); break;
                case 7: frame.payload_type = std::string(value); break;
                case 8: frame.payload = std::string(value); break;
                default: frame.log_id_new = std::string(value); break;
            }
            continue;
        }
        if (!reader.skip(wire)) return fail(error, "cannot skip unknown field");
    }
    *out = std::move(frame);
    return true;
}

Frame make_ping_frame(std::int32_t service) {
    Frame frame;
    frame.service = service;
    frame.method = kMethodControl;
    frame.headers.emplace_back("type", "ping");
    return frame;
}

Frame make_ack_frame(const Frame& received, std::int64_t biz_rt_ms, const std::string& response_json) {
    Frame ack = received;
    ack.headers.emplace_back("biz_rt", std::to_string(biz_rt_ms < 0 ? 0 : biz_rt_ms));
    ack.payload = response_json;
    return ack;
}

} // namespace acecode::im::feishu
