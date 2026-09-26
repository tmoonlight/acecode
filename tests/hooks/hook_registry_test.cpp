#include <gtest/gtest.h>

#include "hooks/hook_registry.hpp"
#include "skills/default_skill_seeder.hpp"
#include "test_support/repo_root.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct TempTree {
    fs::path root;

    TempTree() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() /
               ("acecode-hook-registry-test-" + std::to_string(stamp));
        fs::create_directories(root);
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs << text;
}

std::string read_text(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(ifs),
        std::istreambuf_iterator<char>());
}

fs::path packaged_hook_seed_path() {
    // 测试目录搬迁后仍读取仓库的种子；找不到根目录必须报错。
    const fs::path repository_root = acecode::test_support::find_repo_root(__FILE__);
    const auto& seed = acecode::default_hook_seeds().front();
    return repository_root / "assets" / "seed" / "hooks" /
        seed.relative_path / "hooks.json";
}

acecode::HookSource source_for(const std::string& path,
                               acecode::HookSourceScope scope = acecode::HookSourceScope::UserGlobal,
                               acecode::HookSourceFormat format = acecode::HookSourceFormat::CodexJson) {
    acecode::HookSource source;
    source.scope = scope;
    source.format = format;
    source.path = path;
    source.id = acecode::make_hook_source_id(scope, format, path);
    return source;
}

} // namespace

TEST(HookRegistry, StableIdsAndHashesIgnoreJsonKeyOrder) {
    auto a = nlohmann::json::parse(R"({
        "hooks": {
            "PreToolUse": [
                {
                    "matcher": "Bash",
                    "hooks": [
                        {
                            "type": "command",
                            "command": "node hook.js",
                            "commandWindows": "node hook-win.js",
                            "timeout": 10,
                            "statusMessage": "checking"
                        }
                    ]
                }
            ]
        }
    })");
    auto b = nlohmann::json::parse(R"({
        "hooks": {
            "PreToolUse": [
                {
                    "hooks": [
                        {
                            "statusMessage": "checking",
                            "timeout": 10,
                            "commandWindows": "node hook-win.js",
                            "command": "node hook.js",
                            "type": "command"
                        }
                    ],
                    "matcher": "Bash"
                }
            ]
        }
    })");

    auto source = source_for("C:/repo/.codex/hooks.json");
    auto ra = acecode::parse_codex_hooks_json_source(a, source);
    auto rb = acecode::parse_codex_hooks_json_source(b, source);

    ASSERT_EQ(ra.hooks.size(), 1u);
    ASSERT_EQ(rb.hooks.size(), 1u);
    EXPECT_EQ(ra.hooks[0].id, source.id + "::PreToolUse#1.1");
    EXPECT_EQ(ra.hooks[0].id, rb.hooks[0].id);
    EXPECT_EQ(ra.hooks[0].definition_hash, rb.hooks[0].definition_hash);
}

TEST(HookRegistry, ParsesCodexCommandFieldsAndDefaultTimeout) {
    auto j = nlohmann::json::parse(R"({
        "hooks": {
            "SessionStart": [
                {
                    "hooks": [
                        {
                            "type": "command",
                            "command": "python hook.py",
                            "command_windows": "py hook.py",
                            "statusMessage": "Loading hooks"
                        }
                    ]
                }
            ]
        }
    })");

    auto registry = acecode::parse_codex_hooks_json_source(
        j, source_for("/repo/.codex/hooks.json"));
    ASSERT_EQ(registry.hooks.size(), 1u);
    const auto& hook = registry.hooks[0];
    EXPECT_EQ(hook.event_name, "SessionStart");
    EXPECT_EQ(hook.matcher, "");
    EXPECT_EQ(hook.command.command, "python hook.py");
    EXPECT_EQ(hook.command.command_windows, "py hook.py");
    EXPECT_EQ(hook.command.timeout_seconds, 600);
    EXPECT_EQ(hook.command.status_message, "Loading hooks");
    EXPECT_FALSE(hook.skipped);
}

