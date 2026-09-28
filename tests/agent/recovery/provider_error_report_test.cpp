#include "agent/recovery/provider_error_report.hpp"
#include <gtest/gtest.h>

// 重试诊断需要同时保留服务端等待值和本地等待值,错误正文不得替代结构化状态。
TEST(AgentProviderErrorReport, PreservesRetryAndBodyMetadata) {
    acecode::ProviderErrorInfo info;
    info.kind = acecode::ProviderErrorKind::Http;
    info.status_code = 429;
    info.retryable = true;
    info.retry_attempt = 2;
    info.retry_delay_ms = 1250;
    info.server_retry_after_ms = 3000;
    info.raw_body = "{not parsed}";
    const auto payload = acecode::agent::detail::provider_error_to_json(info);
    EXPECT_EQ(payload["kind"], "http");
    EXPECT_EQ(payload["status_code"], 429);
    EXPECT_EQ(payload["retry_delay_ms"], 1250);
    EXPECT_EQ(payload["server_retry_after_ms"], 3000);
    EXPECT_EQ(payload["raw_body"], info.raw_body);
    EXPECT_NE(acecode::agent::detail::provider_error_summary_for_log(info).find("message={not parsed}"), std::string::npos);
}
