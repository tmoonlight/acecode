#include <gtest/gtest.h>

#include "experts/expert_registry.hpp"
#include "../agent_loop/stub_provider.hpp"
#include "permissions.hpp"
#include "session/session_registry.hpp"
#include "session/session_storage.hpp"
#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("acecode_experts_test_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

acecode::ExpertDraft make_agent(std::string id = "reviewer",
                                std::string display = "Code Reviewer") {
    acecode::ExpertDraft draft;
    draft.id = std::move(id);
    draft.display_name = std::move(display);
    draft.profession = "Review specialist";
    draft.description = "Finds correctness and security issues.";
    draft.quick_prompts = {"Review this change"};
    draft.lead = {"lead", "Lead Reviewer", "Reviewer", "Review code carefully."};
    return draft;
}

} // namespace

TEST(ExpertRegistry, CreatesDiscoversUpdatesAndDeletesGlobalExpert) {
    TempDir temp;
    acecode::ExpertRegistry registry(temp.path / "global");
    std::string error;

    auto draft = make_agent();
    ASSERT_TRUE(registry.create_global(draft, &error)) << error;
    ASSERT_FALSE(registry.create_global(draft, &error));

    auto found = registry.find(acecode::path_to_utf8(temp.path), "reviewer");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->display_name, "Code Reviewer");
    EXPECT_EQ(found->source, "global");
    EXPECT_TRUE(found->managed_global);
    ASSERT_NE(found->selected_agent(), nullptr);
    EXPECT_EQ(found->selected_agent()->instructions, "Review code carefully.");

    draft.display_name = "Senior Reviewer";
    ASSERT_TRUE(registry.update_global("reviewer", draft, &error)) << error;
    found = registry.find(acecode::path_to_utf8(temp.path), "reviewer");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->display_name, "Senior Reviewer");

    ASSERT_TRUE(registry.delete_global("reviewer", &error)) << error;
    EXPECT_FALSE(registry.find(acecode::path_to_utf8(temp.path), "reviewer").has_value());
}

TEST(ExpertRegistry, ClosestWorkspaceExpertShadowsGlobalExpert) {
    TempDir temp;
    const fs::path workspace = temp.path / "project" / "nested";
    fs::create_directories(workspace);
    acecode::ExpertRegistry global_writer(temp.path / "global");
    std::string error;
    ASSERT_TRUE(global_writer.create_global(make_agent("reviewer", "Global"), &error)) << error;

    acecode::ExpertRegistry workspace_writer(temp.path / "project" / ".acecode" / "experts");
    ASSERT_TRUE(workspace_writer.create_global(make_agent("reviewer", "Workspace"), &error)) << error;

    acecode::ExpertRegistry registry(temp.path / "global");
    auto found = registry.find(acecode::path_to_utf8(workspace), "reviewer");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->display_name, "Workspace");
    EXPECT_EQ(found->source, "workspace");
    EXPECT_FALSE(found->managed_global);
}

TEST(ExpertRegistry, RejectsEscapedAgentAndSkillPaths) {
    TempDir temp;
    const fs::path root = temp.path / "global";
    const fs::path package = root / "unsafe";
    fs::create_directories(package);
    std::ofstream(temp.path / "outside.md") << "Do unsafe work";
    std::ofstream(package / "expert.json")
        << R"({"name":"unsafe","expertType":"agent","displayName":"Unsafe","agentName":"lead","agents":[{"id":"lead","path":"../../outside.md"}]})";

    acecode::ExpertRegistry registry(root);
    std::vector<acecode::ExpertDiagnostic> diagnostics;
    EXPECT_TRUE(registry.list(acecode::path_to_utf8(temp.path), &diagnostics).empty());
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics.front().message.find("escapes"), std::string::npos);
}

