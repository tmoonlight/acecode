// 端到端验证两个真实子进程在同一 cwd 创建不同 session 时,都会写入
// canonical `<session-id>.jsonl` / `<session-id>.meta.json` 文件,不会再生成
// `<session-id>-<pid>.jsonl` 多副本。

#include <gtest/gtest.h>

#include "session/session_storage.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path make_unique_tmp(const std::string& hint) {
    auto base = fs::temp_directory_path() /
                ("acecode_e2e_" + hint + "_" +
                 std::to_string(::testing::UnitTest::GetInstance()
                     ->current_test_info()->line()) + "_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

std::string quote_arg(const std::string& s) {
    std::string out = "\"";
    out += s;
    out += "\"";
    return out;
}

int run_writer(const std::string& fake_home,
               const std::string& cwd,
               const std::string& content) {
    std::ostringstream cmd;
#ifdef _WIN32
    cmd << "\"";
#endif
    cmd << quote_arg(ACECODE_CONCURRENT_SESSION_WRITER_PATH)
        << " " << quote_arg(fake_home)
        << " " << quote_arg(cwd)
        << " " << quote_arg(content);
#ifdef _WIN32
    cmd << "\"";
#endif
    return std::system(cmd.str().c_str());
}

std::string read_all(const fs::path& path) {
    std::ifstream ifs(path);
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST(SessionConcurrencyE2E, TwoRealSubprocessesProduceCanonicalSessionFiles) {
    auto fake_home = make_unique_tmp("home");
    auto cwd       = make_unique_tmp("cwd");

    int rc1 = run_writer(fake_home.string(), cwd.string(), "content-from-process-A");
    int rc2 = run_writer(fake_home.string(), cwd.string(), "content-from-process-B");
    ASSERT_EQ(rc1, 0) << "writer subprocess A should exit normally";
    ASSERT_EQ(rc2, 0) << "writer subprocess B should exit normally";

    auto project_dir = fake_home / ".acecode" / "projects" /
                        acecode::SessionStorage::compute_project_hash(cwd.string());
    ASSERT_TRUE(fs::exists(project_dir))
        << "project_dir should be created by a writer subprocess: "
        << project_dir.string();

    std::regex canonical_re(R"(^\d{8}-\d{6}-[0-9a-f]{4}\.jsonl$)");
    std::regex pid_re(R"(^\d{8}-\d{6}-[0-9a-f]{4}-\d+\.jsonl$)");
    std::vector<fs::path> jsonl_paths;
    for (const auto& e : fs::directory_iterator(project_dir)) {
        if (!e.is_regular_file()) continue;
        std::string fname = e.path().filename().string();
        EXPECT_FALSE(std::regex_match(fname, pid_re))
            << "PID-suffixed session files are old incompatible data: " << fname;
        if (std::regex_match(fname, canonical_re)) {
            jsonl_paths.push_back(e.path());
        }
    }

    ASSERT_EQ(jsonl_paths.size(), 2u)
        << "two subprocesses should create two canonical session transcripts";

    int files_with_a = 0;
    int files_with_b = 0;
    for (const auto& p : jsonl_paths) {
        const std::string body = read_all(p);
        if (body.find("content-from-process-A") != std::string::npos) ++files_with_a;
        if (body.find("content-from-process-B") != std::string::npos) ++files_with_b;
    }
    EXPECT_EQ(files_with_a, 1) << "content-A should appear in exactly one file";
    EXPECT_EQ(files_with_b, 1) << "content-B should appear in exactly one file";

    auto sessions = acecode::SessionStorage::list_sessions(project_dir.string());
    EXPECT_EQ(sessions.size(), 2u)
        << "list_sessions should enumerate the two canonical sessions";

    std::error_code ec;
    fs::remove_all(fake_home, ec);
    fs::remove_all(cwd, ec);
}
