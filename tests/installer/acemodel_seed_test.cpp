// 覆盖 src/apps/installer/acemodel_seed:安装完成页填写 ACEModel Key 后合并 config.json。
// 场景与旧 Inno 安装包的 installer/windows/seed_acemodel.test.ps1 对齐:
//   1. 已有配置:qwen 保持不变、moonlight 被更新、starrylight 的手动能力与协议保留、aurora 新增。
//   2. 其它配置段(mcp_servers 等)与键顺序原样保留。
//   3. 空文件 / 不存在的文件从空对象开始,默认模型设为 moonlight。
//   4. 已有 default_model_name 不覆盖。
//   5. 根节点不是对象、Key 为空时报错且不写文件。
//   6. 文件版:带 UTF-8 BOM 的 config.json 也能读,写回后可被再次解析。

#include "installer/acemodel_seed.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace acecode::installer;
using nlohmann::ordered_json;
namespace fs = std::filesystem;

namespace {

const ordered_json* find_model(const ordered_json& config, const std::string& name) {
    for (const auto& entry : config["saved_models"]) {
        if (entry.value("name", std::string()) == name) return &entry;
    }
    return nullptr;
}

const char* kExistingConfig = R"({
  "provider": "",
  "mcp_servers": {
    "playwright": {"args": ["-y", "@playwright/mcp@latest"], "command": "npx", "disabled": true}
  },
  "saved_models": [
    {"name": "qwen", "provider": "openai", "model": "nvidia/nemotron-3-nano-4b",
     "base_url": "http://127.0.0.1:1234/v1", "api_key": "local", "capabilities": ["tool_use"]},
    {"name": "moonlight", "provider": "openai", "model": "moonlight",
     "base_url": "https://old.example/v1", "api_key": "old-key"},
    {"name": "starrylight", "provider": "openai", "model": "starrylight",
     "api_protocol": "chat_completions", "base_url": "https://old.example/v1", "api_key": "old-key",
     "capabilities": ["tool_use"], "capabilities_source": "manual"}
  ]
})";

}  // namespace

// 场景:用户已有 qwen / moonlight / starrylight 三个模型,填写 Key " test-ace-key-123 "(带空格)。
// 期望:qwen 一字不改;moonlight 换成新 Key 与 ACEModel 地址、补齐协议和能力;
// starrylight 手动设置过的 chat_completions 与 tool_use 能力保留;新增 aurora;Key 去掉首尾空格。
TEST(AceModelSeed, UpdatesPresetsAndKeepsUserChoices) {
    std::string error;
    const auto merged = merge_acemodel_profiles(kExistingConfig, " test-ace-key-123 ", &error);
    ASSERT_TRUE(merged.has_value()) << error;
    const auto config = ordered_json::parse(*merged);

    const auto* qwen = find_model(config, "qwen");
    ASSERT_NE(qwen, nullptr);
    EXPECT_EQ((*qwen)["api_key"], "local");
    EXPECT_EQ((*qwen)["base_url"], "http://127.0.0.1:1234/v1");

    const auto* moonlight = find_model(config, "moonlight");
    ASSERT_NE(moonlight, nullptr);
    EXPECT_EQ((*moonlight)["api_key"], "test-ace-key-123");
    EXPECT_EQ((*moonlight)["base_url"], kAceModelBaseUrl);
    EXPECT_EQ((*moonlight)["api_protocol"], "responses");
    EXPECT_EQ((*moonlight)["endpoint_mode"], "base_url");
    EXPECT_EQ((*moonlight)["context_window"], 250000);
    EXPECT_EQ((*moonlight)["capabilities"], ordered_json::array({"vision", "tool_use"}));
    EXPECT_EQ((*moonlight)["capabilities_source"], "catalog");

    const auto* starrylight = find_model(config, "starrylight");
    ASSERT_NE(starrylight, nullptr);
    EXPECT_EQ((*starrylight)["api_protocol"], "chat_completions");
    EXPECT_EQ((*starrylight)["capabilities"], ordered_json::array({"tool_use"}));
    EXPECT_EQ((*starrylight)["capabilities_source"], "manual");
    EXPECT_EQ((*starrylight)["api_key"], "test-ace-key-123");

    const auto* aurora = find_model(config, "aurora");
    ASSERT_NE(aurora, nullptr);
    EXPECT_EQ((*aurora)["models_dev_provider_id"], "acemodel");
    EXPECT_EQ(config["saved_models"].size(), 4u);
    EXPECT_EQ(config["default_model_name"], "moonlight");
}