TEST(ExpertRegistry, TeamReferencesExistingExpertsWithoutCopyingDefinitions) {
    TempDir temp;
    acecode::ExpertRegistry registry(temp.path / "global");
    std::string error;
    ASSERT_TRUE(registry.create_global(make_agent("reviewer", "Reviewer"), &error)) << error;
    auto tester = make_agent("tester", "Tester");
    tester.profession = "QA";
    tester.lead.profession = "QA";
    tester.lead.instructions = "Test the implementation.";
    ASSERT_TRUE(registry.create_global(tester, &error)) << error;

    acecode::ExpertDraft team;
    team.id = "delivery-team";
    team.type = acecode::ExpertType::Team;
    team.display_name = "Delivery Team";
    team.description = "Ship reliable changes.";
    team.lead_expert_id = "reviewer";
    team.member_expert_ids = {"tester"};
    ASSERT_TRUE(registry.create_global(team, &error)) << error;

    auto found = registry.find(acecode::path_to_utf8(temp.path), "delivery-team");
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found->references_existing_experts);
    EXPECT_EQ(found->lead_expert_id, "reviewer");
    EXPECT_EQ(found->member_expert_ids, std::vector<std::string>({"tester"}));
    EXPECT_TRUE(found->is_declared_member("tester"));
    EXPECT_FALSE(found->is_declared_member("intruder"));
    ASSERT_NE(found->selected_agent(), nullptr);
    EXPECT_EQ(found->selected_agent()->display_name, "Reviewer");
    ASSERT_NE(found->selected_agent("tester"), nullptr);
    EXPECT_EQ(found->selected_agent("tester")->profession, "QA");
    EXPECT_EQ(found->selected_agent("tester")->instructions,
              "Test the implementation.");

    std::ifstream manifest_input(
        temp.path / "global" / "delivery-team" / "expert.json");
    const auto manifest = nlohmann::json::parse(manifest_input);
    manifest_input.close();
    EXPECT_FALSE(manifest.contains("agents"));
    EXPECT_EQ(manifest["teamInfo"]["leadExpert"], "reviewer");
    EXPECT_EQ(manifest["teamInfo"]["memberExperts"][0], "tester");

    const fs::path stale_agents =
        temp.path / "global" / "delivery-team" / "agents";
    fs::create_directories(stale_agents);
    std::ofstream(stale_agents / "legacy-copy.md") << "Legacy member copy.";
    team.description = "Updated without copied member documents.";
    ASSERT_TRUE(registry.update_global("delivery-team", team, &error)) << error;
    EXPECT_FALSE(fs::exists(stale_agents));

    tester.display_name = "Senior Tester";
    tester.lead.instructions = "Run the current acceptance plan.";
    ASSERT_TRUE(registry.update_global("tester", tester, &error)) << error;
    found = registry.find(acecode::path_to_utf8(temp.path), "delivery-team");
    ASSERT_TRUE(found.has_value());
    ASSERT_NE(found->selected_agent("tester"), nullptr);
    EXPECT_EQ(found->selected_agent("tester")->display_name, "Senior Tester");
    EXPECT_EQ(found->selected_agent("tester")->instructions,
              "Run the current acceptance plan.");
}

