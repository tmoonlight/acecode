// 覆盖 model_context_resolver 的非阻塞 session-facing 解析路径。
// 重点保护:session create/resume 不应为了远程 /models 元数据阻塞。

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "config/saved_models.hpp"
#include "provider/model_context_resolver.hpp"
#include "provider/models_dev_registry.hpp"

#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {

using namespace std::chrono_literals;

struct LocalModelMetadataServer {
    httplib::Server server;
    int port = 0;
    std::thread thread;

    explicit LocalModelMetadataServer(std::function<void(httplib::Server&)> setup) {
        setup(server);
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 50 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }

    ~LocalModelMetadataServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

fs::path tmp_dir(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("acecode_model_ctx_" + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << contents;
}

acecode::AppConfig make_openrouter_cfg(const fs::path& registry_path) {
    acecode::AppConfig cfg;
    cfg.provider = "openai";
    cfg.openai.base_url = "http://127.0.0.1:9/v1";
    cfg.openai.api_key = "test-key";
    cfg.openai.model = "poolside/laguna-xs.2:free";
    cfg.openai.models_dev_provider_id = "openrouter";
    cfg.models_dev.user_override_path = registry_path.string();
    return cfg;
}

const char* kOpenRouterRegistry = R"({
  "openrouter": {
    "id": "openrouter",
    "models": {
      "poolside/laguna-xs.2:free": {
        "id": "poolside/laguna-xs.2:free",
        "limit": { "context": 131072 }
      }
    }
  }
})";

const char* kOtherRegistry = R"({
  "other": {
    "id": "other",
    "models": {
      "other-model": { "id": "other-model", "context_length": 4096 }
    }
  }
})";

const char* kGrokRegistryWithPricing = R"({
  "xai": {
    "id": "xai",
    "models": {
      "grok-4.6": {
        "id": "grok-4.6",
        "cost": { "input": 2, "output": 6 },
        "limit": { "context": 500000, "input": 500000, "output": 500000 },
        "modalities": { "input": ["text", "image"], "output": ["text"] }
      }
    }
  }
})";

const char* kGrokRegistryWithPricingOnly = R"({
  "xai": {
    "id": "xai",
    "models": {
      "grok-unknown-window": {
        "id": "grok-unknown-window",
        "cost": { "input": 2, "output": 6 },
        "modalities": { "input": ["text"], "output": ["text"] }
      }
    }
  }
})";

} // namespace

// 场景:本地 models.dev 有匹配项 → 非阻塞解析直接返回准确 context。
TEST(ModelContextResolver, NonblockingUsesLocalModelsDevContext) {
    acecode::reset_model_context_window_cache_for_test();
    auto dir = tmp_dir("local");
    auto registry_path = dir / "api.json";
    write_file(registry_path, kOpenRouterRegistry);

    auto cfg = make_openrouter_cfg(registry_path);
    acecode::initialize_registry(cfg, "");

    int got = acecode::resolve_model_context_window_nonblocking(
        cfg, "openai", cfg.openai.model, 128000);

    EXPECT_EQ(got, 131072);
}

// 场景:Grok 目录同时有 `cost.input=2` 与 `limit.context=500000`。
// 价格字段绝不能被递归误认成 2-token 上下文窗口。
TEST(ModelContextResolver, GrokCatalogUsesTokenLimitInsteadOfInputPrice) {
    acecode::reset_model_context_window_cache_for_test();
    auto dir = tmp_dir("grok_pricing");
    auto registry_path = dir / "api.json";
    write_file(registry_path, kGrokRegistryWithPricing);

    acecode::AppConfig cfg;
    cfg.provider = "grok";
    cfg.context_window = 128000;
    cfg.openai.models_dev_provider_id = "xai";
    cfg.models_dev.user_override_path = registry_path.string();
    acecode::initialize_registry(cfg, "");

    EXPECT_EQ(acecode::resolve_model_context_window_nonblocking(
                  cfg, "grok", "grok-4.6", cfg.context_window),
              500000);
}

