#include "feedback/feedback_upload.hpp"

#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>
#include <httplib.h>
#include <zip.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path root;

    explicit TempDir(const std::string& name) {
        root = fs::temp_directory_path() /
               (name + "_" + std::to_string(std::chrono::steady_clock::now()
                                                 .time_since_epoch()
                                                 .count()));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << text;
}

std::string read_zip_entry(const fs::path& zip_path, const std::string& entry) {
    int err = 0;
    zip_t* archive = zip_open(acecode::path_to_utf8(zip_path).c_str(), ZIP_RDONLY, &err);
    if (!archive) return {};
    zip_int64_t index = zip_name_locate(archive, entry.c_str(), ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_close(archive);
        return {};
    }
    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat_index(archive, static_cast<zip_uint64_t>(index), 0, &st) != 0) {
        zip_close(archive);
        return {};
    }
    zip_file_t* file = zip_fopen_index(archive, static_cast<zip_uint64_t>(index), 0);
    if (!file) {
        zip_close(archive);
        return {};
    }
    std::string out(static_cast<std::size_t>(st.size), '\0');
    zip_int64_t read = zip_fread(file, out.data(), out.size());
    zip_fclose(file);
    zip_close(archive);
    if (read < 0) return {};
    out.resize(static_cast<std::size_t>(read));
    return out;
}

bool zip_entry_exists(const fs::path& zip_path, const std::string& entry) {
    int err = 0;
    zip_t* archive = zip_open(acecode::path_to_utf8(zip_path).c_str(), ZIP_RDONLY, &err);
    if (!archive) return false;
    const bool exists =
        zip_name_locate(archive, entry.c_str(), ZIP_FL_ENC_UTF_8) >= 0;
    zip_close(archive);
    return exists;
}

struct LocalHttpServer {
    httplib::Server svr;
    int port = 0;
    std::thread th;

    explicit LocalHttpServer(std::function<void(httplib::Server&)> setup) {
        setup(svr);
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }

    ~LocalHttpServer() {
        svr.stop();
        if (th.joinable()) th.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/";
    }
};

} // namespace

TEST(FeedbackUpload, BuildPackageIncludesSessionMetadataAndLogTail) {
    TempDir tmp("acecode_feedback_package");
    const fs::path session = tmp.root / "session.jsonl";
    const fs::path log = tmp.root / "acecode.log";
    write_text(session, "{\"role\":\"user\",\"content\":\"hello\"}\n");
    write_text(log, "0123456789abcdefghijklmnopqrstuvwxyz");

    acecode::feedback::FeedbackPackageRequest req;
    req.feedback_text = "third turn froze";
    req.session_id = "20260618-010203-abcd";
    req.session_jsonl_path = session;
    req.logs.push_back({log, "logs/acecode.log.tail.txt", 0});
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";
    req.acecode_version = "test-version";
    req.platform = "windows-x64";
    req.computer_name = "QA BOX";
    req.login_name = "alice@example";
    req.max_log_bytes = 16;

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(fs::is_regular_file(result.package_path));
    EXPECT_EQ(result.package_filename,
              "acecode-feedback-20260618-010203-abcd-20260618-010203-windows-x64-QA-BOX-alice-example.zip");
    EXPECT_TRUE(result.log_included);
    EXPECT_EQ(result.log_tail_bytes, 16u);

    EXPECT_EQ(read_zip_entry(result.package_path,
                             "session/20260618-010203-abcd.jsonl"),
              "{\"role\":\"user\",\"content\":\"hello\"}\n");
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/acecode.log.tail.txt"),
              "klmnopqrstuvwxyz");

    auto metadata = nlohmann::json::parse(
        read_zip_entry(result.package_path, "feedback.json"));
    EXPECT_EQ(metadata["feedback_text"], "third turn froze");
    EXPECT_EQ(metadata["source"], "tui");
    EXPECT_EQ(metadata["session_id"], "20260618-010203-abcd");
    EXPECT_EQ(metadata["selected_session_id"], "20260618-010203-abcd");
    EXPECT_EQ(metadata["acecode_version"], "test-version");
    EXPECT_EQ(metadata["platform"], "windows-x64");
    EXPECT_EQ(metadata["computer_name"], "QA BOX");
    EXPECT_EQ(metadata["login_name"], "alice@example");
    EXPECT_TRUE(metadata["log_available"].get<bool>());
    EXPECT_EQ(metadata["log_tail_bytes"], 16);
}