TEST(ExpertRegistry, ReferencedTeamKeepsEachExpertsOwnSkillRoots) {
    TempDir temp;
    const fs::path global_root = temp.path / "global";
    acecode::ExpertRegistry registry(global_root);
    std::string error;
    auto reviewer = make_agent("reviewer", "Reviewer");
    reviewer.capabilities.skills =
        std::vector<std::string>{"review-skill"};
    reviewer.capabilities.mcp_servers =
        std::vector<std::string>{"review-mcp"};
    reviewer.capabilities.tools =
        std::vector<std::string>{"file_read"};
    ASSERT_TRUE(registry.create_global(reviewer, &error)) << error;
    auto tester = make_agent("tester", "Tester");
    tester.capabilities.skills =
        std::vector<std::string>{"test-skill"};
    tester.capabilities.mcp_servers =
        std::vector<std::string>{"test-mcp"};
    tester.capabilities.tools =
        std::vector<std::string>{"file_write"};
    ASSERT_TRUE(registry.create_global(tester, &error)) << error;

    for (const std::string& id : {"reviewer", "tester"}) {
        const fs::path package = global_root / id;
        fs::create_directories(package / "skills");
        const std::string skill_name =
            id == "reviewer" ? "review-skill" : "test-skill";
        fs::create_directories(package / "skills" / skill_name);
        std::ofstream(package / "skills" / skill_name / "SKILL.md")
            << "---\nname: " << skill_name
            << "\ndescription: Team member skill\n---\n";
        std::ifstream input(package / "expert.json");
        auto manifest = nlohmann::json::parse(input);
        input.close();
        manifest["skills"] = nlohmann::json::array({"skills"});
        std::ofstream(package / "expert.json") << manifest.dump(2) << '\n';
    }

    acecode::ExpertDraft team;
    team.id = "delivery-team";
    team.type = acecode::ExpertType::Team;
    team.display_name = "Delivery Team";
    team.lead_expert_id = "reviewer";
    team.member_expert_ids = {"tester"};
    ASSERT_TRUE(registry.create_global(team, &error)) << error;

    const auto found =
        registry.find(acecode::path_to_utf8(temp.path), "delivery-team");
    ASSERT_TRUE(found.has_value());
    const auto lead_roots = found->selected_skill_roots();
    const auto member_roots = found->selected_skill_roots("tester");
    ASSERT_EQ(lead_roots.size(), 1u);
    ASSERT_EQ(member_roots.size(), 1u);
    EXPECT_EQ(lead_roots.front(),
              fs::weakly_canonical(global_root / "reviewer" / "skills"));
    EXPECT_EQ(member_roots.front(),
              fs::weakly_canonical(global_root / "tester" / "skills"));

    const auto lead_scopes = found->selected_capabilities();
    const auto member_scopes = found->selected_capabilities("tester");
    ASSERT_TRUE(lead_scopes.tools.has_value());
    ASSERT_TRUE(member_scopes.tools.has_value());
    EXPECT_EQ(*lead_scopes.tools,
              std::vector<std::string>({"file_read"}));
    EXPECT_EQ(*member_scopes.tools,
              std::vector<std::string>({"file_write"}));
    EXPECT_EQ(*lead_scopes.mcp_servers,
              std::vector<std::string>({"review-mcp"}));
    EXPECT_EQ(*member_scopes.mcp_servers,
              std::vector<std::string>({"test-mcp"}));

    acecode::AppConfig cfg;
    cfg.skills.reuse_opencode = false;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::SessionRegistryDeps deps;
    deps.provider_accessor =
        [] { return std::shared_ptr<acecode::LlmProvider>{}; };
    deps.tools = &tools;
    deps.cwd = acecode::path_to_utf8(temp.path);
    deps.config = &cfg;
    deps.expert_registry = &registry;
    deps.template_permissions = &permissions;
    acecode::SessionRegistry sessions(std::move(deps));

    acecode::SessionOptions lead_options;
    lead_options.cwd = acecode::path_to_utf8(temp.path);
    lead_options.expert_id = "delivery-team";
    const std::string lead_id = sessions.create(lead_options);
    auto lead_entry = sessions.acquire(lead_id);
    ASSERT_NE(lead_entry, nullptr);
    ASSERT_TRUE(lead_entry->tool_capability_policy.builtin_tools.has_value());
    EXPECT_TRUE(lead_entry->tool_capability_policy.builtin_tools->count(
                    "file_read") != 0);
    EXPECT_TRUE(lead_entry->tool_capability_policy.mcp_servers->count(
                    "review-mcp") != 0);
    ASSERT_NE(lead_entry->skill_registry, nullptr);
    EXPECT_TRUE(lead_entry->skill_registry->find("review-skill").has_value());
    EXPECT_FALSE(lead_entry->skill_registry->find("test-skill").has_value());

    auto member_options = lead_options;
    member_options.expert_member_id = "tester";
    const std::string member_id = sessions.create(member_options);
    auto member_entry = sessions.acquire(member_id);
    ASSERT_NE(member_entry, nullptr);
    ASSERT_TRUE(
        member_entry->tool_capability_policy.builtin_tools.has_value());
    EXPECT_TRUE(member_entry->tool_capability_policy.builtin_tools->count(
                    "file_write") != 0);
    EXPECT_FALSE(member_entry->tool_capability_policy.builtin_tools->count(
                     "file_read") != 0);
    EXPECT_TRUE(member_entry->tool_capability_policy.mcp_servers->count(
                    "test-mcp") != 0);
    ASSERT_NE(member_entry->skill_registry, nullptr);
    EXPECT_TRUE(member_entry->skill_registry->find("test-skill").has_value());
    EXPECT_FALSE(
        member_entry->skill_registry->find("review-skill").has_value());

    sessions.destroy(lead_id);
    sessions.destroy(member_id);
}