TEST(HookRegistry, DisabledCodexSourceRetainsDiagnosticWithoutHandlers) {
    auto config = nlohmann::json::parse(R"({
        "enabled": false,
        "hooks": {
            "PostToolUse": [
                {"matcher": "Bash", "hooks": [{"type": "command", "command": "user-hook"}]}
            ]
        }
    })");
    auto source = source_for("/repo/.codex/hooks.json");
    auto registry = acecode::parse_codex_hooks_json_source(config, source);
    acecode::apply_hook_trust_state(registry, acecode::HookTrustStore{}, true);

    EXPECT_TRUE(registry.feature_enabled);
    EXPECT_TRUE(registry.hooks.empty());
    ASSERT_EQ(registry.sources.size(), 1u);
    EXPECT_EQ(registry.sources[0].id, source.id);
    ASSERT_EQ(registry.sources[0].diagnostics.size(), 1u);
    EXPECT_EQ(registry.sources[0].diagnostics[0].code, "HOOK_SOURCE_DISABLED");
    ASSERT_EQ(registry.diagnostics.size(), 1u);
    EXPECT_EQ(registry.diagnostics[0].code, "HOOK_SOURCE_DISABLED");
    EXPECT_EQ(registry.diagnostics[0].severity, acecode::HookDiagnosticSeverity::Info);
    EXPECT_EQ(registry.diagnostics[0].source_id, source.id);
}

TEST(HookRegistry, MissingOrTrueSourceEnabledPreservesDefinitionsAndTrust) {
    auto config = nlohmann::json::parse(R"({
        "hooks": {
            "Stop": [
                {"hooks": [{"type": "command", "command": "user-hook"}]}
            ]
        }
    })");
    auto source = source_for("/repo/.codex/hooks.json");
    auto implicit = acecode::parse_codex_hooks_json_source(config, source);
    ASSERT_EQ(implicit.hooks.size(), 1u);
    acecode::HookTrustStore store;
    acecode::trust_hook_definition(store, implicit.hooks[0]);

    config["enabled"] = true;
    auto explicit_enabled = acecode::parse_codex_hooks_json_source(config, source);
    acecode::apply_hook_trust_state(explicit_enabled, store, true);

    ASSERT_EQ(explicit_enabled.hooks.size(), 1u);
    EXPECT_EQ(explicit_enabled.hooks[0].id, implicit.hooks[0].id);
    EXPECT_EQ(explicit_enabled.hooks[0].definition_hash, implicit.hooks[0].definition_hash);
    EXPECT_EQ(explicit_enabled.hooks[0].trust_status, acecode::HookTrustStatus::Trusted);
    EXPECT_TRUE(explicit_enabled.diagnostics.empty());
}

TEST(HookRegistry, DetectsBarePermissionResolvedObjectAsCodexHooks) {
    auto j = nlohmann::json::parse(R"({
        "PermissionResolved": [
            {
                "matcher": "Bash",
                "hooks": [
                    {"type": "command", "command": "python resolved.py"}
                ]
            }
        ]
    })");

    auto registry = acecode::parse_hook_source_json(
        j,
        source_for("/repo/.acecode/hooks.json",
                   acecode::HookSourceScope::UserGlobal,
                   acecode::HookSourceFormat::Unknown),
        true);

    ASSERT_EQ(registry.hooks.size(), 1u);
    EXPECT_EQ(registry.hooks[0].event_name, "PermissionResolved");
    EXPECT_EQ(registry.hooks[0].matcher, "Bash");
    EXPECT_EQ(registry.hooks[0].command.command, "python resolved.py");
}

TEST(HookRegistry, DetectsBareSessionTitleChangedObjectAsCodexHooks) {
    auto j = nlohmann::json::parse(R"({
        "SessionTitleChanged": [
            {
                "matcher": "resume",
                "hooks": [
                    {"type": "command", "command": "python title.py"}
                ]
            }
        ]
    })");

    auto registry = acecode::parse_hook_source_json(
        j,
        source_for("/repo/.acecode/hooks.json",
                   acecode::HookSourceScope::UserGlobal,
                   acecode::HookSourceFormat::Unknown),
        true);

    ASSERT_EQ(registry.hooks.size(), 1u);
    EXPECT_EQ(registry.hooks[0].event_name, "SessionTitleChanged");
    EXPECT_EQ(registry.hooks[0].matcher, "resume");
    EXPECT_EQ(registry.hooks[0].command.command, "python title.py");
}

TEST(HookRegistry, ParsesUnsupportedAndAsyncHandlersAsSkipped) {
    auto j = nlohmann::json::parse(R"({
        "hooks": {
            "Stop": [
                {
                    "matcher": "*",
                    "hooks": [
                        {"type": "prompt", "prompt": "x"},
                        {"type": "agent", "name": "x"},
                        {"type": "command", "command": "echo async", "async": true},
                        {"type": "weird"}
                    ]
                }
            ]
        }
    })");

    auto registry = acecode::parse_codex_hooks_json_source(
        j, source_for("/repo/.codex/hooks.json"));
    ASSERT_EQ(registry.hooks.size(), 4u);
    EXPECT_EQ(registry.hooks[0].kind, acecode::HookHandlerKind::UnsupportedPrompt);
    EXPECT_EQ(registry.hooks[1].kind, acecode::HookHandlerKind::UnsupportedAgent);
    EXPECT_EQ(registry.hooks[2].kind, acecode::HookHandlerKind::Command);
    EXPECT_EQ(registry.hooks[3].kind, acecode::HookHandlerKind::Unknown);
    for (const auto& hook : registry.hooks) {
        EXPECT_TRUE(hook.skipped);
        EXPECT_FALSE(hook.skip_reason.empty());
    }
    EXPECT_GE(registry.diagnostics.size(), 4u);
}