TEST(FeedbackUpload, BuildDesktopPackageCanBeLogOnly) {
    TempDir tmp("acecode_feedback_desktop_log_only");
    const fs::path log = tmp.root / "desktop-2026-06-18.log";
    write_text(log, "desktop log line 1\ndesktop log line 2\n");

    acecode::feedback::FeedbackPackageRequest req;
    req.source = "desktop";
    req.feedback_text = "settings pane froze";
    req.logs.push_back({log, "logs/desktop.log.tail.txt", 0});
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";
    req.acecode_version = "test-version";
    req.platform = "windows-x64";
    req.computer_name = "Desk";
    req.login_name = "tester";
    req.max_log_bytes = 19;

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(fs::is_regular_file(result.package_path));
    EXPECT_EQ(result.package_filename,
              "acecode-feedback-desktop-20260618-010203-windows-x64-Desk-tester.zip");
    EXPECT_TRUE(result.log_included);
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/desktop.log.tail.txt"),
              "desktop log line 2\n");
    EXPECT_FALSE(zip_entry_exists(result.package_path, "session/session.jsonl"));

    auto metadata = nlohmann::json::parse(
        read_zip_entry(result.package_path, "feedback.json"));
    EXPECT_EQ(metadata["source"], "desktop");
    EXPECT_TRUE(metadata["session_id"].is_null());
    EXPECT_TRUE(metadata["selected_session_id"].is_null());
    EXPECT_EQ(metadata["included_files"].size(), 2u);
    EXPECT_EQ(metadata["included_files"][0], "logs/desktop.log.tail.txt");
    EXPECT_EQ(metadata["included_files"][1], "feedback.json");
}

TEST(FeedbackUpload, BuildDesktopPackageMayIncludeSelectedSession) {
    TempDir tmp("acecode_feedback_desktop_selected_session");
    const fs::path session = tmp.root / "selected.jsonl";
    const fs::path log = tmp.root / "desktop-2026-06-18.log";
    write_text(session, "{\"role\":\"user\",\"content\":\"selected\"}\n");
    write_text(log, "desktop log");

    acecode::feedback::FeedbackPackageRequest req;
    req.source = "desktop";
    req.feedback_text = "selected context";
    req.session_id = "sid-selected";
    req.session_jsonl_path = session;
    req.workspace_hash = "workspace-a";
    req.logs.push_back({log, "logs/desktop.log.tail.txt", 0});
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(read_zip_entry(result.package_path, "session/sid-selected.jsonl"),
              "{\"role\":\"user\",\"content\":\"selected\"}\n");
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/desktop.log.tail.txt"),
              "desktop log");

    auto metadata = nlohmann::json::parse(
        read_zip_entry(result.package_path, "feedback.json"));
    EXPECT_EQ(metadata["source"], "desktop");
    EXPECT_EQ(metadata["session_id"], "sid-selected");
    EXPECT_EQ(metadata["selected_session_id"], "sid-selected");
    EXPECT_EQ(metadata["workspace_hash"], "workspace-a");
}

TEST(FeedbackUpload, BuildDesktopPackageSucceedsWhenLogIsMissing) {
    TempDir tmp("acecode_feedback_desktop_missing_log");

    acecode::feedback::FeedbackPackageRequest req;
    req.source = "desktop";
    req.logs.push_back({tmp.root / "missing.log", "logs/desktop.log.tail.txt", 0});
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.log_included);
    EXPECT_FALSE(zip_entry_exists(result.package_path, "logs/desktop.log.tail.txt"));
    auto metadata = nlohmann::json::parse(
        read_zip_entry(result.package_path, "feedback.json"));
    EXPECT_FALSE(metadata["log_available"].get<bool>());
    EXPECT_TRUE(metadata["session_id"].is_null());
}