TEST(ExpertRegistry, RejectsMissingSelfNestedAndOutOfScopeTeamReferences) {
    TempDir temp;
    const fs::path workspace = temp.path / "workspace";
    const fs::path other_workspace = temp.path / "other";
    fs::create_directories(workspace);
    fs::create_directories(other_workspace);
    acecode::ExpertRegistry registry(temp.path / "global");
    std::string error;

    ASSERT_TRUE(registry.create_global(make_agent("reviewer", "Reviewer"), &error)) << error;
    acecode::ExpertDraft missing;
    missing.id = "missing-team";
    missing.type = acecode::ExpertType::Team;
    missing.display_name = "Missing Team";
    missing.lead_expert_id = "reviewer";
    missing.member_expert_ids = {"not-installed"};
    EXPECT_FALSE(registry.create_global(missing, &error));

    auto self = missing;
    self.id = "self-team";
    self.display_name = "Self Team";
    self.lead_expert_id = "self-team";
    self.member_expert_ids = {"reviewer"};
    EXPECT_FALSE(registry.create_global(self, &error));

    acecode::ExpertRegistry workspace_writer(workspace / ".acecode" / "experts");
    ASSERT_TRUE(workspace_writer.create_global(
        make_agent("workspace-tester", "Workspace Tester"), &error)) << error;
    acecode::ExpertDraft scoped;
    scoped.id = "scoped-team";
    scoped.type = acecode::ExpertType::Team;
    scoped.display_name = "Scoped Team";
    scoped.lead_expert_id = "reviewer";
    scoped.member_expert_ids = {"workspace-tester"};
    ASSERT_TRUE(registry.create_global(scoped, &error,
                                      acecode::path_to_utf8(workspace))) << error;
    EXPECT_TRUE(registry.find(acecode::path_to_utf8(workspace),
                              "scoped-team").has_value());
    EXPECT_FALSE(registry.find(acecode::path_to_utf8(other_workspace),
                               "scoped-team").has_value());

    ASSERT_TRUE(registry.create_global(
        make_agent("global-tester", "Global Tester"), &error)) << error;
    acecode::ExpertDraft base_team;
    base_team.id = "base-team";
    base_team.type = acecode::ExpertType::Team;
    base_team.display_name = "Base Team";
    base_team.lead_expert_id = "reviewer";
    base_team.member_expert_ids = {"global-tester"};
    ASSERT_TRUE(registry.create_global(base_team, &error)) << error;

    acecode::ExpertDraft nested = base_team;
    nested.id = "nested-team";
    nested.display_name = "Nested Team";
    nested.lead_expert_id = "base-team";
    EXPECT_FALSE(registry.create_global(nested, &error));
}

TEST(ExpertRegistry, DraftJsonRejectsInvalidIdentifiersAndToolFieldsDoNotMatter) {
    std::string error;
    auto invalid = acecode::ExpertRegistry::draft_from_json({
        {"id", "../bad"},
        {"display_name", "Bad"},
        {"instructions", "Bad instructions"},
    }, &error);
    EXPECT_FALSE(invalid.has_value());

    auto valid = acecode::ExpertRegistry::draft_from_json({
        {"id", "safe"},
        {"display_name", "Safe"},
        {"instructions", "Act safely."},
        {"tools", nlohmann::json::array({"shell", "admin"})},
    }, &error);
    ASSERT_TRUE(valid.has_value()) << error;
    EXPECT_EQ(valid->lead.instructions, "Act safely.");
}