// 场景:目录只有价格、没有 Token 上限时必须回退，不能把价格 2 当窗口。
TEST(ModelContextResolver, GrokCatalogInputPriceAloneFallsBack) {
    acecode::reset_model_context_window_cache_for_test();
    auto dir = tmp_dir("grok_pricing_only");
    auto registry_path = dir / "api.json";
    write_file(registry_path, kGrokRegistryWithPricingOnly);

    acecode::AppConfig cfg;
    cfg.provider = "grok";
    cfg.context_window = 128000;
    cfg.openai.models_dev_provider_id = "xai";
    cfg.models_dev.user_override_path = registry_path.string();
    acecode::initialize_registry(cfg, "");

    EXPECT_EQ(acecode::resolve_model_context_window_nonblocking(
                  cfg, "grok", "grok-unknown-window", cfg.context_window),
              cfg.context_window);
}

// 场景:首次命中本地 metadata 后写入进程缓存;随后 registry 丢失同 provider,
// 同一模型 key 仍从缓存返回,不退回 fallback。
TEST(ModelContextResolver, NonblockingReturnsProcessCacheBeforeFallback) {
    acecode::reset_model_context_window_cache_for_test();
    auto dir = tmp_dir("cache");
    auto registry_path = dir / "api.json";
    write_file(registry_path, kOpenRouterRegistry);

    auto cfg = make_openrouter_cfg(registry_path);
    acecode::initialize_registry(cfg, "");
    ASSERT_EQ(acecode::resolve_model_context_window_nonblocking(
                  cfg, "openai", cfg.openai.model, 128000),
              131072);

    auto other_path = dir / "other.json";
    write_file(other_path, kOtherRegistry);
    auto cfg_without_match = cfg;
    cfg_without_match.models_dev.user_override_path = other_path.string();
    acecode::initialize_registry(cfg_without_match, "");

    int got = acecode::resolve_model_context_window_nonblocking(
        cfg_without_match, "openai", cfg_without_match.openai.model, 128000);

    EXPECT_EQ(got, 131072);
}

// 场景:没有缓存/本地 metadata 且不能 probe endpoint → 立即返回 fallback。
TEST(ModelContextResolver, NonblockingFallsBackWithoutEndpointProbe) {
    acecode::reset_model_context_window_cache_for_test();
    auto dir = tmp_dir("fallback");
    auto registry_path = dir / "api.json";
    write_file(registry_path, kOtherRegistry);

    auto cfg = make_openrouter_cfg(registry_path);
    cfg.openai.base_url.clear();
    acecode::initialize_registry(cfg, "");

    int got = acecode::resolve_model_context_window_nonblocking(
        cfg, "openai", cfg.openai.model, 77777);

    EXPECT_EQ(got, 77777);
}

// 场景:ACEModel 不存在于 models.dev 且 /models 没有上下文元数据时，
// 三个内置模型仍使用 250K 本地回退，不落回全局 128K。
TEST(ModelContextResolver, NonblockingUsesAceModelBuiltinContext) {
    acecode::reset_model_context_window_cache_for_test();
    acecode::AppConfig cfg;
    cfg.provider = "openai";
    cfg.context_window = 128000;
    cfg.openai.models_dev_provider_id = "acemodel";
    cfg.openai.base_url.clear();

    for (const char* model : {"moonlight", "starrylight", "aurora"}) {
        EXPECT_EQ(acecode::resolve_model_context_window_nonblocking(
                      cfg, "openai", model, cfg.context_window),
                  250000);
    }
    EXPECT_EQ(acecode::resolve_model_context_window_nonblocking(
                  cfg, "openai", "unknown", cfg.context_window),
              cfg.context_window);
}

// 场景:ACEModel 阻塞解析优先读取 /models。大于和小于 250K 的有效
// 服务器值都保持原样，只有缺失字段的模型使用 250K 回退。
TEST(ModelContextResolver, BlockingAceModelUsesServerContextBeforeFallback) {
    acecode::reset_model_context_window_cache_for_test();
    LocalModelMetadataServer metadata_server([](httplib::Server& server) {
        server.Get("/models", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(R"({
                "data": [
                    {"id":"aurora","max_context_tokens":"1000000"},
                    {"id":"starrylight","context_window":128000},
                    {"id":"moonlight"}
                ]
            })", "application/json");
        });
    });

    acecode::AppConfig cfg;
    cfg.provider = "openai";
    cfg.context_window = 128000;
    cfg.openai.models_dev_provider_id = "acemodel";
    cfg.openai.base_url = metadata_server.base_url();

    EXPECT_EQ(acecode::resolve_model_context_window(
                  cfg, "openai", "aurora", cfg.context_window),
              1000000);
    EXPECT_EQ(acecode::resolve_model_context_window(
                  cfg, "openai", "starrylight", cfg.context_window),
              128000);
    EXPECT_EQ(acecode::resolve_model_context_window(
                  cfg, "openai", "moonlight", cfg.context_window),
              250000);
}