TEST(FeedbackUpload, BuildPackageIncludesDesktopAndDaemonLogs) {
    TempDir tmp("acecode_feedback_multi_log");
    const fs::path logs = tmp.root / "logs";
    write_text(logs / "desktop-2026-06-18.log", "desktop line\n");
    write_text(logs / "daemon-2026-06-18.log", "daemon line\n");

    acecode::feedback::FeedbackPackageRequest req;
    req.source = "desktop";
    req.feedback_text = "turn hung";
    req.logs = acecode::feedback::collect_runtime_log_sources(logs);
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";

    ASSERT_EQ(req.logs.size(), 2u);
    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.log_included);
    EXPECT_EQ(result.log_tail_bytes, std::string("desktop line\n").size() +
                                         std::string("daemon line\n").size());
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/desktop.log.tail.txt"),
              "desktop line\n");
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/daemon.log.tail.txt"),
              "daemon line\n");

    ASSERT_EQ(result.logs.size(), 2u);
    EXPECT_EQ(result.logs[0].entry_name, "logs/desktop.log.tail.txt");
    EXPECT_TRUE(result.logs[0].included);
    EXPECT_EQ(result.logs[1].entry_name, "logs/daemon.log.tail.txt");
    EXPECT_TRUE(result.logs[1].included);

    auto metadata = nlohmann::json::parse(
        read_zip_entry(result.package_path, "feedback.json"));
    EXPECT_TRUE(metadata["log_available"].get<bool>());
    ASSERT_EQ(metadata["logs"].size(), 2u);
    EXPECT_EQ(metadata["logs"][1]["entry_name"], "logs/daemon.log.tail.txt");
    EXPECT_TRUE(metadata["logs"][1]["available"].get<bool>());
    EXPECT_EQ(metadata["included_files"].size(), 3u);
    EXPECT_EQ(metadata["included_files"][0], "logs/desktop.log.tail.txt");
    EXPECT_EQ(metadata["included_files"][1], "logs/daemon.log.tail.txt");
    EXPECT_EQ(metadata["included_files"][2], "feedback.json");
}

TEST(FeedbackUpload, CollectTuiRuntimeLogSourcesIncludesTuiAndDaemonButNotDesktop) {
    TempDir tmp("acecode_feedback_tui_sources");
    const fs::path logs = tmp.root / "logs";
    const auto older_tui = logs / "tui-2026-06-17.log";
    const auto newer_tui = logs / "tui-2026-06-18.log";
    write_text(older_tui, "older tui");
    write_text(newer_tui, "newer tui");
    write_text(logs / "desktop-2026-06-18.log", "unrelated desktop");
    write_text(logs / "daemon-2026-06-18.log", "daemon");
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(older_tui, now - std::chrono::hours(2));
    fs::last_write_time(newer_tui, now - std::chrono::hours(1));

    const auto tui_sources = acecode::feedback::collect_tui_runtime_log_sources(logs);
    ASSERT_EQ(tui_sources.size(), 2u);
    EXPECT_EQ(tui_sources[0].entry_name, "logs/tui.log.tail.txt");
    EXPECT_EQ(tui_sources[0].path.filename(), fs::path("tui-2026-06-18.log"));
    EXPECT_EQ(tui_sources[1].entry_name, "logs/daemon.log.tail.txt");

    const auto desktop_sources = acecode::feedback::collect_runtime_log_sources(logs);
    ASSERT_EQ(desktop_sources.size(), 2u);
    EXPECT_EQ(desktop_sources[0].entry_name, "logs/desktop.log.tail.txt");
    EXPECT_EQ(desktop_sources[1].entry_name, "logs/daemon.log.tail.txt");

    const fs::path legacy_workspace_log = tmp.root / "workspace" / "acecode.log";
    write_text(legacy_workspace_log, "legacy workspace log");
    acecode::feedback::FeedbackPackageRequest req;
    req.source = "tui";
    req.logs = tui_sources;
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";
    const auto package = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(package.ok) << package.error;
    EXPECT_TRUE(zip_entry_exists(package.package_path, "logs/tui.log.tail.txt"));
    EXPECT_TRUE(zip_entry_exists(package.package_path, "logs/daemon.log.tail.txt"));
    EXPECT_FALSE(zip_entry_exists(package.package_path, "logs/desktop.log.tail.txt"));
    EXPECT_FALSE(zip_entry_exists(package.package_path, "logs/acecode.log.tail.txt"));
}

