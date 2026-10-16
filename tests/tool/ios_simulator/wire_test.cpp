#include <gtest/gtest.h>

#include "tool/ios_simulator/wire.hpp"

namespace wire = acecode::ios_simulator::wire;

TEST(IosSimulatorWire, RoundTripsJsonAndBinaryInPieces) {
    const std::string binary("\x00\x01\xff jpeg", 8);
    const auto frame = wire::encode({{"id", 7}, {"ok", true}}, binary);
    wire::Decoder decoder;
    // Feed one byte at a time: frames must only appear once complete.
    for (std::size_t i = 0; i + 1 < frame.size(); ++i) {
        decoder.feed(frame.data() + i, 1);
        EXPECT_FALSE(decoder.next().has_value());
    }
    decoder.feed(frame.data() + frame.size() - 1, 1);
    auto decoded = decoder.next();
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->header["id"], 7);
    EXPECT_EQ(decoded->binary, binary);
    EXPECT_FALSE(decoder.next().has_value());
    EXPECT_EQ(decoder.buffered(), 0u);
}

TEST(IosSimulatorWire, DecodesBackToBackFrames) {
    const auto stream = wire::encode({{"event", "frame"}, {"seq", 1}}, "aa") +
                        wire::encode({{"event", "frame"}, {"seq", 2}});
    wire::Decoder decoder;
    decoder.feed(stream.data(), stream.size());
    auto first = decoder.next();
    auto second = decoder.next();
    ASSERT_TRUE(first && second);
    EXPECT_EQ(first->header["seq"], 1);
    EXPECT_EQ(first->binary, "aa");
    EXPECT_EQ(second->header["seq"], 2);
    EXPECT_TRUE(second->binary.empty());
}

TEST(IosSimulatorWire, RejectsCorruptHeadersAndJson) {
    wire::Decoder bad_length;
    std::string header;
    wire::put_u32(header, 2);  // body shorter than the json length field
    wire::put_u32(header, 0);
    bad_length.feed(header.data(), header.size());
    EXPECT_FALSE(bad_length.next().has_value());
    EXPECT_FALSE(bad_length.error().empty());

    wire::Decoder bad_json;
    std::string frame;
    wire::put_u32(frame, 4 + 3);
    wire::put_u32(frame, 3);
    frame += "{{{";
    bad_json.feed(frame.data(), frame.size());
    EXPECT_FALSE(bad_json.next().has_value());
    EXPECT_FALSE(bad_json.error().empty());

    wire::Decoder oversized;
    std::string huge;
    wire::put_u32(huge, static_cast<std::uint32_t>(wire::kMaxBodyBytes + 1));
    wire::put_u32(huge, 0);
    oversized.feed(huge.data(), huge.size());
    EXPECT_FALSE(oversized.next().has_value());
    EXPECT_FALSE(oversized.error().empty());
}