TEST(ExpertRegistry, NormalizesMetadataAndPreservesOptionalCapabilityStates) {
    std::string error;
    auto draft = acecode::ExpertRegistry::draft_from_json({
        {"id", "unicode-expert"},
        {"display_name", "研发专家"},
        {"author", "吴八哥"},
        {"profession", "开发"},
        {"description", "处理复杂研发任务"},
        {"instructions", "严格验证每一项改动。"},
        {"tags", nlohmann::json::array({" 开发 ", "", "OPC-一人公司", "开发"})},
        {"expertise", nlohmann::json::array({"系统架构", " 高级开发 ", "系统架构"})},
        {"quick_prompts", nlohmann::json::array({"审查当前改动", " ", "审查当前改动"})},
        {"capabilities", {
            {"skills", nlohmann::json::array()},
            {"mcp_servers", nlohmann::json::array({" github ", "missing", "github"})},
            {"tools", nlohmann::json::array({"file_read", "AskUserQuestion"})},
        }},
    }, &error);
    ASSERT_TRUE(draft.has_value()) << error;
    EXPECT_EQ(draft->author, "吴八哥");
    EXPECT_EQ(draft->tags,
              std::vector<std::string>({"开发", "OPC-一人公司"}));
    EXPECT_EQ(draft->expertise,
              std::vector<std::string>({"系统架构", "高级开发"}));
    EXPECT_EQ(draft->quick_prompts,
              std::vector<std::string>({"审查当前改动"}));
    ASSERT_TRUE(draft->capabilities.skills.has_value());
    EXPECT_TRUE(draft->capabilities.skills->empty());
    ASSERT_TRUE(draft->capabilities.mcp_servers.has_value());
    EXPECT_EQ(*draft->capabilities.mcp_servers,
              std::vector<std::string>({"github", "missing"}));
    ASSERT_TRUE(draft->capabilities.tools.has_value());
    EXPECT_EQ(*draft->capabilities.tools,
              std::vector<std::string>({"file_read", "AskUserQuestion"}));

    TempDir temp;
    acecode::ExpertRegistry registry(temp.path / "global");
    ASSERT_TRUE(registry.create_global(*draft, &error)) << error;
    auto found = registry.find(acecode::path_to_utf8(temp.path),
                               "unicode-expert");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->author, "吴八哥");
    EXPECT_EQ(found->tags, draft->tags);
    EXPECT_EQ(found->expertise, draft->expertise);
    ASSERT_TRUE(found->capabilities.skills.has_value());
    EXPECT_TRUE(found->capabilities.skills->empty());
    ASSERT_TRUE(found->capabilities.mcp_servers.has_value());
    EXPECT_EQ(*found->capabilities.mcp_servers,
              std::vector<std::string>({"github", "missing"}));
    EXPECT_FALSE(found->created_at.empty());
    EXPECT_FALSE(found->updated_at.empty());

    const auto dto = acecode::expert_definition_to_json(*found, true);
    EXPECT_EQ(dto["author"], "吴八哥");
    EXPECT_EQ(dto["capabilities"]["skills"], nlohmann::json::array());
    EXPECT_EQ(dto["capabilities"]["mcp_servers"][1], "missing");
    EXPECT_FALSE(dto.contains("package_root"));
    EXPECT_FALSE(dto.contains("skill_roots"));
}

TEST(ExpertRegistry, UpdateMergesManagedFieldsWithoutLosingPackageData) {
    TempDir temp;
    const fs::path global_root = temp.path / "global";
    acecode::ExpertRegistry registry(global_root);
    std::string error;

    auto initial = make_agent("preserved", "Original");
    initial.capabilities_present = true;
    initial.capabilities.skills =
        std::vector<std::string>{"frontend-design", "missing-skill"};
    initial.capabilities.mcp_servers =
        std::vector<std::string>{"github"};
    initial.capabilities.tools =
        std::vector<std::string>{"file_read"};
    ASSERT_TRUE(registry.create_global(initial, &error)) << error;

    const fs::path package = global_root / "preserved";
    fs::create_directories(package / "skills" / "bundled");
    fs::create_directories(package / "resources");
    std::ofstream(package / "skills" / "bundled" / "SKILL.md")
        << "---\nname: bundled\n---\nBundled skill.\n";
    std::ofstream(package / "resources" / "guide.txt") << "keep me";
    std::ofstream(package / "avatar.svg") << "<svg/>";

    std::ifstream input(package / "expert.json");
    auto manifest = nlohmann::json::parse(input);
    input.close();
    const std::string created_at =
        manifest.value("created_at", std::string{});
    manifest["avatar"] = "avatar.svg";
    manifest["skills"] = nlohmann::json::array({"skills"});
    manifest["x-forward-compatible"] = {
        {"nested", true},
    };
    manifest["capabilities"]["future_scope"] =
        nlohmann::json::array({"keep"});
    std::ofstream(package / "expert.json") << manifest.dump(2) << '\n';

    auto legacy_client_update = make_agent("preserved", "Legacy Update");
    legacy_client_update.author = "作者";
    legacy_client_update.tags = {"开发", "开发", " OPC "};
    legacy_client_update.expertise = {"架构", "", "架构"};
    ASSERT_TRUE(registry.update_global("preserved", legacy_client_update, &error))
        << error;

    std::ifstream preserved_input(package / "expert.json");
    auto preserved = nlohmann::json::parse(preserved_input);
    preserved_input.close();
    EXPECT_EQ(preserved["avatar"], "avatar.svg");
    EXPECT_EQ(preserved["skills"], nlohmann::json::array({"skills"}));
    EXPECT_TRUE(preserved["x-forward-compatible"]["nested"]);
    EXPECT_EQ(preserved["capabilities"]["skills"][1], "missing-skill");
    EXPECT_EQ(preserved["capabilities"]["future_scope"][0], "keep");
    EXPECT_EQ(preserved["created_at"], created_at);
    EXPECT_NE(preserved["updated_at"].get<std::string>(), std::string{});
    EXPECT_TRUE(fs::is_regular_file(package / "avatar.svg"));
    EXPECT_TRUE(fs::is_regular_file(package / "resources" / "guide.txt"));
    EXPECT_TRUE(fs::is_regular_file(
        package / "skills" / "bundled" / "SKILL.md"));

    auto explicit_scope_update = legacy_client_update;
    explicit_scope_update.capabilities_present = true;
    explicit_scope_update.capabilities.skills =
        std::vector<std::string>{};
    explicit_scope_update.capabilities.tools =
        std::vector<std::string>{"file_read", "missing-tool"};
    ASSERT_TRUE(registry.update_global("preserved", explicit_scope_update,
                                       &error)) << error;

    auto found =
        registry.find(acecode::path_to_utf8(temp.path), "preserved");
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->capabilities.skills.has_value());
    EXPECT_TRUE(found->capabilities.skills->empty());
    EXPECT_FALSE(found->capabilities.mcp_servers.has_value());
    ASSERT_TRUE(found->capabilities.tools.has_value());
    EXPECT_EQ(*found->capabilities.tools,
              std::vector<std::string>({"file_read", "missing-tool"}));

    std::ifstream scoped_input(package / "expert.json");
    const auto scoped = nlohmann::json::parse(scoped_input);
    EXPECT_FALSE(scoped["capabilities"].contains("mcp_servers"));
    EXPECT_EQ(scoped["capabilities"]["future_scope"][0], "keep");
}