TEST(FeedbackUpload, CollectTuiRuntimeLogSourcesKeepsDaemonWhenTuiIsMissing) {
    TempDir tmp("acecode_feedback_tui_missing");
    const fs::path logs = tmp.root / "logs";
    write_text(logs / "desktop-2026-06-18.log", "unrelated desktop");
    write_text(logs / "daemon-2026-06-18.log", "daemon");

    const auto sources = acecode::feedback::collect_tui_runtime_log_sources(logs);
    ASSERT_EQ(sources.size(), 1u);
    EXPECT_EQ(sources[0].entry_name, "logs/daemon.log.tail.txt");
}

TEST(FeedbackUpload, CollectRuntimeLogSourcesKeepsDaemonWhenDesktopIsMissing) {
    TempDir tmp("acecode_feedback_daemon_only");
    const fs::path logs = tmp.root / "logs";
    write_text(logs / "daemon-2026-06-17.log", "older daemon");
    write_text(logs / "daemon-2026-06-18.log", "newer daemon");
    write_text(logs / "headless-2026-06-18.log", "unrelated");
    fs::last_write_time(logs / "daemon-2026-06-17.log",
                        fs::file_time_type::clock::now() - std::chrono::hours(2));
    fs::last_write_time(logs / "daemon-2026-06-18.log",
                        fs::file_time_type::clock::now() - std::chrono::hours(1));

    auto sources = acecode::feedback::collect_runtime_log_sources(logs);
    ASSERT_EQ(sources.size(), 1u);
    EXPECT_EQ(sources[0].entry_name, "logs/daemon.log.tail.txt");
    EXPECT_EQ(sources[0].path.filename(), fs::path("daemon-2026-06-18.log"));
}

TEST(FeedbackUpload, CollectRuntimeLogSourcesIsEmptyWhenLogsDirIsMissing) {
    TempDir tmp("acecode_feedback_no_logs_dir");
    EXPECT_TRUE(acecode::feedback::collect_runtime_log_sources(tmp.root / "nope").empty());
}

