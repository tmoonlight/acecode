// 图像生成的真实网络冒烟测试(openspec add-image-generation-tool)。
//
// 默认跳过 —— 单测不该依赖网络,而且每跑一次都是一次真实计费请求。
// 手动运行:
//   set ACECODE_RUN_IMAGE_SMOKE=1
//   set ACECODE_IMAGE_BASE_URL=https://.../v1
//   set ACECODE_IMAGE_API_KEY=sk-...
//   set ACECODE_IMAGE_MODEL=acemodel-image        (可选)
//   acecode_unit_tests --gtest_filter=ImageGenerationNetworkSmoke.*
//
// 门控方式对齐 tests/desktop/notifications_native_test.cpp 的
// ACECODE_RUN_NOTIFICATION_SMOKE。凭据只从环境读,绝不写进仓库或配置文件。

#include <gtest/gtest.h>

#include "tool/image_generate/image_generation_client.hpp"
#include "utils/encoding.hpp"

#include <atomic>
#include <cstdlib>
#include <string>

using namespace acecode;
using namespace acecode::image_generation;

namespace {

bool smoke_enabled() {
    return getenv_utf8("ACECODE_RUN_IMAGE_SMOKE") == "1";
}

std::string env_or(const char* name, const std::string& fallback) {
    const std::string value = getenv_utf8(name);
    return value.empty() ? fallback : value;
}

} // namespace

TEST(ImageGenerationNetworkSmoke, GeneratesOneImageFromRealEndpoint) {
    if (!smoke_enabled()) {
        GTEST_SKIP() << "set ACECODE_RUN_IMAGE_SMOKE=1 to run (spends real quota)";
    }

    ImageRequest request;
    request.base_url = env_or("ACECODE_IMAGE_BASE_URL", "");
    request.api_key = env_or("ACECODE_IMAGE_API_KEY", "");
    request.model = env_or("ACECODE_IMAGE_MODEL", "acemodel-image");
    request.prompt = "a plain red square on a white background";
    request.timeout_ms = 300000;

    ASSERT_FALSE(request.base_url.empty()) << "ACECODE_IMAGE_BASE_URL is required";
    ASSERT_FALSE(request.api_key.empty()) << "ACECODE_IMAGE_API_KEY is required";

    std::atomic<bool> abort_flag{false};
    const ImageResponse response = execute_image_request(request, &abort_flag);

    ASSERT_TRUE(response.ok) << "quota_error=" << response.quota_error
                             << " error=" << response.error;
    EXPECT_FALSE(response.b64_data.empty());
    // 实测单张 2MB 起步;明显小于这个量级说明拿到的不是真图。
    EXPECT_GT(response.b64_data.size(), 100000u);
    EXPECT_EQ(response.mime_type.rfind("image/", 0), 0u);
}

TEST(ImageGenerationNetworkSmoke, AbortReturnsPromptlyDuringGeneration) {
    if (!smoke_enabled()) {
        GTEST_SKIP() << "set ACECODE_RUN_IMAGE_SMOKE=1 to run";
    }

    ImageRequest request;
    request.base_url = env_or("ACECODE_IMAGE_BASE_URL", "");
    request.api_key = env_or("ACECODE_IMAGE_API_KEY", "");
    request.model = env_or("ACECODE_IMAGE_MODEL", "acemodel-image");
    request.prompt = "a plain blue circle";
    request.timeout_ms = 300000;

    ASSERT_FALSE(request.base_url.empty());
    ASSERT_FALSE(request.api_key.empty());

    // 预置 abort:生成要 20~60 秒,若中止不生效这条用例会挂到超时。
    // 这正是 McpManager::invoke 踩过的坑 —— 慢调用期间停止请求完全失效。
    std::atomic<bool> abort_flag{true};
    const auto started = std::chrono::steady_clock::now();
    const ImageResponse response = execute_image_request(request, &abort_flag);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_TRUE(response.aborted);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 5)
        << "abort must not wait for the HTTP timeout";
}
