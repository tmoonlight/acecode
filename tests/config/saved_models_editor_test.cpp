// tests/config/saved_models_editor_test.cpp
//
// 覆盖 src/config/saved_models_editor.cpp。这是 saved_models 增删改的唯一
// 校验入口(daemon HTTP / TUI 命令都过这里),九个错误码每个都有 UI 文案
// 依赖,任一被绕过都会让坏数据进 cfg.json。
//
// 触发场景 / 期望:
//   - 各类无效 draft → 对应错误码 + cfg 不变(回滚验证)
//   - 成功路径:add / update / remove 后 cfg.saved_models 长度与字段正确
//   - update 改名走 delete+add;若 old_name 是 default → IN_USE_AS_DEFAULT
//   - remove default → IN_USE_AS_DEFAULT,cfg 不变

#include <gtest/gtest.h>

#include "config/saved_models_editor.hpp"

#include "config/config.hpp"
#include "config/saved_models.hpp"

using acecode::AppConfig;
using acecode::add_saved_model;
using acecode::ModelProfile;
using acecode::remove_saved_model;
using acecode::SavedModelDraft;
using acecode::SavedModelEditError;
using acecode::update_saved_model;

namespace {

AppConfig make_cfg_with_one_default() {
    AppConfig cfg;
    cfg.provider = "copilot";
    cfg.copilot.model = "gpt-4o";
    ModelProfile a;
    a.name = "copilot-fast";
    a.provider = "copilot";
    a.model = "gpt-4o";
    cfg.saved_models.push_back(a);
    cfg.default_model_name = "copilot-fast";
    return cfg;
}

SavedModelDraft good_openai_draft(const std::string& name = "local-lm") {
    SavedModelDraft d;
    d.name = name;
    d.provider = "openai";
    d.model = "llama-3";
    d.base_url = "http://localhost:1234/v1";
    d.api_key = "sk-x";
    return d;
}

} // namespace

// 场景:add 一个空名字 → INVALID_NAME,cfg 不变。
TEST(SavedModelsEditor, AddRejectsEmptyName) {
    auto cfg = make_cfg_with_one_default();
    auto before = cfg.saved_models.size();
    SavedModelDraft d = good_openai_draft("");
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::INVALID_NAME);
    EXPECT_EQ(cfg.saved_models.size(), before);
}

// 场景:add 名字以 ( 开头 → RESERVED_NAME。前缀 ( 给 (legacy)/(session:...) 留。
TEST(SavedModelsEditor, AddRejectsReservedNamePrefix) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("(my-fav)");
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::RESERVED_NAME);
}

// 场景:add 重名 → NAME_TAKEN。
TEST(SavedModelsEditor, AddRejectsNameTaken) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("copilot-fast");  // 已存在
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::NAME_TAKEN);
}

// 场景:add 未知 provider → UNKNOWN_PROVIDER。
TEST(SavedModelsEditor, AddRejectsUnknownProvider) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft();
    d.provider = "anthropic";
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::UNKNOWN_PROVIDER);
}

// 场景:add openai 但 model 空 → MISSING_MODEL。
TEST(SavedModelsEditor, AddRejectsMissingModel) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft();
    d.model = "";
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::MISSING_MODEL);
}

// 场景:add openai 但 base_url 空 → MISSING_BASE_URL。
TEST(SavedModelsEditor, AddRejectsMissingBaseUrl) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft();
    d.base_url = "";
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::MISSING_BASE_URL);
}

// 场景:add openai 但 api_key 空 → INVALID_API_KEY(沿用 validate_saved_models 规则)。
TEST(SavedModelsEditor, AddRejectsEmptyApiKey) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft();
    d.api_key = "";
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::INVALID_API_KEY);
}

// 场景:add 成功 → cfg.saved_models 增 1,字段正确。
TEST(SavedModelsEditor, AddAppendsValidDraft) {
    auto cfg = make_cfg_with_one_default();
    auto d = good_openai_draft("local-lm");
    EXPECT_EQ(add_saved_model(cfg, d), SavedModelEditError::OK);
    ASSERT_EQ(cfg.saved_models.size(), 2u);
    EXPECT_EQ(cfg.saved_models[1].name, "local-lm");
    EXPECT_EQ(cfg.saved_models[1].base_url, "http://localhost:1234/v1");
    EXPECT_EQ(cfg.saved_models[1].api_key, "sk-x");
}

// 场景:update 不存在的 name → NOT_FOUND,cfg 不变。
TEST(SavedModelsEditor, UpdateRejectsNotFound) {
    auto cfg = make_cfg_with_one_default();
    auto before = cfg.saved_models;
    auto d = good_openai_draft("nope");
    EXPECT_EQ(update_saved_model(cfg, "nope", d), SavedModelEditError::NOT_FOUND);
    EXPECT_EQ(cfg.saved_models.size(), before.size());
}

// 场景:update 改名时旧名是默认 → IN_USE_AS_DEFAULT。
TEST(SavedModelsEditor, UpdateRefusesRenamingDefault) {
    auto cfg = make_cfg_with_one_default();  // default = copilot-fast
    auto d = good_openai_draft("copilot-fast-v2");
    d.provider = "copilot";
    d.base_url.clear();
    d.api_key.clear();
    EXPECT_EQ(update_saved_model(cfg, "copilot-fast", d),
              SavedModelEditError::IN_USE_AS_DEFAULT);
    EXPECT_EQ(cfg.saved_models[0].name, "copilot-fast");  // 未变
}

// 场景:update 同名改字段(非 default 条目) → 字段被替换。
TEST(SavedModelsEditor, UpdateReplacesFieldsForSameName) {
    auto cfg = make_cfg_with_one_default();
    add_saved_model(cfg, good_openai_draft("local-lm"));
    SavedModelDraft d = good_openai_draft("local-lm");
    d.api_key = "sk-new";
    EXPECT_EQ(update_saved_model(cfg, "local-lm", d), SavedModelEditError::OK);
    EXPECT_EQ(cfg.saved_models[1].api_key, "sk-new");
}

// 场景:remove 默认条目 → IN_USE_AS_DEFAULT,cfg 不变。
TEST(SavedModelsEditor, RemoveRefusesDefault) {
    auto cfg = make_cfg_with_one_default();
    EXPECT_EQ(remove_saved_model(cfg, "copilot-fast"),
              SavedModelEditError::IN_USE_AS_DEFAULT);
    EXPECT_EQ(cfg.saved_models.size(), 1u);
}

// 场景:remove 非默认 → 长度 -1。
TEST(SavedModelsEditor, RemoveDeletesNonDefault) {
    auto cfg = make_cfg_with_one_default();
    add_saved_model(cfg, good_openai_draft("local-lm"));
    EXPECT_EQ(remove_saved_model(cfg, "local-lm"), SavedModelEditError::OK);
    EXPECT_EQ(cfg.saved_models.size(), 1u);
    EXPECT_EQ(cfg.saved_models[0].name, "copilot-fast");
}

// 场景:remove 不存在 → NOT_FOUND。
TEST(SavedModelsEditor, RemoveRejectsNotFound) {
    auto cfg = make_cfg_with_one_default();
    EXPECT_EQ(remove_saved_model(cfg, "ghost"), SavedModelEditError::NOT_FOUND);
}