// 触发场景:用户提交反馈,logs 目录里攒着多天的升级诊断日志(每个进程一个
// upgrade-<date>-<pid>.log)。
// 期望行为:最近三天(72 小时,按修改时间)内写过的全部升级日志合并成一个
// logs/upgrade.log.tail.txt,从旧到新拼接;更早的不带;desktop/daemon 的单文件
// 来源不再掺入升级日志。71h / 73h 两个边界值就是为了钉住 72 小时这条线。
TEST(FeedbackUpload, RecentUpgradeLogsAreMergedChronologicallyIntoOneEntry) {
    TempDir tmp("acecode_feedback_upgrade_window");
    const fs::path logs = tmp.root / "logs";
    const auto stale = logs / "upgrade-2026-09-10-100.log";
    const auto oldest = logs / "upgrade-2026-09-12-200.log";
    const auto middle = logs / "upgrade-2026-09-13-300.log";
    const auto newest = logs / "upgrade-2026-09-13-50.log";
    write_text(stale, "{\"event\":\"stale\"}\n");
    write_text(oldest, "{\"event\":\"oldest\"}\n");
    write_text(middle, "{\"event\":\"middle\"}\n");
    write_text(newest, "{\"event\":\"newest\"}\n");
    write_text(logs / "daemon-2026-09-13.log", "daemon line\n");
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(stale, now - std::chrono::hours(73));
    fs::last_write_time(oldest, now - std::chrono::hours(71));
    fs::last_write_time(middle, now - std::chrono::hours(20));
    // pid 50 的文件名排在 300 前面,但修改时间最新:合并顺序必须按时间不按文件名。
    fs::last_write_time(newest, now - std::chrono::minutes(5));

    auto bundle = acecode::feedback::collect_recent_upgrade_log_bundle(logs);
    ASSERT_TRUE(bundle.has_value());
    EXPECT_EQ(bundle->entry_name, "logs/upgrade.log.tail.txt");
    ASSERT_EQ(bundle->paths.size(), 3u);
    EXPECT_EQ(bundle->paths[0], oldest);
    EXPECT_EQ(bundle->paths[1], middle);
    EXPECT_EQ(bundle->paths[2], newest);

    auto sources = acecode::feedback::collect_runtime_log_sources(logs);
    ASSERT_EQ(sources.size(), 1u);
    EXPECT_EQ(sources[0].entry_name, "logs/daemon.log.tail.txt");

    acecode::feedback::FeedbackPackageRequest req;
    req.source = "desktop";
    req.logs = std::move(sources);
    req.log_bundles.push_back(*bundle);
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-09-15T01:02:03Z";
    const auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/upgrade.log.tail.txt"),
              "{\"event\":\"oldest\"}\n{\"event\":\"middle\"}\n{\"event\":\"newest\"}\n");
    EXPECT_TRUE(result.log_included);
    EXPECT_EQ(result.log_tail_bytes,
              std::string("daemon line\n").size() + 3 * std::string("{\"event\":\"oldest\"}\n").size());

    // metadata:daemon 一行 + 合并包每个成员一行(共用条目名,各记自己进包的字节数)。
    ASSERT_EQ(result.logs.size(), 4u);
    EXPECT_EQ(result.logs[0].entry_name, "logs/daemon.log.tail.txt");
    for (std::size_t i = 1; i < 4; ++i) {
        EXPECT_EQ(result.logs[i].entry_name, "logs/upgrade.log.tail.txt");
        EXPECT_TRUE(result.logs[i].included);
        EXPECT_EQ(result.logs[i].tail_bytes, std::string("{\"event\":\"oldest\"}\n").size());
    }
    EXPECT_EQ(result.logs[1].source_path, acecode::path_to_utf8(oldest));
    EXPECT_EQ(result.logs[3].source_path, acecode::path_to_utf8(newest));
    // 条目只进 included_files 一次。
    ASSERT_EQ(result.included_files.size(), 3u);
    EXPECT_EQ(result.included_files[0], "logs/daemon.log.tail.txt");
    EXPECT_EQ(result.included_files[1], "logs/upgrade.log.tail.txt");
    EXPECT_EQ(result.included_files[2], "feedback.json");
    auto metadata = nlohmann::json::parse(read_zip_entry(result.package_path, "feedback.json"));
    ASSERT_EQ(metadata["logs"].size(), 4u);
    EXPECT_EQ(metadata["logs"][3]["path"], acecode::path_to_utf8(newest));
    EXPECT_EQ(metadata["included_files"].size(), 3u);
}

// 触发场景:三天内没跑过升级(只有更早的升级日志),或者 logs 目录压根不存在。
// 期望行为:不做「退回最新一份」的兜底,返回 nullopt;反馈包里没有升级条目,
// 读包的人据此就能知道这三天没有升级动作。
TEST(FeedbackUpload, RecentUpgradeLogBundleIsAbsentOutsideTheWindow) {
    TempDir tmp("acecode_feedback_upgrade_none");
    const fs::path logs = tmp.root / "logs";
    const auto stale = logs / "upgrade-2026-09-01-100.log";
    write_text(stale, "{\"event\":\"stale\"}\n");
    fs::last_write_time(stale, fs::file_time_type::clock::now() - std::chrono::hours(24 * 5));
    // 名字不合 upgrade-<...>.log 规则的一律不算:没日期后缀 / 别的前缀 / 备份后缀。
    write_text(logs / "upgrade.log", "no suffix\n");
    write_text(logs / "upgraded-2026-09-15-1.log", "other prefix\n");
    write_text(logs / "upgrade-2026-09-15-1.log.bak", "backup\n");

    EXPECT_FALSE(acecode::feedback::collect_recent_upgrade_log_bundle(logs).has_value());
    EXPECT_FALSE(acecode::feedback::collect_recent_upgrade_log_bundle(tmp.root / "nope").has_value());

    // 显式放宽窗口后,同一份过期日志就被选中 —— 说明上面的 nullopt 是窗口过滤的结果。
    auto widened = acecode::feedback::collect_recent_upgrade_log_bundle(
        logs, std::chrono::hours(24 * 6));
    ASSERT_TRUE(widened.has_value());
    ASSERT_EQ(widened->paths.size(), 1u);
    EXPECT_EQ(widened->paths[0], stale);
}

