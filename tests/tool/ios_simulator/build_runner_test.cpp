#include <gtest/gtest.h>

#include "tool/ios_simulator/build_runner.hpp"

#include <algorithm>

using namespace acecode::ios_simulator;

namespace {
bool contains(const std::vector<std::string>& args, const std::string& value) {
    return std::find(args.begin(), args.end(), value) != args.end();
}
} // namespace

TEST(IosSimulatorBuild, ArgumentsTargetTheSimulator) {
    BuildRequest request;
    request.project = "/w/App.xcodeproj";
    request.scheme = "App";
    request.udid = "UDID-1";
    request.derived_data = "/d";
    auto args = xcodebuild_build_args(request);
    EXPECT_TRUE(contains(args, "-project"));
    EXPECT_TRUE(contains(args, "platform=iOS Simulator,id=UDID-1"));
    EXPECT_TRUE(contains(args, "-derivedDataPath"));
    EXPECT_EQ(args.back(), "build");
    EXPECT_TRUE(contains(args, "Debug"));

    request.workspace = "/w/App.xcworkspace";
    request.udid.clear();
    args = xcodebuild_target_args(request);
    EXPECT_TRUE(contains(args, "-workspace"));
    EXPECT_FALSE(contains(args, "-project"));  // workspace wins
    EXPECT_TRUE(contains(args, "generic/platform=iOS Simulator"));
}

TEST(IosSimulatorBuild, DerivedDataIsStablePerProject) {
    BuildRequest a;
    a.project = "/w/A.xcodeproj";
    BuildRequest b;
    b.project = "/w/B.xcodeproj";
    EXPECT_EQ(derived_data_for("/base", a), derived_data_for("/base", a));
    EXPECT_NE(derived_data_for("/base", a), derived_data_for("/base", b));
    EXPECT_EQ(derived_data_for("/base", a).rfind("/base/", 0), 0u);
}

TEST(IosSimulatorBuild, ExtractsErrorsAndTail) {
    const std::string log =
        "note: building\n/w/A.swift:3:5: error: cannot find 'x' in scope\n"
        "/w/A.swift:3:5: error: cannot find 'x' in scope\n** BUILD FAILED **\n";
    auto errors = extract_build_errors(log);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors[0].find("cannot find 'x'"), std::string::npos);
    EXPECT_EQ(tail_lines(log, 1), "** BUILD FAILED **");
    EXPECT_EQ(tail_lines("a\n\nb\nc\n", 2), "b\nc");
}

TEST(IosSimulatorBuild, FindsTheApplicationProduct) {
    const auto settings = nlohmann::json::parse(R"([
      {"target": "AppTests", "buildSettings": {"WRAPPER_EXTENSION": "xctest", "BUILT_PRODUCTS_DIR": "/p",
                                              "FULL_PRODUCT_NAME": "AppTests.xctest"}},
      {"target": "App", "buildSettings": {"PRODUCT_TYPE": "com.apple.product-type.application",
        "BUILT_PRODUCTS_DIR": "/d/Build/Products/Debug-iphonesimulator", "FULL_PRODUCT_NAME": "App.app",
        "PRODUCT_BUNDLE_IDENTIFIER": "dev.acecode.App", "PRODUCT_NAME": "App"}}
    ])");
    auto product = parse_build_settings(settings);
    ASSERT_TRUE(product.has_value());
    EXPECT_EQ(product->app_path, "/d/Build/Products/Debug-iphonesimulator/App.app");
    EXPECT_EQ(product->bundle_id, "dev.acecode.App");
    EXPECT_FALSE(parse_build_settings(nlohmann::json::object()).has_value());
}

TEST(IosSimulatorBuild, ListsSchemesFromWorkspaceAndProject) {
    const auto list = nlohmann::json::parse(
        R"({"workspace": {"schemes": ["App", "Pods-App"]}, "project": {"schemes": ["App", "AppKit"]}})");
    auto schemes = parse_scheme_list(list);
    EXPECT_EQ(schemes, (std::vector<std::string>{"App", "Pods-App", "AppKit"}));
}