TEST(HookRegistry, KeepsLegacyDirectCommandSemantics) {
    auto j = nlohmann::json::parse(R"({
        "enabled": true,
        "events": {
            "startup.models_loaded": [
                {
                    "id": "legacy-startup",
                    "mode": "async",
                    "command": "python",
                    "args": ["hook.py"],
                    "timeout_ms": 1234
                }
            ]
        }
    })");

    auto registry = acecode::parse_hook_source_json(
        j,
        source_for("C:/Users/me/.acecode/hooks.json",
                   acecode::HookSourceScope::Legacy,
                   acecode::HookSourceFormat::Unknown),
        true);

    ASSERT_EQ(registry.hooks.size(), 1u);
    const auto& hook = registry.hooks[0];
    EXPECT_TRUE(hook.legacy_direct);
    EXPECT_EQ(hook.id.find("legacy-startup"), hook.id.size() - std::string("legacy-startup").size());
    EXPECT_EQ(hook.legacy.definition.command.command, "python");
    ASSERT_EQ(hook.legacy.definition.command.args.size(), 1u);
    EXPECT_EQ(hook.legacy.definition.command.args[0], "hook.py");
    EXPECT_EQ(hook.legacy.definition.mode, acecode::HookMode::Async);
    EXPECT_EQ(hook.legacy.definition.timeout_ms, 1234);
    EXPECT_EQ(hook.trust_status, acecode::HookTrustStatus::Trusted);
}

TEST(HookRegistry, DisabledLegacyConfigStillSurfacesHooksAsDisabled) {
    auto j = nlohmann::json::parse(R"({
        "enabled": false,
        "events": {
            "startup.before_model_load": [
                {
                    "id": "legacy-startup",
                    "command": "node",
                    "args": ["hook.js"]
                }
            ]
        }
    })");

    auto registry = acecode::parse_hook_source_json(
        j,
        source_for("C:/Users/me/.acecode/hooks.json",
                   acecode::HookSourceScope::Legacy,
                   acecode::HookSourceFormat::Unknown),
        true);
    acecode::HookTrustStore store;
    acecode::apply_hook_trust_state(registry, store, true);

    ASSERT_EQ(registry.hooks.size(), 1u);
    EXPECT_EQ(registry.hooks[0].trust_status, acecode::HookTrustStatus::Disabled);
    bool saw_disabled_diag = false;
    for (const auto& d : registry.diagnostics) {
        if (d.code == "LEGACY_DISABLED") saw_disabled_diag = true;
    }
    EXPECT_TRUE(saw_disabled_diag);
}

TEST(HookRegistry, SourceDiscoveryMergesGlobalAndTrustedProjectSources) {
    TempTree tmp;
    const fs::path ace_home = tmp.root / "ace-home";
    const fs::path codex_home = tmp.root / "codex-home";
    const fs::path project = tmp.root / "project";

    write_text(ace_home / "hooks.json", R"({
        "enabled": true,
        "events": {
            "startup.before_model_load": [
                {"id": "legacy", "command": "legacy-hook"}
            ]
        }
    })");
    write_text(codex_home / "hooks.json", R"({
        "hooks": {
            "SessionStart": [
                {"hooks": [{"type": "command", "command": "global-codex"}]}
            ]
        }
    })");
    write_text(project / ".acecode" / "hooks.json", R"({
        "hooks": {
            "PreToolUse": [
                {"matcher": "Bash", "hooks": [{"type": "command", "command": "project-ace"}]}
            ]
        }
    })");
    write_text(project / ".codex" / "hooks.json", R"({
        "hooks": {
            "Stop": [
                {"hooks": [{"type": "command", "command": "project-codex"}]}
            ]
        }
    })");

    acecode::HookLoadOptions opts;
    opts.acecode_home = acecode::path_to_utf8(ace_home);
    opts.codex_home = acecode::path_to_utf8(codex_home);
    opts.cwd = acecode::path_to_utf8(project);
    opts.project_trusted = true;
    auto trusted = acecode::load_hook_registry(opts);
    EXPECT_EQ(trusted.hooks.size(), 4u);
    EXPECT_EQ(trusted.sources.size(), 4u);

    opts.project_trusted = false;
    auto untrusted = acecode::load_hook_registry(opts);
    EXPECT_EQ(untrusted.hooks.size(), 2u);
    bool saw_skip = false;
    for (const auto& d : untrusted.diagnostics) {
        if (d.code == "PROJECT_HOOKS_UNTRUSTED") saw_skip = true;
    }
    EXPECT_TRUE(saw_skip);
}