TEST(ExpertRegistry, RejectsDraftsThatCannotRoundTrip) {
    TempDir temp;
    acecode::ExpertRegistry registry(temp.path / "global");
    std::string error;

    auto blank_instructions = make_agent("blank-instructions", "Blank Instructions");
    blank_instructions.lead.instructions = " \n\t ";
    EXPECT_FALSE(registry.create_global(blank_instructions, &error));
    EXPECT_FALSE(fs::exists(temp.path / "global" / "blank-instructions"));

    auto oversized_metadata = make_agent("oversized-metadata", "Oversized Metadata");
    oversized_metadata.lead.display_name = std::string(513, 'x');
    EXPECT_FALSE(registry.create_global(oversized_metadata, &error));
    EXPECT_FALSE(fs::exists(temp.path / "global" / "oversized-metadata"));

    auto invalid_utf8 = make_agent("invalid-utf8", "Invalid UTF-8");
    invalid_utf8.lead.instructions = std::string(1, static_cast<char>(0xC3));
    EXPECT_FALSE(registry.create_global(invalid_utf8, &error));
    EXPECT_FALSE(fs::exists(temp.path / "global" / "invalid-utf8"));
}

TEST(ExpertRegistry, SessionCreationBindsKnownExpertAndRejectsUnknownExpert) {
    TempDir temp;
    const fs::path workspace = temp.path / "workspace";
    fs::create_directories(workspace);
    acecode::ExpertRegistry experts(temp.path / "global");
    std::string error;
    ASSERT_TRUE(experts.create_global(make_agent(), &error)) << error;

    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::SessionRegistryDeps deps;
    deps.provider_accessor = [] { return std::shared_ptr<acecode::LlmProvider>{}; };
    deps.tools = &tools;
    deps.cwd = acecode::path_to_utf8(workspace);
    deps.expert_registry = &experts;
    deps.template_permissions = &permissions;
    acecode::SessionRegistry sessions(std::move(deps));

    acecode::SessionOptions options;
    options.cwd = acecode::path_to_utf8(workspace);
    options.expert_id = "reviewer";
    const std::string id = sessions.create(options);
    auto entry = sessions.acquire(id);
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->expert.has_value());
    EXPECT_EQ(entry->expert_id, "reviewer");
    EXPECT_FALSE(entry->expert_missing);
    ASSERT_NE(entry->loop, nullptr);

    options.expert_id = "missing";
    EXPECT_THROW(sessions.create(options), std::invalid_argument);
    sessions.destroy(id);
}

