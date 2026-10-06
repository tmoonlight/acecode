#include <gtest/gtest.h>

#include "im/feishu/feishu_frame.hpp"

#include <initializer_list>

// im/feishu/feishu_frame:飞书长连接 pbbp2 帧的手写 protobuf 编解码。
// 期望字节全部由真实 protobuf 运行时按同一 schema 生成(service_id = 1234),
// 与飞书协议调研文档 §5.3 的测试向量一致;编码必须逐字节相同。

namespace acecode::im::feishu {
namespace {

// "08 00 10 …" → 原始字节。
std::string hex(const char* text) {
    std::string out;
    int high = -1;
    for (const char* p = text; *p; ++p) {
        const char c = *p;
        int value = -1;
        if (c >= '0' && c <= '9') value = c - '0';
        else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
        if (value < 0) continue;
        if (high < 0) {
            high = value;
        } else {
            out.push_back(static_cast<char>(high * 16 + value));
            high = -1;
        }
    }
    return out;
}

std::string bytes(std::initializer_list<int> values) {
    std::string out;
    for (const int v : values) out.push_back(static_cast<char>(v));
    return out;
}

const char* kPing = "08 00 10 00 18 d2 09 20 00 2a 0c 0a 04 74 79 70 65 12 04 70 69 6e 67";
const char* kData =
    "08 00 10 00 18 d2 09 20 01 2a 0d 0a 04 74 79 70 65 12 05 65 76 65 6e 74 2a 10 0a 0a 6d 65 73 73 61 67 65 5f "
    "69 64 12 02 6d 31 2a 08 0a 03 73 75 6d 12 01 31 2a 08 0a 03 73 65 71 12 01 30 2a 0e 0a 08 74 72 61 63 65 5f 69 "
    "64 12 02 74 31 42 10 7b 22 73 63 68 65 6d 61 22 3a 22 32 2e 30 22 7d";
const char* kAck =
    "08 00 10 00 18 d2 09 20 01 2a 0d 0a 04 74 79 70 65 12 05 65 76 65 6e 74 2a 10 0a 0a 6d 65 73 73 61 67 65 5f "
    "69 64 12 02 6d 31 2a 08 0a 03 73 75 6d 12 01 31 2a 08 0a 03 73 65 71 12 01 30 2a 0e 0a 08 74 72 61 63 65 5f 69 "
    "64 12 02 74 31 2a 0b 0a 06 62 69 7a 5f 72 74 12 01 33 42 0c 7b 22 63 6f 64 65 22 3a 32 30 30 7d";
// SeqID=300, LogID=7000000000, payload_encoding 存在但为空, payload_type="json", payload="P", LogIDNew="L9"。
const char* kFull =
    "08 ac 02 10 80 8c ee 89 1a 18 d2 09 20 01 2a 0d 0a 04 74 79 70 65 12 05 65 76 65 6e 74 32 00 3a 04 6a 73 6f 6e "
    "42 01 50 4a 02 4c 39";
// service = -5:int32 负数按 64 位符号扩展,是 10 字节 varint。
const char* kNegative = "08 01 10 02 18 fb ff ff ff ff ff ff ff ff 01 20 01";

// 场景:客户端心跳帧(service_id 1234)。
// 期望:与官方运行时的字节完全一致 —— 字段 1–4 即使为 0 也写出,只有一个 type=ping 头,没有 payload。
TEST(FeishuFrame, EncodesPingExactly) {
    EXPECT_EQ(encode_frame(make_ping_frame(1234)), hex(kPing));
}

// 场景:构造一个带 5 个头与 JSON payload 的 DATA 帧。
// 期望:逐字节等于测试向量;header 保持插入顺序,可选字段 6/7/9 未设置时不写出。
TEST(FeishuFrame, EncodesDataFrameExactly) {
    Frame frame;
    frame.service = 1234;
    frame.method = kMethodData;
    frame.headers = {{"type", "event"}, {"message_id", "m1"}, {"sum", "1"}, {"seq", "0"}, {"trace_id", "t1"}};
    frame.payload = R"({"schema":"2.0"})";
    EXPECT_EQ(encode_frame(frame), hex(kData));
}

// 场景:解码服务端事件帧,再按规则生成 ACK(业务耗时 3 ms)。
// 期望:ACK 原样回显原帧的全部字段与头(顺序不变),末尾追加 biz_rt=3,payload 换成 {"code":200};
// 逐字节等于测试向量。回归意义:ACK 字段漏回显会让平台认为没送达,15 秒后重投。
TEST(FeishuFrame, AckEchoesReceivedFrameAndAppendsBizRt) {
    Frame frame;
    std::string error;
    ASSERT_TRUE(decode_frame(hex(kData), &frame, &error)) << error;
    EXPECT_EQ(frame.service, 1234);
    EXPECT_EQ(frame.method, kMethodData);
    EXPECT_EQ(frame.header("message_id"), "m1");
    ASSERT_TRUE(frame.payload.has_value());
    EXPECT_EQ(*frame.payload, R"({"schema":"2.0"})");
    EXPECT_EQ(encode_frame(make_ack_frame(frame, 3, R"({"code":200})")), hex(kAck));
}

// 场景:服务端帧带了全部可选字段,其中 payload_encoding 存在但为空。
// 期望:解码出全部字段(大数值 LogID 正确);重新编码与原字节完全一致 —— “存在但为空”的字段
// 回 ACK 时也要回显(Go SDK 总是写出)。
TEST(FeishuFrame, OptionalFieldPresenceRoundTrips) {
    Frame frame;
    ASSERT_TRUE(decode_frame(hex(kFull), &frame, nullptr));
    EXPECT_EQ(frame.seq_id, 300u);
    EXPECT_EQ(frame.log_id, 7000000000ull);
    ASSERT_TRUE(frame.payload_encoding.has_value());
    EXPECT_TRUE(frame.payload_encoding->empty());
    EXPECT_EQ(frame.payload_type.value_or(""), "json");
    EXPECT_EQ(frame.payload.value_or(""), "P");
    EXPECT_EQ(frame.log_id_new.value_or(""), "L9");
    EXPECT_EQ(encode_frame(frame), hex(kFull));
}

// 场景:service 为负数(协议里不会出现,但要按 protobuf 规则处理)。
// 期望:编码为 10 字节 varint;解码回原值。
TEST(FeishuFrame, NegativeInt32UsesTenByteVarint) {
    Frame frame;
    frame.seq_id = 1;
    frame.log_id = 2;
    frame.service = -5;
    frame.method = kMethodData;
    EXPECT_EQ(encode_frame(frame), hex(kNegative));
    Frame decoded;
    ASSERT_TRUE(decode_frame(hex(kNegative), &decoded, nullptr));
    EXPECT_EQ(decoded.service, -5);
}

// 场景:字段乱序到达,夹带四种 wire type 的未知字段,header 里也有未知子字段,且有重复头。
// 期望:全部跳过未知字段,已知字段正确解析;重复头按出现顺序保留。
TEST(FeishuFrame, DecoderSkipsUnknownFieldsInAnyOrder) {
    std::string data;
    data += bytes({0x42, 0x02}) + "{}";                                    // payload 先到
    data += bytes({0x78, 0x96, 0x01});                                    // 字段 15 varint
    data += bytes({0x81, 0x01, 1, 2, 3, 4, 5, 6, 7, 8});                  // 字段 16 fixed64
    data += bytes({0x8a, 0x01, 0x03}) + "abc";                            // 字段 17 length-delimited
    data += bytes({0x95, 0x01, 1, 2, 3, 4});                              // 字段 18 fixed32
    data += bytes({0x2a, 0x08, 0x0a, 0x01, 'k', 0x12, 0x01, 'v', 0x18, 0x05});  // header + 未知子字段
    data += bytes({0x2a, 0x06, 0x0a, 0x01, 'k', 0x12, 0x01, 'w'});        // 重复的 k
    data += bytes({0x20, 0x01, 0x18, 0xd2, 0x09});                        // method 在 service 之前
    Frame frame;
    std::string error;
    ASSERT_TRUE(decode_frame(data, &frame, &error)) << error;
    EXPECT_EQ(frame.service, 1234);
    EXPECT_EQ(frame.method, kMethodData);
    ASSERT_EQ(frame.headers.size(), 2u);
    EXPECT_EQ(frame.headers[0].second, "v");
    EXPECT_EQ(frame.headers[1].second, "w");
    EXPECT_EQ(frame.header("k"), "v");
    EXPECT_EQ(frame.payload.value_or(""), "{}");
    EXPECT_FALSE(frame.payload_type.has_value());
}

// 场景:损坏的帧 —— 长度越界、varint 超过 10 字节、group wire type、已知字段的 wire type 不对、截断的 tag。
// 期望:全部解码失败并给出原因,不越界读、不崩溃。
TEST(FeishuFrame, DecoderRejectsMalformedInput) {
    Frame frame;
    std::string error;
    EXPECT_FALSE(decode_frame(bytes({0x42, 0x10, 'a'}), &frame, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(decode_frame(bytes({0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}),
                              &frame, nullptr));
    EXPECT_FALSE(decode_frame(bytes({0x7b, 0x00}), &frame, nullptr));        // 字段 15 group 起始
    EXPECT_FALSE(decode_frame(bytes({0x1a, 0x01, 0x00}), &frame, nullptr));  // service 用了 length-delimited
    EXPECT_FALSE(decode_frame(bytes({0x80}), &frame, nullptr));
    EXPECT_TRUE(decode_frame(std::string(), &frame, nullptr));  // 空帧合法(全部取默认值)
}

} // namespace
} // namespace acecode::im::feishu