TEST(HookRegistry, SourceDiscoveryReportsMalformedSources) {
    TempTree tmp;
    const fs::path ace_home = tmp.root / "ace-home";
    write_text(ace_home / "hooks.json", "{not-json");

    acecode::HookLoadOptions opts;
    opts.acecode_home = acecode::path_to_utf8(ace_home);
    opts.codex_home = acecode::path_to_utf8(tmp.root / "missing-codex");
    opts.cwd = acecode::path_to_utf8(tmp.root / "project");
    auto registry = acecode::load_hook_registry(opts);

    EXPECT_TRUE(registry.hooks.empty());
    bool saw_parse = false;
    bool saw_missing = false;
    for (const auto& d : registry.diagnostics) {
        if (d.code == "HOOK_SOURCE_PARSE_FAILED") saw_parse = true;
        if (d.code == "HOOK_SOURCE_MISSING") saw_missing = true;
    }
    EXPECT_TRUE(saw_parse);
    EXPECT_TRUE(saw_missing);
}

TEST(HookRegistry, DisabledOfficialSeedPreservesUserConfigurationAndTrust) {
    TempTree tmp;
    const fs::path ace_home = tmp.root / "ace-home";
    const auto& seed = acecode::default_hook_seeds().front();
    const fs::path managed_path =
        ace_home / "hooks" / seed.relative_path / "hooks.json";
    write_text(managed_path, read_text(packaged_hook_seed_path()));

    const fs::path user_path = ace_home / "hooks.json";
    const std::string user_config = R"({
        "hooks": {
            "Stop": [
                {"hooks": [{"type": "command", "command": "user-hook"}]}
            ]
        }
    })";
    write_text(user_path, user_config);

    acecode::HookLoadOptions opts;
    opts.acecode_home = acecode::path_to_utf8(ace_home);
    opts.codex_home = acecode::path_to_utf8(tmp.root / "missing-codex");
    opts.include_project_sources = false;

    const auto first = acecode::load_hook_registry(opts);
    const auto second = acecode::load_hook_registry(opts);

    EXPECT_EQ(read_text(user_path), user_config);
    ASSERT_EQ(first.hooks.size(), 1u);
    ASSERT_EQ(second.hooks.size(), first.hooks.size());
    EXPECT_EQ(first.hooks[0].id, second.hooks[0].id);
    EXPECT_FALSE(first.hooks[0].managed);
    EXPECT_EQ(first.hooks[0].command.command, "user-hook");
    EXPECT_EQ(first.hooks[0].trust_status, acecode::HookTrustStatus::PendingReview);
    ASSERT_EQ(first.sources.size(), 2u);
    const auto& managed_source = first.sources[0];
    EXPECT_TRUE(managed_source.managed);
    EXPECT_EQ(acecode::path_from_utf8(managed_source.path), managed_path);
    ASSERT_EQ(managed_source.diagnostics.size(), 1u);
    EXPECT_EQ(managed_source.diagnostics[0].code, "HOOK_SOURCE_DISABLED");

    acecode::HookTrustStore store;
    acecode::trust_hook_definition(store, first.hooks[0]);
    const auto trusted = acecode::load_hook_registry(opts, &store);
    ASSERT_EQ(trusted.hooks.size(), 1u);
    EXPECT_EQ(trusted.hooks[0].trust_status, acecode::HookTrustStatus::Trusted);

    acecode::set_hook_disabled(store, trusted.hooks[0], true);
    const auto disabled = acecode::load_hook_registry(opts, &store);
    ASSERT_EQ(disabled.hooks.size(), 1u);
    EXPECT_EQ(disabled.hooks[0].trust_status, acecode::HookTrustStatus::Disabled);
    EXPECT_EQ(read_text(user_path), user_config);
}