TEST(ExpertRegistry, ActiveSessionSwitchQueuesAndPersistsExpertBinding) {
    TempDir temp;
    const fs::path workspace = temp.path / "workspace";
    fs::create_directories(workspace);
    acecode::ExpertRegistry experts(temp.path / "global");
    std::string error;
    ASSERT_TRUE(experts.create_global(make_agent("reviewer", "Reviewer"), &error))
        << error;
    ASSERT_TRUE(experts.create_global(make_agent("writer", "Writer"), &error))
        << error;

    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::SessionRegistryDeps deps;
    deps.provider_accessor = [] { return std::shared_ptr<acecode::LlmProvider>{}; };
    deps.tools = &tools;
    deps.cwd = acecode::path_to_utf8(workspace);
    deps.expert_registry = &experts;
    deps.template_permissions = &permissions;
    acecode::SessionRegistry sessions(std::move(deps));

    acecode::SessionOptions options;
    options.cwd = acecode::path_to_utf8(workspace);
    options.expert_id = "reviewer";
    const std::string id = sessions.create(options);
    auto entry = sessions.acquire(id);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->sm, nullptr);
    entry->sm->set_input_draft("persist expert switch");

    auto switched = sessions.switch_expert(id, "writer");
    ASSERT_EQ(switched.status, acecode::ExpertSwitchStatus::Accepted);
    ASSERT_TRUE(switched.expert.has_value());
    EXPECT_EQ(switched.expert->id, "writer");

    bool applied = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        auto active = sessions.list_active();
        if (active.size() == 1 && active.front().expert_id == "writer") {
            applied = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(applied);
    EXPECT_EQ(entry->sm->current_expert_id(), "writer");

    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(acecode::path_to_utf8(workspace));
    const auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(project_dir, id));
    EXPECT_EQ(meta.expert_id, "writer");

    auto missing = sessions.switch_expert(id, "missing");
    EXPECT_EQ(missing.status, acecode::ExpertSwitchStatus::UnknownExpert);
    sessions.destroy(id);
}