// 触发场景:升级重启死循环之类的故障让三天内攒出大量升级日志,合并后超过尾巴上限。
// 期望行为:整体只保留最后 cap 字节 —— 最新的记录活下来,最老的先被裁;metadata
// 里每个成员的 tail_bytes 反映真正进包的字节数,被整个裁掉的成员记 0 但仍标可读;
// 缺失的成员不影响其余成员进包。
TEST(FeedbackUpload, UpgradeLogBundleKeepsNewestBytesWhenOverCap) {
    TempDir tmp("acecode_feedback_upgrade_cap");
    const fs::path a = tmp.root / "a.log";
    const fs::path b = tmp.root / "b.log";
    const fs::path c = tmp.root / "c.log";
    write_text(a, "AAAA");
    write_text(b, "BBBB");
    write_text(c, "CCCC");

    acecode::feedback::FeedbackPackageRequest req;
    acecode::feedback::FeedbackLogBundle bundle;
    bundle.paths = {a, tmp.root / "missing.log", b, c};
    bundle.entry_name = "logs/upgrade.log.tail.txt";
    bundle.max_bytes = 6;
    req.log_bundles.push_back(bundle);
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-09-15T01:02:03Z";

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/upgrade.log.tail.txt"), "BBCCCC");
    EXPECT_EQ(result.log_tail_bytes, 6u);
    ASSERT_EQ(result.logs.size(), 4u);
    EXPECT_TRUE(result.logs[0].included);
    EXPECT_EQ(result.logs[0].tail_bytes, 0u);
    EXPECT_FALSE(result.logs[1].included);
    EXPECT_EQ(result.logs[1].tail_bytes, 0u);
    EXPECT_TRUE(result.logs[2].included);
    EXPECT_EQ(result.logs[2].tail_bytes, 2u);
    EXPECT_TRUE(result.logs[3].included);
    EXPECT_EQ(result.logs[3].tail_bytes, 4u);
}

// 触发场景:合并包的成员全部不存在(比如枚举后、打包前被清理工具删掉)。
// 期望行为:打包仍成功,不产生条目,成员逐个标不可读;log_included 不因此置真。
TEST(FeedbackUpload, UpgradeLogBundleWithNoReadableMemberAddsNoEntry) {
    TempDir tmp("acecode_feedback_upgrade_gone");
    acecode::feedback::FeedbackPackageRequest req;
    acecode::feedback::FeedbackLogBundle bundle;
    bundle.paths = {tmp.root / "gone-1.log", tmp.root / "gone-2.log"};
    bundle.entry_name = "logs/upgrade.log.tail.txt";
    req.log_bundles.push_back(bundle);
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-09-15T01:02:03Z";

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.log_included);
    EXPECT_FALSE(zip_entry_exists(result.package_path, "logs/upgrade.log.tail.txt"));
    ASSERT_EQ(result.logs.size(), 2u);
    EXPECT_FALSE(result.logs[0].included);
    EXPECT_FALSE(result.logs[1].included);
    ASSERT_EQ(result.included_files.size(), 1u);
    EXPECT_EQ(result.included_files[0], "feedback.json");
}

