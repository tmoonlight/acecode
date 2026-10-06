#pragma once

// 飞书 / Lark 长连接的二进制帧(protobuf proto2,package pbbp2)手写编解码,纯逻辑。
//
//   message Header { required string key = 1; required string value = 2; }
//   message Frame {
//     required uint64 SeqID = 1;  required uint64 LogID = 2;
//     required int32  service = 3; required int32 method = 4;   // 0 = CONTROL, 1 = DATA
//     repeated Header headers = 5;
//     optional string payload_encoding = 6; optional string payload_type = 7;
//     optional bytes  payload = 8;          optional string LogIDNew = 9;
//   }
//
// 编码:字段 1–4 即使为 0 也必须写出;可选字段只在“存在”时写出(存在但为空也写),
// 这样回 ACK 时能原样回显服务端帧里出现过的字段。字段按编号升序,headers 保持顺序。
// 解码:接受任意字段顺序与重复 header,跳过未知字段(wire type 0/1/2/5);
// varint 最长 10 字节;group(wire type 3/4)与越界长度视为损坏。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace acecode::im::feishu {

inline constexpr std::int32_t kMethodControl = 0;
inline constexpr std::int32_t kMethodData = 1;

struct Frame {
    std::uint64_t seq_id = 0;
    std::uint64_t log_id = 0;
    std::int32_t service = 0;
    std::int32_t method = 0;
    std::vector<std::pair<std::string, std::string>> headers;  // 保持原始顺序
    std::optional<std::string> payload_encoding;
    std::optional<std::string> payload_type;
    std::optional<std::string> payload;
    std::optional<std::string> log_id_new;

    // 第一个同名 header 的值;不存在时返回空串。
    std::string header(std::string_view key) const;
    bool has_header(std::string_view key) const;
};

std::string encode_frame(const Frame& frame);

// 失败时返回 false,error 为英文原因(只用于日志)。
bool decode_frame(std::string_view bytes, Frame* out, std::string* error);

// 客户端心跳:Frame{SeqID:0, LogID:0, service, method:CONTROL, headers:[type=ping]},无 payload。
Frame make_ping_frame(std::int32_t service);

// 事件帧的 ACK:原样回显收到的帧(SeqID/LogID/service/method/payload_encoding/payload_type/
// LogIDNew 与全部 header),末尾追加 biz_rt,payload 换成应答 JSON(如 {"code":200})。
Frame make_ack_frame(const Frame& received, std::int64_t biz_rt_ms, const std::string& response_json);

} // namespace acecode::im::feishu