// 场景:catalog/seeder 写入的 250K 只是回退，不能遮蔽服务器元数据；
// 用户标记为 manual 的同值仍是最终覆盖。
TEST(ModelContextResolver, AceModelCatalogFallbackIsNotManualOverride) {
    acecode::reset_model_context_window_cache_for_test();
    LocalModelMetadataServer metadata_server([](httplib::Server& server) {
        server.Get("/models", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(
                R"({"data":[{"id":"aurora","context_window":1000000}]})",
                "application/json");
        });
    });

    acecode::AppConfig cfg;
    cfg.context_window = 128000;
    acecode::ModelProfile profile;
    profile.name = "aurora";
    profile.provider = "openai";
    profile.base_url = metadata_server.base_url();
    profile.api_key = "test";
    profile.model = "aurora";
    profile.models_dev_provider_id = "acemodel";
    profile.context_window = 250000;
    profile.capabilities_source = "catalog";

    EXPECT_EQ(acecode::resolve_model_profile_context_window(
                  cfg, profile, cfg.context_window),
              1000000);

    profile.capabilities_source = "manual";
    EXPECT_EQ(acecode::resolve_model_profile_context_window(
                  cfg, profile, cfg.context_window),
              250000);
}

// 场景:Codex provider 使用 Codex CLI 模型 catalog 的运行上下文,不回退到全局 128k。
TEST(ModelContextResolver, NonblockingUsesCodexModelContext) {
    acecode::reset_model_context_window_cache_for_test();
    acecode::AppConfig cfg;
    cfg.provider = "codex";
    cfg.context_window = 128000;

    EXPECT_EQ(acecode::resolve_model_context_window_nonblocking(
                  cfg, "codex", "gpt-5.5", cfg.context_window),
              272000);
    EXPECT_EQ(acecode::resolve_model_context_window_nonblocking(
                  cfg, "codex", "gpt-5.3-codex-spark", cfg.context_window),
              128000);
}

// 场景:saved model 配了手动 context_window → 优先于 models.dev / cache / fallback。
TEST(ModelContextResolver, ProfileContextWindowOverrideWins) {
    acecode::reset_model_context_window_cache_for_test();
    auto dir = tmp_dir("profile_override");
    auto registry_path = dir / "api.json";
    write_file(registry_path, kOpenRouterRegistry);

    auto cfg = make_openrouter_cfg(registry_path);
    acecode::initialize_registry(cfg, "");

    acecode::ModelProfile profile;
    profile.name = "manual";
    profile.provider = "openai";
    profile.base_url = cfg.openai.base_url;
    profile.api_key = cfg.openai.api_key;
    profile.model = cfg.openai.model;
    profile.models_dev_provider_id = "openrouter";
    profile.context_window = 64000;

    EXPECT_EQ(acecode::resolve_model_profile_context_window_nonblocking(
                  cfg, profile, 128000),
              64000);
    EXPECT_EQ(acecode::resolve_model_profile_context_window(
                  cfg, profile, 128000),
              64000);
}

// 场景:model pool 已发现同 model 的窗口时,用户在 saved model 上填写的
// 手动值仍然是最终 runtime 预算;清空手动值后才采用 pool 窗口。
TEST(ModelContextResolver, RuntimeProfileOverrideWinsOverModelPoolWindow) {
    acecode::AppConfig cfg;
    cfg.context_window = 128000;

    acecode::ModelProfile profile;
    profile.name = "manual";
    profile.provider = "copilot";
    profile.model = "pool-model";
    profile.context_window = 64000;

    EXPECT_EQ(acecode::resolve_runtime_model_profile_context_window_nonblocking(
                  cfg, profile, cfg.context_window, 120000),
              64000);

    profile.context_window.reset();
    EXPECT_EQ(acecode::resolve_runtime_model_profile_context_window_nonblocking(
                  cfg, profile, cfg.context_window, 120000),
              120000);
}
