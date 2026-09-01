#include "config/config_recovery.hpp"
#include "config/config.hpp"
#include "utils/logger.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

namespace fs = std::filesystem;

class TempRecoveryConfig {
public:
    TempRecoveryConfig() {
        static std::atomic<unsigned long long> sequence{0};
        root = fs::temp_directory_path() /
            ("acecode-config-recovery-test-" +
             std::to_string(sequence.fetch_add(1)));
        fs::create_directories(root);
        config = root / "config.json";
        acecode::Logger::instance().init(
            (root / "config-recovery-test.log").string());
    }

    ~TempRecoveryConfig() {
        acecode::Logger::instance().init("");
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path root;
    fs::path config;
};

std::string read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}

void write_bytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good());
}

} // namespace

TEST(ConfigRecoveryPersistence, PathsStayBesideExplicitConfig) {
    TempRecoveryConfig temp;
    const auto paths = acecode::config_recovery_paths(temp.config.string());

    EXPECT_EQ(fs::path(paths.root_dir), temp.root / "config-backups");
    EXPECT_EQ(fs::path(paths.last_good_path),
              temp.root / "config-backups" / "last-good" / "config.json");
    EXPECT_EQ(fs::path(paths.invalid_dir),
              temp.root / "config-backups" / "invalid");
    EXPECT_EQ(fs::path(paths.staging_dir),
              temp.root / "config-backups" / "staging");
    EXPECT_EQ(fs::path(paths.notice_path),
              temp.root / "config-backups" / "recovery-notice.json");
}

TEST(ConfigRecoveryPersistence, SnapshotAndArchivesPreserveExactBytes) {
    TempRecoveryConfig temp;
    const std::string valid = "{\r\n  \"api_key\": \"secret\"\r\n}\r\n";
    const std::string invalid = "{\n  \"api_key\": \"secret\",\n";
    std::string error;

    ASSERT_TRUE(acecode::write_last_good_config(
        temp.config.string(), valid, &error)) << error;
    auto snapshot = acecode::read_last_good_config(
        temp.config.string(), &error);
    ASSERT_TRUE(snapshot.has_value()) << error;
    EXPECT_EQ(*snapshot, valid);

    auto first = acecode::archive_invalid_config(
        temp.config.string(), invalid, &error);
    auto second = acecode::archive_invalid_config(
        temp.config.string(), invalid + " ", &error);
    ASSERT_TRUE(first.has_value()) << error;
    ASSERT_TRUE(second.has_value()) << error;
    EXPECT_NE(*first, *second);
    EXPECT_EQ(read_bytes(*first), invalid);
    EXPECT_EQ(read_bytes(*second), invalid + " ");

    auto staged = acecode::stage_config_recovery_candidate(
        temp.config.string(), valid, &error);
    ASSERT_TRUE(staged.has_value()) << error;
    EXPECT_EQ(read_bytes(*staged), valid);
}

TEST(ConfigRecoveryPersistence, NoticeReadAndAcknowledgeAreDurableAndIdempotent) {
    TempRecoveryConfig temp;
    const fs::path invalid = temp.root / "config-backups" / "invalid" /
        "config-invalid.json";
    std::string error;

    ASSERT_TRUE(acecode::write_config_recovery_notice(
        temp.config.string(), invalid.string(), &error)) << error;
    const auto pending = acecode::read_config_recovery_notice(
        temp.config.string());
    EXPECT_TRUE(pending.pending);
    EXPECT_EQ(fs::path(pending.config_path), temp.config);
    EXPECT_EQ(fs::path(pending.invalid_backup_path), invalid);
    EXPECT_EQ(fs::path(pending.invalid_backup_dir), invalid.parent_path());
    EXPECT_GT(pending.recovered_at_ms, 0);

    ASSERT_TRUE(acecode::acknowledge_config_recovery_notice(
        temp.config.string(), &error)) << error;
    EXPECT_FALSE(acecode::read_config_recovery_notice(
        temp.config.string()).pending);
    EXPECT_TRUE(acecode::acknowledge_config_recovery_notice(
        temp.config.string(), &error)) << error;
}

