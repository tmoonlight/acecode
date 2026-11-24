#include <gtest/gtest.h>

#include "utils/url_encoding.hpp"

#include <string>

namespace {

using acecode::utils::percent_decode_path_segment;
using acecode::utils::percent_encode_query_component;

TEST(UrlEncoding, DecodesSlashColonSpacePlusAndUtf8InPathSegment) {
    EXPECT_EQ(percent_decode_path_segment("gpt-4o"), "gpt-4o");
    EXPECT_EQ(percent_decode_path_segment(""), "");
    EXPECT_EQ(percent_decode_path_segment("poolside%2Flaguna-xs.2%3Afree"),
              "poolside/laguna-xs.2:free");
    EXPECT_EQ(percent_decode_path_segment("open%2frouter"), "open/router");
    EXPECT_EQ(percent_decode_path_segment("a+b%20c%2Bd%25e"), "a+b c+d%e");
    EXPECT_EQ(percent_decode_path_segment("%E6%A8%A1%E5%9E%8B%20v1"),
              std::string(u8"模型 v1"));
}

TEST(UrlEncoding, RoundTripsPercentEncodedSpecialCharacters) {
    const std::string original =
        std::string("poolside/laguna-xs.2:free ") + u8"中文" + "+100%#?@";
    const std::string encoded = percent_encode_query_component(original);
    EXPECT_EQ(percent_decode_path_segment(encoded), original);
}

TEST(UrlEncoding, RejectsMalformedOrNulPercentSequences) {
    EXPECT_FALSE(percent_decode_path_segment("bad%").has_value());
    EXPECT_FALSE(percent_decode_path_segment("bad%2").has_value());
    EXPECT_FALSE(percent_decode_path_segment("bad%ZZ").has_value());
    EXPECT_FALSE(percent_decode_path_segment("bad%2G").has_value());
    EXPECT_FALSE(percent_decode_path_segment("bad%00tail").has_value());
    EXPECT_FALSE(percent_decode_path_segment(std::string_view("a\0b", 3)).has_value());
}

} // namespace