TEST(ExpertRegistry,
     BusySwitchChangesPromptSkillsAndToolScopesAtOneQueueBoundary) {
    TempDir temp;
    const fs::path workspace = temp.path / "workspace";
    fs::create_directories(workspace);
    auto write_skill = [&](const std::string& name) {
        const fs::path root =
            workspace / ".acecode" / "skills" / name;
        fs::create_directories(root);
        std::ofstream(root / "SKILL.md")
            << "---\nname: " << name
            << "\ndescription: " << name << " description\n---\n";
    };
    write_skill("review-skill");
    write_skill("write-skill");

    acecode::ExpertRegistry experts(temp.path / "global");
    std::string error;
    auto reviewer = make_agent("reviewer", "Reviewer");
    reviewer.capabilities.skills =
        std::vector<std::string>{"review-skill"};
    reviewer.capabilities.mcp_servers =
        std::vector<std::string>{"review-mcp"};
    reviewer.capabilities.tools =
        std::vector<std::string>{"file_read"};
    ASSERT_TRUE(experts.create_global(reviewer, &error)) << error;
    auto writer = make_agent("writer", "Writer");
    writer.lead.instructions = "Write the requested implementation.";
    writer.capabilities.skills =
        std::vector<std::string>{"write-skill"};
    writer.capabilities.mcp_servers =
        std::vector<std::string>{"write-mcp"};
    writer.capabilities.tools =
        std::vector<std::string>{"file_write"};
    ASSERT_TRUE(experts.create_global(writer, &error)) << error;

    std::atomic<int> tool_calls{0};
    acecode::ToolExecutor tools;
    auto register_tool = [&](std::string name, acecode::ToolSource source,
                             std::string owner, bool read_only) {
        acecode::ToolImpl tool;
        tool.definition.name = std::move(name);
        tool.definition.description = "scope probe";
        tool.definition.parameters = nlohmann::json::object();
        tool.source = source;
        tool.source_owner = std::move(owner);
        tool.is_read_only = read_only;
        tool.execute = [&](const std::string&, const acecode::ToolContext&) {
            ++tool_calls;
            return acecode::ToolResult{"ok", true};
        };
        tools.register_tool(tool);
    };
    register_tool("file_read", acecode::ToolSource::Builtin, {}, true);
    register_tool("file_write", acecode::ToolSource::Builtin, {}, false);
    register_tool("mcp_review_probe", acecode::ToolSource::Mcp,
                  "review-mcp", true);
    register_tool("mcp_write_probe", acecode::ToolSource::Mcp,
                  "write-mcp", true);

    auto provider =
        std::make_shared<acecode_test::StubLlmProvider>();
    provider->set_latency_ms(250);
    provider->push_text("first complete");
    provider->push_text("second complete");

    acecode::AppConfig cfg;
    cfg.skills.reuse_opencode = false;
    acecode::PermissionManager permissions;
    acecode::SessionRegistryDeps deps;
    deps.provider_accessor = [provider] {
        return std::static_pointer_cast<acecode::LlmProvider>(provider);
    };
    deps.tools = &tools;
    deps.cwd = acecode::path_to_utf8(workspace);
    deps.config = &cfg;
    deps.expert_registry = &experts;
    deps.template_permissions = &permissions;
    acecode::SessionRegistry sessions(std::move(deps));

    acecode::SessionOptions options;
    options.cwd = acecode::path_to_utf8(workspace);
    options.expert_id = "reviewer";
    const std::string id = sessions.create(options);
    auto entry = sessions.acquire(id);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->loop, nullptr);
    ASSERT_NE(entry->provider_slot, nullptr);
    {
        std::lock_guard<std::mutex> lock(entry->provider_slot->mu);
        entry->provider_slot->provider = provider;
    }

    entry->loop->submit("review first");
    const auto first_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (provider->turn_count() < 1 &&
           std::chrono::steady_clock::now() < first_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(provider->turn_count(), 1);
    ASSERT_TRUE(entry->loop->is_busy());

    const auto switched = sessions.switch_expert(id, "writer");
    ASSERT_EQ(switched.status, acecode::ExpertSwitchStatus::Accepted);
    EXPECT_TRUE(switched.busy);
    EXPECT_TRUE(switched.pending);
    EXPECT_EQ(switched.effective_boundary, "next_turn");
    EXPECT_EQ(entry->expert_id, "reviewer");

    // The control task is enqueued before this chat task, so the next request
    // must see all writer contexts together.
    entry->loop->submit("write second");
    const auto done_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((provider->turn_count() < 2 || entry->loop->is_busy()) &&
           std::chrono::steady_clock::now() < done_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(provider->turn_count(), 2);
    ASSERT_FALSE(entry->loop->is_busy());

    auto tool_names = [](const std::vector<acecode::ToolDef>& defs) {
        std::vector<std::string> result;
        for (const auto& def : defs) result.push_back(def.name);
        return result;
    };
    EXPECT_EQ(tool_names(provider->tools_for_turn(0)),
              std::vector<std::string>({
                  "file_read",
                  "mcp_review_probe",
              }));
    EXPECT_EQ(tool_names(provider->tools_for_turn(1)),
              std::vector<std::string>({
                  "file_write",
                  "mcp_write_probe",
              }));

    auto request_text = [](const std::vector<acecode::ChatMessage>& messages) {
        std::string result;
        for (const auto& message : messages) {
            result += message.content;
            result.push_back('\n');
        }
        return result;
    };
    const std::string first_request =
        request_text(provider->messages_for_turn(0));
    const std::string second_request =
        request_text(provider->messages_for_turn(1));
    EXPECT_NE(first_request.find("Review code carefully."),
              std::string::npos);
    EXPECT_NE(first_request.find("review-skill"), std::string::npos);
    EXPECT_EQ(first_request.find("write-skill"), std::string::npos);
    EXPECT_NE(second_request.find("Write the requested implementation."),
              std::string::npos);
    EXPECT_NE(second_request.find("write-skill"), std::string::npos);
    EXPECT_EQ(second_request.find("review-skill"), std::string::npos);

    EXPECT_EQ(entry->expert_id, "writer");
    ASSERT_NE(entry->skill_registry, nullptr);
    EXPECT_TRUE(entry->skill_registry->find("write-skill").has_value());
    EXPECT_FALSE(entry->skill_registry->find("review-skill").has_value());
    ASSERT_TRUE(entry->tool_capability_policy.builtin_tools.has_value());
    EXPECT_TRUE(entry->tool_capability_policy.builtin_tools->count(
                    "file_write") != 0);
    EXPECT_FALSE(entry->tool_capability_policy.builtin_tools->count(
                     "file_read") != 0);
    EXPECT_EQ(tool_calls.load(), 0);

    sessions.destroy(id);
}