TEST(ConfigRecoveryPersistence, PersistenceFailuresAreReported) {
    TempRecoveryConfig temp;
    const auto paths = acecode::config_recovery_paths(temp.config.string());
    {
        std::ofstream blocker(paths.root_dir, std::ios::binary);
        blocker << "not a directory";
    }

    std::string error;
    EXPECT_FALSE(acecode::write_last_good_config(
        temp.config.string(), "{}\n", &error));
    EXPECT_NE(error.find("last-good"), std::string::npos);
    EXPECT_FALSE(acecode::archive_invalid_config(
        temp.config.string(), "{", &error).has_value());
    EXPECT_NE(error.find("recovery directory"), std::string::npos);
}

TEST(ConfigRecoveryPersistence, AcknowledgeRejectsDirectoryNoticeTarget) {
    TempRecoveryConfig temp;
    const auto paths = acecode::config_recovery_paths(temp.config.string());
    fs::create_directories(paths.notice_path);

    std::string error;
    EXPECT_FALSE(acecode::acknowledge_config_recovery_notice(
        temp.config.string(), &error));
    EXPECT_NE(error.find("not a regular file"), std::string::npos);
}

TEST(ConfigRecoveryLoad, MalformedJsonRestoresSnapshotAndArchivesExactBytes) {
    TempRecoveryConfig temp;
    const fs::path log_path = temp.root / "recovery.log";
    acecode::Logger::instance().init(log_path.string());
    acecode::Logger::instance().set_level(acecode::LogLevel::Dbg);

    const std::string valid = "{\r\n  \"max_sessions\": 37\r\n}\r\n";
    write_bytes(temp.config, valid);
    const auto seeded = acecode::load_config_from_path(temp.config.string());
    EXPECT_EQ(seeded.max_sessions, 37);

    const std::string invalid =
        "{\n  \"api_key\": \"super-secret-must-not-log\",\n";
    write_bytes(temp.config, invalid);
#if GTEST_HAS_STREAM_REDIRECTION
    testing::internal::CaptureStderr();
#endif
    const auto recovered = acecode::load_config_from_path(temp.config.string());
#if GTEST_HAS_STREAM_REDIRECTION
    EXPECT_TRUE(testing::internal::GetCapturedStderr().empty());
#endif
    EXPECT_EQ(recovered.max_sessions, 37);
    EXPECT_EQ(read_bytes(temp.config), valid);

    const auto notice = acecode::read_config_recovery_notice(
        temp.config.string());
    ASSERT_TRUE(notice.pending);
    ASSERT_FALSE(notice.invalid_backup_path.empty());
    EXPECT_EQ(read_bytes(notice.invalid_backup_path), invalid);
    EXPECT_EQ(fs::path(notice.invalid_backup_dir),
              temp.root / "config-backups" / "invalid");

    const std::string logs = read_bytes(log_path);
    EXPECT_NE(logs.find("active config rejected"), std::string::npos);
    EXPECT_NE(logs.find("restored last-good config"), std::string::npos);
    EXPECT_NE(logs.find("invalid_backup"), std::string::npos);
    EXPECT_EQ(logs.find("super-secret-must-not-log"), std::string::npos);
}

TEST(ConfigRecoveryLoad, FullSemanticValidationUsesSameRollbackPath) {
    TempRecoveryConfig temp;
    write_bytes(temp.config, "{\n  \"max_sessions\": 41\n}\n");
    ASSERT_EQ(
        acecode::load_config_from_path(temp.config.string()).max_sessions,
        41);

    const std::string invalid =
        "{\n  \"web\": {\"port\": 70000},\n  \"max_sessions\": 99\n}\n";
    write_bytes(temp.config, invalid);
    const auto recovered = acecode::load_config_from_path(temp.config.string());
    EXPECT_EQ(recovered.max_sessions, 41);
    EXPECT_NE(recovered.web.port, 70000);

    const auto notice = acecode::read_config_recovery_notice(
        temp.config.string());
    ASSERT_TRUE(notice.pending);
    EXPECT_EQ(read_bytes(notice.invalid_backup_path), invalid);
}