TEST(FeedbackUpload, PartiallyMissingLogsStillPackageTheAvailableOnes) {
    TempDir tmp("acecode_feedback_partial_logs");
    const fs::path daemon_log = tmp.root / "daemon-2026-06-18.log";
    write_text(daemon_log, "daemon only\n");

    acecode::feedback::FeedbackPackageRequest req;
    req.source = "desktop";
    req.logs.push_back({tmp.root / "desktop-missing.log", "logs/desktop.log.tail.txt", 0});
    req.logs.push_back({daemon_log, "logs/daemon.log.tail.txt", 0});
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.log_included);
    EXPECT_FALSE(zip_entry_exists(result.package_path, "logs/desktop.log.tail.txt"));
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/daemon.log.tail.txt"),
              "daemon only\n");
    ASSERT_EQ(result.logs.size(), 2u);
    EXPECT_FALSE(result.logs[0].included);
    EXPECT_TRUE(result.logs[1].included);
}

TEST(FeedbackUpload, PerLogByteCapOverridesTheRequestDefault) {
    TempDir tmp("acecode_feedback_per_log_cap");
    const fs::path big = tmp.root / "desktop-2026-06-18.log";
    const fs::path capped_log = tmp.root / "daemon-2026-06-18.log";
    write_text(big, "0123456789");
    write_text(capped_log, "abcdefghij");

    acecode::feedback::FeedbackPackageRequest req;
    req.source = "desktop";
    req.max_log_bytes = 4;
    req.logs.push_back({big, "logs/desktop.log.tail.txt", 0});
    req.logs.push_back({capped_log, "logs/daemon.log.tail.txt", 2});
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/desktop.log.tail.txt"), "6789");
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/daemon.log.tail.txt"), "ij");
    EXPECT_EQ(result.log_tail_bytes, 6u);
}

TEST(FeedbackUpload, DuplicateLogEntryNamesDoNotOverwriteEachOther) {
    TempDir tmp("acecode_feedback_dup_entry");
    const fs::path a = tmp.root / "a.log";
    const fs::path b = tmp.root / "b.log";
    write_text(a, "first");
    write_text(b, "second");

    acecode::feedback::FeedbackPackageRequest req;
    req.logs.push_back({a, "logs/daemon.log.tail.txt", 0});
    req.logs.push_back({b, "logs/daemon.log.tail.txt", 0});
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/daemon.log.tail.txt"), "first");
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/daemon.log.tail.txt.2"), "second");
}

TEST(FeedbackUpload, LogEntryNameDefaultsToTheSourceFilename) {
    TempDir tmp("acecode_feedback_default_entry");
    const fs::path log = tmp.root / "daemon-2026-06-18.log";
    write_text(log, "daemon line");

    acecode::feedback::FeedbackPackageRequest req;
    req.logs.push_back({log, "", 0});
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(read_zip_entry(result.package_path,
                             "logs/daemon-2026-06-18.log.tail.txt"),
              "daemon line");
}

TEST(FeedbackUpload, LatestRotatedLogPathPicksNewestMatchingBase) {
    TempDir tmp("acecode_feedback_latest_rotated");
    const fs::path logs = tmp.root / "logs";
    write_text(logs / "daemon-2026-06-17.log", "older");
    write_text(logs / "daemon-2026-06-18.log", "newer");
    write_text(logs / "desktop-2026-06-19.log", "ignored");
    fs::last_write_time(logs / "daemon-2026-06-17.log",
                        fs::file_time_type::clock::now() - std::chrono::hours(2));
    fs::last_write_time(logs / "daemon-2026-06-18.log",
                        fs::file_time_type::clock::now() - std::chrono::hours(1));
    fs::last_write_time(logs / "desktop-2026-06-19.log",
                        fs::file_time_type::clock::now());

    auto found = acecode::feedback::latest_rotated_log_path(logs, "daemon");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->filename(), fs::path("daemon-2026-06-18.log"));
    EXPECT_FALSE(acecode::feedback::latest_rotated_log_path(logs, "headless").has_value());
}