// 场景:同上的已有配置。
// 期望:与 ACEModel 无关的段(mcp_servers 里的数组)原样保留,原有键的先后顺序不被重排
// (用 nlohmann 默认 json 会按字母序重写整个文件,用户手写的配置会被打乱)。
TEST(AceModelSeed, PreservesOtherSectionsAndKeyOrder) {
    std::string error;
    const auto merged = merge_acemodel_profiles(kExistingConfig, "k", &error);
    ASSERT_TRUE(merged.has_value()) << error;
    const auto config = ordered_json::parse(*merged);
    EXPECT_EQ(config["mcp_servers"]["playwright"]["args"],
              ordered_json::array({"-y", "@playwright/mcp@latest"}));
    auto it = config.begin();
    EXPECT_EQ(it.key(), "provider");
    ++it;
    EXPECT_EQ(it.key(), "mcp_servers");
    ++it;
    EXPECT_EQ(it.key(), "saved_models");
}

// 场景:首次安装,config.json 不存在(传入空文本)或只有空白;另一份已有 default_model_name。
// 期望:前者得到三个预设并把默认模型设为 moonlight;后者默认模型保持用户原值。
TEST(AceModelSeed, StartsFromEmptyAndKeepsExistingDefault) {
    std::string error;
    auto merged = merge_acemodel_profiles("  \n", "key", &error);
    ASSERT_TRUE(merged.has_value()) << error;
    auto config = ordered_json::parse(*merged);
    EXPECT_EQ(config["saved_models"].size(), 3u);
    EXPECT_EQ(config["default_model_name"], "moonlight");

    merged = merge_acemodel_profiles(R"({"default_model_name": "qwen"})", "key", &error);
    ASSERT_TRUE(merged.has_value()) << error;
    config = ordered_json::parse(*merged);
    EXPECT_EQ(config["default_model_name"], "qwen");
}

// 场景:config.json 根是数组(损坏或手误);Key 只有空白。
// 期望:都返回错误,不产出新内容,调用方据此提示「写入失败」而不是覆盖用户文件。
TEST(AceModelSeed, RejectsInvalidRootAndEmptyKey) {
    std::string error;
    EXPECT_FALSE(merge_acemodel_profiles("[1, 2]", "key", &error).has_value());
    EXPECT_FALSE(error.empty());
    error.clear();
    EXPECT_FALSE(merge_acemodel_profiles("{}", "   ", &error).has_value());
    EXPECT_FALSE(error.empty());
}

// 场景:磁盘上的 config.json 以 UTF-8 BOM 开头(记事本另存会加),目录里还没有别的文件。
// 期望:能读入并写回合法 JSON;临时文件不残留。修复前 BOM 会让 JSON 解析失败,
// 用户填了 Key 却提示写入失败。
TEST(AceModelSeed, FileVariantHandlesBomAndCleansUp) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() / ("acecode-seed-test-" + std::to_string(stamp));
    fs::create_directories(dir);
    const fs::path config = dir / "config.json";
    {
        std::ofstream out(config, std::ios::binary);
        out << "\xEF\xBB\xBF" << R"({"provider": ""})";
    }
    std::string error;
    ASSERT_TRUE(seed_acemodel_config_file(config, "bom-key", &error)) << error;
    std::ifstream in(config, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const auto parsed = ordered_json::parse(text.str(), nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());
    EXPECT_EQ(parsed["saved_models"].size(), 3u);
    EXPECT_FALSE(fs::exists(dir / "config.json.setup-tmp"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