TEST(ConfigRecoveryLoad, SupportedMissingDefaultRepairDoesNotCreateNotice) {
    TempRecoveryConfig temp;
    const nlohmann::json source = {
        {"default_model_name", "removed-model"},
        {"saved_models", nlohmann::json::array({{
            {"name", "available-model"},
            {"provider", "openai"},
            {"base_url", "https://example.test/v1"},
            {"api_key", "secret"},
            {"model", "runtime-model"},
        }})},
    };
    write_bytes(temp.config, source.dump(2) + "\n");

    const auto loaded = acecode::load_config_from_path(temp.config.string());
    EXPECT_EQ(loaded.default_model_name, "available-model");
    EXPECT_FALSE(acecode::read_config_recovery_notice(
        temp.config.string()).pending);
    const auto paths = acecode::config_recovery_paths(temp.config.string());
    EXPECT_FALSE(fs::exists(paths.invalid_dir));

    const auto snapshot = nlohmann::json::parse(
        read_bytes(paths.last_good_path));
    EXPECT_EQ(snapshot["default_model_name"], "available-model");
}

TEST(ConfigRecoveryLoad, FailedTargetedRepairNeverAdvancesStaleSnapshot) {
    TempRecoveryConfig temp;
    const nlohmann::json source = {
        {"default_model_name", "removed-model"},
        {"saved_models", nlohmann::json::array({{
            {"name", "available-model"},
            {"provider", "copilot"},
            {"model", "runtime-model"},
        }})},
    };
    const std::string original_bytes = source.dump(2) + "\n";
    write_bytes(temp.config, original_bytes);

    // atomic_write_file uses a sibling .tmp path. A directory at that exact
    // path deterministically simulates persistence failure without making the
    // active config unreadable.
    const fs::path blocked_tmp = temp.config.string() + ".tmp";
    ASSERT_TRUE(fs::create_directory(blocked_tmp));

    const auto in_memory = acecode::load_config_from_path(temp.config.string());
    EXPECT_EQ(in_memory.default_model_name, "available-model");
    EXPECT_EQ(read_bytes(temp.config), original_bytes);
    const auto paths = acecode::config_recovery_paths(temp.config.string());
    EXPECT_FALSE(fs::exists(paths.last_good_path));
    EXPECT_FALSE(acecode::read_config_recovery_notice(
        temp.config.string()).pending);

    ASSERT_TRUE(fs::remove(blocked_tmp));
    const auto retried = acecode::load_config_from_path(temp.config.string());
    EXPECT_EQ(retried.default_model_name, "available-model");
    const auto repaired = nlohmann::json::parse(read_bytes(temp.config));
    EXPECT_EQ(repaired["default_model_name"], "available-model");
    EXPECT_EQ(read_bytes(paths.last_good_path), read_bytes(temp.config));
}

#if GTEST_HAS_DEATH_TEST
TEST(ConfigRecoveryLoad, MissingSnapshotPreservesInvalidActiveFileAndExits) {
    TempRecoveryConfig temp;
    const std::string invalid = "{\n  \"secret\": \"do-not-print\",\n";
    write_bytes(temp.config, invalid);

    EXPECT_EXIT(
        (void)acecode::load_config_from_path(temp.config.string()),
        testing::ExitedWithCode(1),
        "automatic rollback failed: no readable last-good snapshot");
    EXPECT_EQ(read_bytes(temp.config), invalid);
}

TEST(ConfigRecoveryLoad, InvalidSnapshotDoesNotReplaceInvalidActiveFile) {
    TempRecoveryConfig temp;
    const std::string active = "{\n  \"active\": true,\n";
    write_bytes(temp.config, active);
    std::string error;
    ASSERT_TRUE(acecode::write_last_good_config(
        temp.config.string(), "{\n  \"snapshot\": true,\n", &error)) << error;

    EXPECT_EXIT(
        (void)acecode::load_config_from_path(temp.config.string()),
        testing::ExitedWithCode(1),
        "last-good snapshot rejected");
    EXPECT_EQ(read_bytes(temp.config), active);
}
#endif