TEST(FeedbackUpload, LatestDesktopLogPathPicksNewestDesktopLog) {
    TempDir tmp("acecode_feedback_desktop_latest_log");
    const fs::path logs = tmp.root / "logs";
    const fs::path older = logs / "desktop-2026-06-17.log";
    const fs::path newer = logs / "desktop-2026-06-18.log";
    const fs::path ignored = logs / "daemon-2026-06-19.log";
    write_text(older, "older");
    write_text(newer, "newer");
    write_text(ignored, "ignored");
    fs::last_write_time(older, fs::file_time_type::clock::now() - std::chrono::hours(2));
    fs::last_write_time(newer, fs::file_time_type::clock::now() - std::chrono::hours(1));
    fs::last_write_time(ignored, fs::file_time_type::clock::now());

    auto found = acecode::feedback::latest_desktop_log_path(logs);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->filename(), newer.filename());
}

TEST(FeedbackUpload, PackageFilenameAddsAvailableMachineSuffixFields) {
    EXPECT_EQ(
        acecode::feedback::make_feedback_package_filename(
            "sid", "2026-06-18T01:02:03Z", "windows/x64", "Build Box",
            "\xE7\x94\xA8\xE6\x88\xB7"),
        "acecode-feedback-sid-20260618-010203-windows-x64-Build-Box"
        "-xe7x94xa8xe6x88xb7.zip");

    EXPECT_EQ(
        acecode::feedback::make_feedback_package_filename(
            "sid", "2026-06-18T01:02:03Z", "windows-x64", "", ""),
        "acecode-feedback-sid-20260618-010203-windows-x64.zip");
}

TEST(FeedbackUpload, BuildPackageSucceedsWhenLogIsMissing) {
    TempDir tmp("acecode_feedback_missing_log");
    const fs::path session = tmp.root / "session.jsonl";
    write_text(session, "{}\n");

    acecode::feedback::FeedbackPackageRequest req;
    req.session_id = "sid";
    req.session_jsonl_path = session;
    req.logs.push_back({tmp.root / "missing.log", "logs/acecode.log.tail.txt", 0});
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.log_included);
    auto metadata = nlohmann::json::parse(
        read_zip_entry(result.package_path, "feedback.json"));
    EXPECT_FALSE(metadata["log_available"].get<bool>());
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/acecode.log.tail.txt"), "");
}

TEST(FeedbackUpload, UploadPackageSendsGoHttpServerCompatibleMultipart) {
    TempDir tmp("acecode_feedback_upload");
    const fs::path package = tmp.root / "feedback.zip";
    write_text(package, "zip-bytes");

    std::string received_filename;
    std::string received_content;
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/", [&](const httplib::Request& req, httplib::Response& res) {
            EXPECT_TRUE(req.is_multipart_form_data());
            auto file = req.get_file_value("file");
            auto filename = req.get_file_value("filename");
            received_filename = file.filename;
            received_content = file.content;
            EXPECT_EQ(filename.content, "feedback.zip");
            res.set_content(R"({"success":true})", "application/json");
        });
    });

    acecode::feedback::FeedbackUploadRequest req;
    req.upload_url = server.base_url();
    req.package_path = package;
    req.package_filename = "feedback.zip";
    req.timeout_ms = 3000;

    auto result = acecode::feedback::upload_feedback_package(req);
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(received_filename, "feedback.zip");
    EXPECT_EQ(received_content, "zip-bytes");
}

TEST(FeedbackUpload, UploadPackageTreatsSuccessFalseAsFailure) {
    TempDir tmp("acecode_feedback_upload_fail");
    const fs::path package = tmp.root / "feedback.zip";
    write_text(package, "zip-bytes");

    LocalHttpServer server([](httplib::Server& s) {
        s.Post("/", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"({"success":false,"error":"denied"})", "application/json");
        });
    });

    acecode::feedback::FeedbackUploadRequest req;
    req.upload_url = server.base_url();
    req.package_path = package;
    req.package_filename = "feedback.zip";
    req.timeout_ms = 3000;

    auto result = acecode::feedback::upload_feedback_package(req);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_NE(result.error.find("denied"), std::string::npos);
}