TEST(HookRegistry, ModifiedSeedHookIsPreservedButNotAutomaticallyTrusted) {
    TempTree tmp;
    const fs::path ace_home = tmp.root / "ace-home";
    const auto& seed = acecode::default_hook_seeds().front();
    const fs::path managed_path =
        ace_home / "hooks" / seed.relative_path / "hooks.json";
    auto modified = read_text(packaged_hook_seed_path());
    const auto command_pos = modified.find("--state idle");
    ASSERT_NE(command_pos, std::string::npos);
    modified.replace(command_pos, std::string("--state idle").size(),
                     "--state blocked");
    write_text(managed_path, modified);

    acecode::HookLoadOptions opts;
    opts.acecode_home = acecode::path_to_utf8(ace_home);
    opts.codex_home = acecode::path_to_utf8(tmp.root / "missing-codex");
    opts.include_project_sources = false;
    const auto registry = acecode::load_hook_registry(opts);

    EXPECT_EQ(read_text(managed_path), modified);
    EXPECT_TRUE(registry.hooks.empty());
    bool saw_mismatch = false;
    for (const auto& d : registry.diagnostics) {
        if (d.code == "MANAGED_SEED_HOOK_DEFINITION_MISMATCH") {
            saw_mismatch = true;
        }
    }
    EXPECT_TRUE(saw_mismatch);
}

TEST(HookRegistry, TrustStoreHandlesPendingTrustedChangedDisabledAndManaged) {
    auto j = nlohmann::json::parse(R"({
        "hooks": {
            "PreToolUse": [
                {"matcher": "Bash", "hooks": [{"type": "command", "command": "echo one"}]}
            ]
        }
    })");
    auto source = source_for("/repo/.codex/hooks.json");
    auto registry = acecode::parse_codex_hooks_json_source(j, source);
    ASSERT_EQ(registry.hooks.size(), 1u);

    acecode::HookTrustStore store;
    acecode::apply_hook_trust_state(registry, store, true);
    EXPECT_EQ(registry.hooks[0].trust_status, acecode::HookTrustStatus::PendingReview);

    acecode::trust_hook_definition(store, registry.hooks[0]);
    acecode::apply_hook_trust_state(registry, store, true);
    EXPECT_EQ(registry.hooks[0].trust_status, acecode::HookTrustStatus::Trusted);

    auto changed_json = nlohmann::json::parse(R"({
        "hooks": {
            "PreToolUse": [
                {"matcher": "Bash", "hooks": [{"type": "command", "command": "echo two"}]}
            ]
        }
    })");
    auto changed = acecode::parse_codex_hooks_json_source(changed_json, source);
    acecode::apply_hook_trust_state(changed, store, true);
    ASSERT_EQ(changed.hooks.size(), 1u);
    EXPECT_EQ(changed.hooks[0].trust_status, acecode::HookTrustStatus::PendingReview);

    acecode::set_hook_disabled(store, registry.hooks[0], true);
    acecode::apply_hook_trust_state(registry, store, true);
    EXPECT_EQ(registry.hooks[0].trust_status, acecode::HookTrustStatus::Disabled);

    auto managed_source = source_for("/managed/hooks.json",
                                     acecode::HookSourceScope::Managed,
                                     acecode::HookSourceFormat::CodexJson);
    managed_source.managed = true;
    auto managed = acecode::parse_codex_hooks_json_source(j, managed_source);
    acecode::apply_hook_trust_state(managed, store, true);
    ASSERT_EQ(managed.hooks.size(), 1u);
    EXPECT_EQ(managed.hooks[0].trust_status, acecode::HookTrustStatus::ManagedTrusted);
}

TEST(HookRegistry, TrustStorePersistsAtomicallyEnoughForFailureRollback) {
    TempTree tmp;
    const auto path = tmp.root / "hooks_state.json";

    acecode::HookTrustStore store;
    store.trusted.push_back({"source", "hook", "hash-a"});
    std::string error;
    ASSERT_TRUE(acecode::save_hook_trust_store_to_path(
        store, acecode::path_to_utf8(path), &error)) << error;

    auto loaded = acecode::load_hook_trust_store_from_path(acecode::path_to_utf8(path), &error);
    ASSERT_EQ(loaded.trusted.size(), 1u);
    EXPECT_EQ(loaded.trusted[0].definition_hash, "hash-a");

    fs::path bad_target = tmp.root / "bad-dir";
    fs::create_directories(bad_target);
    acecode::HookTrustStore changed;
    changed.trusted.push_back({"source", "hook", "hash-b"});
    EXPECT_FALSE(acecode::save_hook_trust_store_to_path(
        changed, acecode::path_to_utf8(bad_target), &error));

    loaded = acecode::load_hook_trust_store_from_path(acecode::path_to_utf8(path), &error);
    ASSERT_EQ(loaded.trusted.size(), 1u);
    EXPECT_EQ(loaded.trusted[0].definition_hash, "hash-a");
}
