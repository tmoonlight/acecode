// 覆盖 src/desktop/workspace_registry.cpp。WorkspaceRegistry 是 desktop 的多
// workspace 模型基石 — 默认命名错、损坏文件挂、原子写不原子,任何一项跑偏
// 用户都能立刻在 sidebar 上看到。所有写盘路径走 tmp dir 以免污染 ~/.acecode。

#include <gtest/gtest.h>

#include "desktop/workspace_registry.hpp"
#include "session/session_storage.hpp"
#include "utils/cwd_hash.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using acecode::compute_cwd_hash;
using acecode::desktop::WorkspaceMeta;
using acecode::desktop::WorkspaceRegistry;
using acecode::desktop::default_workspace_name;
using acecode::desktop::ensure_workspace_metadata;
using acecode::desktop::workspace_hash_matches_cwd;

namespace {

// 测试 fixture: 每个 TEST 独立 tmp dir,析构时清理。
class TmpProjectsDir {
public:
    TmpProjectsDir() {
        auto base = fs::temp_directory_path() / "acecode_workspace_registry_test";
        fs::remove_all(base);
        fs::create_directories(base);
        path_ = base.string();
    }
    ~TmpProjectsDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

// 直接写一个 workspace.json,用于覆盖 scan / 损坏 fallback 的 setup 路径。
void seed_workspace_json(const std::string& projects_dir,
                         const std::string& hash,
                         const std::string& cwd,
                         const std::string& name,
                         std::optional<bool> desktop_visible = true,
                         const std::string& raw_override = {}) {
    fs::create_directories(fs::path(projects_dir) / hash);
    std::ofstream ofs((fs::path(projects_dir) / hash / "workspace.json").string());
    if (!raw_override.empty()) {
        ofs << raw_override; // 用于损坏 JSON 测试
        return;
    }
    nlohmann::json j;
    j["cwd"] = cwd;
    j["name"] = name;
    if (desktop_visible.has_value()) {
        j["desktop_visible"] = *desktop_visible;
    }
    ofs << j.dump();
}

} // namespace

// 场景: POSIX 路径默认命名 = basename
TEST(WorkspaceRegistryDefault, PosixBasename) {
    EXPECT_EQ(default_workspace_name("/home/shao/proj"), "proj");
}

// 场景: Windows 反斜杠路径 默认命名 = basename
TEST(WorkspaceRegistryDefault, WindowsBackslashBasename) {
    EXPECT_EQ(default_workspace_name("N:\\Users\\shao\\acecode"), "acecode");
}

// 场景: UTF-8 路径名不应被 Windows 窄编码转换打碎。
TEST(WorkspaceRegistryDefault, Utf8BasenamePreserved) {
    EXPECT_EQ(default_workspace_name("C:\\Users\\shao\\项目A"), "项目A");
}

// 场景: 尾斜杠不影响默认命名("foo/" 仍是 "foo")
TEST(WorkspaceRegistryDefault, TrailingSlashBasename) {
    EXPECT_EQ(default_workspace_name("/home/shao/proj/"), "proj");
    EXPECT_EQ(default_workspace_name("/home/shao/proj//"), "proj");
}

// 场景: 根路径走 root_name 兜底("C:\\" → "C:" 在 Windows;POSIX 则继续到字面常量)
TEST(WorkspaceRegistryDefault, RootPathFallback) {
    auto v = default_workspace_name("/");
    // 在 POSIX 上 "/" 的 basename 为空 + root_name 也为空 → 走"workspace"字面;
    // 在 Windows 上 "/" 解析为根路径,root_name 通常为空 → 同样走"workspace"。
    // 不论平台,结果只可能是 root_name (非空) 或 "workspace"。
    EXPECT_FALSE(v.empty());
}

// 场景: 完全空字符串 cwd → 字面 "workspace"(避免空字符串当 name 显示)
TEST(WorkspaceRegistryDefault, EmptyCwdLiteralFallback) {
    EXPECT_EQ(default_workspace_name(""), "workspace");
}

// 场景: 空目录 scan 后 list 为空(不该崩,不该误造条目)
TEST(WorkspaceRegistry, ScanEmptyDir) {
    TmpProjectsDir tmp;
    WorkspaceRegistry r;
    r.scan(tmp.path());
    EXPECT_TRUE(r.list().empty());
}

// 场景: 不存在的 projects_dir → list 为空,不抛异常
TEST(WorkspaceRegistry, ScanMissingDir) {
    WorkspaceRegistry r;
    r.scan("/tmp/this/path/does/not/exist/abcxyz");
    EXPECT_TRUE(r.list().empty());
}

// 场景: scan 入册带合法 workspace.json 的子目录
TEST(WorkspaceRegistry, ScanLoadsValidEntry) {
    TmpProjectsDir tmp;
    seed_workspace_json(tmp.path(), "abcdef0123456789", "/home/x/y", "myname");

    WorkspaceRegistry r;
    r.scan(tmp.path());
    auto v = r.list();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].hash, "abcdef0123456789");
    EXPECT_EQ(v[0].cwd, "/home/x/y");
    EXPECT_EQ(v[0].name, "myname");
    EXPECT_TRUE(v[0].desktop_visible);
}

// 场景: scan 跳过缺 workspace.json 的孤儿目录(老 SessionManager 写过 sessions 但没 metadata)
TEST(WorkspaceRegistry, ScanSkipsOrphanDir) {
    TmpProjectsDir tmp;
    fs::create_directories(fs::path(tmp.path()) / "deadbeef00000000");
    // 故意不写 workspace.json
    WorkspaceRegistry r;
    r.scan(tmp.path());
    EXPECT_TRUE(r.list().empty());
}

// 场景: 历史 projects/<hash> 只有 session meta、没有 workspace.json 时,
// desktop scan 必须保持隐藏,不能回填 workspace.json。
TEST(WorkspaceRegistry, ScanDoesNotBackfillMissingWorkspaceJsonFromSessionMeta) {
    TmpProjectsDir tmp;
    const std::string cwd = "/home/u/old-project";
    const std::string hash = compute_cwd_hash(cwd);
    fs::create_directories(fs::path(tmp.path()) / hash);

    acecode::SessionMeta meta;
    meta.id = "20260502-010203-abcd";
    meta.cwd = cwd;
    meta.created_at = "2026-05-02T01:02:03Z";
    meta.updated_at = meta.created_at;
    acecode::SessionStorage::write_meta(
        (fs::path(tmp.path()) / hash / (meta.id + ".meta.json")).string(),
        meta);

    WorkspaceRegistry r;
    r.scan(tmp.path());
    auto v = r.list();
    EXPECT_TRUE(v.empty());
    EXPECT_FALSE(fs::exists(fs::path(tmp.path()) / hash / "workspace.json"));
}

// 场景: workspace.json 合法但缺 desktop_visible marker,desktop startup 仍隐藏。
TEST(WorkspaceRegistry, ScanMissingVisibleMarkerHidesEntry) {
    TmpProjectsDir tmp;
    seed_workspace_json(tmp.path(), "abcdef0123456789", "/home/x/y", "myname", std::nullopt);

    WorkspaceRegistry r;
    r.scan(tmp.path());
    EXPECT_TRUE(r.list().empty());
}

// 场景: workspace.json 明确 desktop_visible=false,desktop startup 隐藏。
TEST(WorkspaceRegistry, ScanFalseVisibleMarkerHidesEntry) {
    TmpProjectsDir tmp;
    seed_workspace_json(tmp.path(), "abcdef0123456789", "/home/x/y", "myname", false);

    WorkspaceRegistry r;
    r.scan(tmp.path());
    EXPECT_TRUE(r.list().empty());
}

// 场景: 损坏 JSON workspace.json 走 fallback — 不入册,且不删除原文件
TEST(WorkspaceRegistry, ScanCorruptedJsonGracefullySkips) {
    TmpProjectsDir tmp;
    seed_workspace_json(tmp.path(), "abcdef0123456789", "", "", true, "{ this is not json");

    WorkspaceRegistry r;
    r.scan(tmp.path());
    EXPECT_TRUE(r.list().empty());

    // 原文件应仍在(不自动清理用户数据)
    EXPECT_TRUE(fs::exists(fs::path(tmp.path()) / "abcdef0123456789" / "workspace.json"));
}

// 场景: workspace.json 缺 name 字段 → fallback 到 default_workspace_name(cwd)
TEST(WorkspaceRegistry, ScanMissingNameFallsBackToBasename) {
    TmpProjectsDir tmp;
    fs::create_directories(fs::path(tmp.path()) / "1111111111111111");
    {
        std::ofstream ofs((fs::path(tmp.path()) / "1111111111111111" / "workspace.json").string());
        ofs << R"({"cwd": "/home/u/code/projx", "desktop_visible": true})"; // 缺 name
    } // ofs 析构 → 关闭 + 刷盘

    WorkspaceRegistry r;
    r.scan(tmp.path());
    auto v = r.list();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].name, "projx");
}

// 场景: register_new 给新 cwd 创建条目 + 写 workspace.json
TEST(WorkspaceRegistry, RegisterNewCreatesEntryAndFile) {
    TmpProjectsDir tmp;
    WorkspaceRegistry r;
    auto m = r.register_new(tmp.path(), "/home/u/test-proj");
    EXPECT_EQ(m.cwd, "/home/u/test-proj");
    EXPECT_EQ(m.name, "test-proj"); // 默认命名
    EXPECT_EQ(m.hash, compute_cwd_hash("/home/u/test-proj"));
    EXPECT_TRUE(m.desktop_visible);

    // 文件落地
    auto json_path = fs::path(tmp.path()) / m.hash / "workspace.json";
    EXPECT_TRUE(fs::exists(json_path));
    std::ifstream ifs(json_path.string());
    auto j = nlohmann::json::parse(ifs);
    EXPECT_TRUE(j["desktop_visible"].get<bool>());

    // list 包含新条目
    EXPECT_EQ(r.list().size(), 1u);
}

// 场景: Desktop 启动后 daemon 用另一份 registry 新增 workspace；Desktop
// 操作前重扫持久化 marker 后必须能解析新 hash，无需重启进程。
TEST(WorkspaceRegistry, RescanFindsWorkspaceRegisteredByAnotherInstance) {
    TmpProjectsDir tmp;
    WorkspaceRegistry desktop_registry;
    desktop_registry.scan(tmp.path());
    EXPECT_TRUE(desktop_registry.list().empty());

    WorkspaceRegistry daemon_registry;
    auto created = daemon_registry.register_new(tmp.path(), "/home/u/live-created");
    EXPECT_FALSE(desktop_registry.get(created.hash).has_value());

    desktop_registry.scan(tmp.path());
    auto refreshed = desktop_registry.get(created.hash);
    ASSERT_TRUE(refreshed.has_value());
    EXPECT_EQ(refreshed->cwd, created.cwd);
    EXPECT_EQ(refreshed->name, created.name);
    EXPECT_TRUE(refreshed->desktop_visible);
}

// 场景: 手动添加一个已有 TUI 项目目录时,只写 workspace.json marker,
// 不重写已有 session meta;后续 session listing 仍能看到旧会话。
TEST(WorkspaceRegistry, RegisterNewImportsExistingTuiSessionsWithoutRewritingThem) {
    TmpProjectsDir tmp;
    const std::string cwd = "/home/u/old-project";
    const std::string hash = compute_cwd_hash(cwd);
    const auto project_dir = fs::path(tmp.path()) / hash;
    fs::create_directories(project_dir);

    acecode::SessionMeta meta;
    meta.id = "20260502-010203-abcd";
    meta.cwd = cwd;
    meta.created_at = "2026-05-02T01:02:03Z";
    meta.updated_at = "2026-05-02T01:03:03Z";
    meta.summary = "old tui session";
    meta.message_count = 2;
    const auto meta_path = (project_dir / (meta.id + ".meta.json")).string();
    acecode::SessionStorage::write_meta(meta_path, meta);
    std::ifstream before_stream(meta_path);
    const std::string before((std::istreambuf_iterator<char>(before_stream)),
                             std::istreambuf_iterator<char>());

    WorkspaceRegistry r;
    auto m = r.register_new(tmp.path(), cwd);
    EXPECT_EQ(m.hash, hash);
    EXPECT_TRUE(m.desktop_visible);

    std::ifstream after_stream(meta_path);
    const std::string after((std::istreambuf_iterator<char>(after_stream)),
                            std::istreambuf_iterator<char>());
    EXPECT_EQ(after, before);

    auto sessions = acecode::SessionStorage::list_sessions(project_dir.string());
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].id, meta.id);

    WorkspaceRegistry r2;
    r2.scan(tmp.path());
    auto visible = r2.list();
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(visible[0].hash, hash);
}

// 场景: hide 只把 desktop_visible 写 false + 从 list 移除,不删除目录 / session 文件。
TEST(WorkspaceRegistry, HidePersistsHiddenMarkerWithoutDeletingData) {
    TmpProjectsDir tmp;
    const std::string cwd = "/home/u/hide-me";
    WorkspaceRegistry r;
    auto m = r.register_new(tmp.path(), cwd);

    const auto project_dir = fs::path(tmp.path()) / m.hash;
    acecode::SessionMeta meta;
    meta.id = "20260505-010203-hide";
    meta.cwd = cwd;
    meta.created_at = "2026-05-05T01:02:03Z";
    meta.updated_at = meta.created_at;
    const auto meta_path = project_dir / (meta.id + ".meta.json");
    acecode::SessionStorage::write_meta(meta_path.string(), meta);

    EXPECT_TRUE(r.hide(tmp.path(), m.hash));
    EXPECT_TRUE(r.list().empty());
    EXPECT_TRUE(fs::exists(project_dir));
    EXPECT_TRUE(fs::exists(meta_path));

    std::ifstream ifs((project_dir / "workspace.json").string());
    auto j = nlohmann::json::parse(ifs);
    ASSERT_TRUE(j.contains("desktop_visible"));
    EXPECT_FALSE(j["desktop_visible"].get<bool>());

    WorkspaceRegistry r2;
    r2.scan(tmp.path());
    EXPECT_TRUE(r2.list().empty());
}

// 场景: 用户重新添加同一个隐藏 workspace 时,保留原 name 并恢复可见 marker。
TEST(WorkspaceRegistry, RegisterNewRestoresHiddenWorkspacePreservingName) {
    TmpProjectsDir tmp;
    const std::string cwd = "/home/u/restore-me";
    const std::string hash = compute_cwd_hash(cwd);
    seed_workspace_json(tmp.path(), hash, cwd, "custom-restore-name", false);

    WorkspaceRegistry r;
    auto m = r.register_new(tmp.path(), cwd);
    EXPECT_EQ(m.hash, hash);
    EXPECT_EQ(m.name, "custom-restore-name");
    EXPECT_TRUE(m.desktop_visible);

    std::ifstream ifs((fs::path(tmp.path()) / hash / "workspace.json").string());
    auto j = nlohmann::json::parse(ifs);
    EXPECT_TRUE(j["desktop_visible"].get<bool>());
    EXPECT_EQ(j["name"].get<std::string>(), "custom-restore-name");

    WorkspaceRegistry r2;
    r2.scan(tmp.path());
    auto visible = r2.list();
    ASSERT_EQ(visible.size(), 1u);
    EXPECT_EQ(visible[0].hash, hash);
    EXPECT_EQ(visible[0].name, "custom-restore-name");
}

TEST(WorkspaceRegistry, WorkspaceHashMatchesCwd) {
    const std::string cwd = "/home/u/hash-me";
    EXPECT_TRUE(workspace_hash_matches_cwd(compute_cwd_hash(cwd), cwd));
    EXPECT_FALSE(workspace_hash_matches_cwd("deadbeefdeadbeef", cwd));
    EXPECT_FALSE(workspace_hash_matches_cwd("", cwd));
    EXPECT_FALSE(workspace_hash_matches_cwd(compute_cwd_hash(cwd), ""));
}

// 场景: TUI/daemon 启动时只应补缺失 workspace.json,不能覆盖用户重命名。
TEST(WorkspaceRegistry, EnsureWorkspaceMetadataDoesNotOverwriteExistingName) {
    TmpProjectsDir tmp;
    const std::string cwd = "/home/u/keep-name";
    const std::string hash = compute_cwd_hash(cwd);
    seed_workspace_json(tmp.path(), hash, cwd, "custom-name");

    EXPECT_TRUE(ensure_workspace_metadata(tmp.path(), cwd));

    WorkspaceRegistry r;
    r.scan(tmp.path());
    auto v = r.list();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].name, "custom-name");
}

// 场景: TUI/daemon bootstrap 只补 metadata,但 desktop_visible=false,
// 所以下次 Desktop startup 不会自动显示该目录。
TEST(WorkspaceRegistry, EnsureWorkspaceMetadataWritesHiddenMarker) {
    TmpProjectsDir tmp;
    const std::string cwd = "/home/u/tui-only";
    const std::string hash = compute_cwd_hash(cwd);

    EXPECT_TRUE(ensure_workspace_metadata(tmp.path(), cwd));

    std::ifstream ifs((fs::path(tmp.path()) / hash / "workspace.json").string());
    auto j = nlohmann::json::parse(ifs);
    ASSERT_TRUE(j.contains("desktop_visible"));
    EXPECT_FALSE(j["desktop_visible"].get<bool>());

    WorkspaceRegistry r;
    r.scan(tmp.path());
    EXPECT_TRUE(r.list().empty());
}

// 场景: register_new 第二次同 cwd → 返回已有 meta,不重写文件
TEST(WorkspaceRegistry, RegisterNewIdempotent) {
    TmpProjectsDir tmp;
    WorkspaceRegistry r;
    auto m1 = r.register_new(tmp.path(), "/home/u/dup");
    // 改一下 name 模拟用户已经 set_name 过
    r.set_name(tmp.path(), m1.hash, "renamed");
    auto m2 = r.register_new(tmp.path(), "/home/u/dup");
    EXPECT_EQ(m2.hash, m1.hash);
    EXPECT_EQ(m2.name, "renamed"); // 没被默认名覆盖
    EXPECT_EQ(r.list().size(), 1u);
}

// 场景: set_name 拒空,且不动磁盘
TEST(WorkspaceRegistry, SetNameRejectsEmpty) {
    TmpProjectsDir tmp;
    WorkspaceRegistry r;
    auto m = r.register_new(tmp.path(), "/home/u/x");
    EXPECT_FALSE(r.set_name(tmp.path(), m.hash, ""));
    auto cur = r.get(m.hash);
    ASSERT_TRUE(cur.has_value());
    EXPECT_EQ(cur->name, "x"); // 仍是默认名
}

// 场景: set_name 未知 hash 报错
TEST(WorkspaceRegistry, SetNameUnknownHash) {
    TmpProjectsDir tmp;
    WorkspaceRegistry r;
    EXPECT_FALSE(r.set_name(tmp.path(), "nonexistent", "x"));
}

// 场景: set_name 持久化往返(写盘后重新 scan 仍是新名)
TEST(WorkspaceRegistry, SetNamePersistsAcrossScan) {
    TmpProjectsDir tmp;
    {
        WorkspaceRegistry r;
        auto m = r.register_new(tmp.path(), "/home/u/q");
        EXPECT_TRUE(r.set_name(tmp.path(), m.hash, "renamed-q"));
        std::ifstream ifs((fs::path(tmp.path()) / m.hash / "workspace.json").string());
        auto j = nlohmann::json::parse(ifs);
        EXPECT_TRUE(j["desktop_visible"].get<bool>());
    }
    WorkspaceRegistry r2;
    r2.scan(tmp.path());
    auto v = r2.list();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].name, "renamed-q");
    EXPECT_TRUE(v[0].desktop_visible);
}

// 场景: 并发 set_name(同一 hash 多线程)不让磁盘留半截 / 内存出现损坏。
// 这条主要验证 mutex 没死锁 + atomic_write 不撞文件。
TEST(WorkspaceRegistry, ConcurrentSetNameDoesNotCorrupt) {
    TmpProjectsDir tmp;
    WorkspaceRegistry r;
    auto m = r.register_new(tmp.path(), "/home/u/concurrent");

    constexpr int kThreads = 8;
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&, i] {
            r.set_name(tmp.path(), m.hash, "name-" + std::to_string(i));
        });
    }
    for (auto& t : ts) t.join();

    // 内存里 name 应该是 8 个值之一,且 list 仍只有 1 条
    auto cur = r.get(m.hash);
    ASSERT_TRUE(cur.has_value());
    EXPECT_EQ(r.list().size(), 1u);

    // 文件应可解析(没残留半截 JSON)
    std::ifstream ifs((fs::path(tmp.path()) / m.hash / "workspace.json").string());
    ASSERT_TRUE(ifs.is_open());
    std::stringstream buf; buf << ifs.rdbuf();
    auto j = nlohmann::json::parse(buf.str()); // 抛异常即测试失败
    EXPECT_TRUE(j.contains("name"));
    EXPECT_TRUE(j["name"].is_string());
}

// 场景: get 不存在的 hash 返回 nullopt
TEST(WorkspaceRegistry, GetMissing) {
    WorkspaceRegistry r;
    EXPECT_FALSE(r.get("nope").has_value());
}

// 触发场景:~/.acecode/projects 下每个用过的 cwd 都留一个 hash 目录,实测
// 16568 个,其中带 workspace.json 的只有几十个。侧边栏 5 秒轮询里
// /api/workspaces 与 /api/pinned-sessions/order 各调一次 scan(),而逐个探
// workspace.json 实测要 267ms(纯目录枚举只要 12.8ms)—— 这 267ms 一直占着
// 浏览器仅有的并发连接,把用户点击展开时的会话列表请求挤到队尾,表现为
// 侧边栏长时间停在「加载中...」。
//
// 期望行为:目录没动过就沿用上次结论,不再重读它的 workspace.json。
// 观测手段:绕开 atomic 写(直接覆写文件内容)改掉 marker —— 这不会推进所在
// 目录的 mtime,所以缓存应当仍返回旧值。真实写路径走 atomic_file 的
// tmp+rename,rename 会推进目录 mtime,不受此影响(见下一条测试)。
TEST(WorkspaceRegistryProbeCache, UnchangedDirectoryIsNotReRead) {
    TmpProjectsDir tmp;
    const std::string hash = "aaaaaaaaaaaaaaaa";
    seed_workspace_json(tmp.path(), hash, "/tmp/a", "A");

    WorkspaceRegistry reg;
    reg.scan(tmp.path());
    ASSERT_TRUE(reg.get(hash).has_value());
    ASSERT_EQ(reg.get(hash)->name, "A");

    // 把目录时间戳推到「新鲜窗口」(2 秒)之外,缓存才允许生效。真实数据里
    // 老项目目录的时间戳本来就是旧的;窗口内的目录会被强制重读,那是为了
    // 不漏掉与上次扫描落在同一时钟 tick 的写入。
    fs::last_write_time(fs::path(tmp.path()) / hash,
                        fs::file_time_type::clock::now() - std::chrono::hours(1));
    WorkspaceRegistry warm;
    warm.scan(tmp.path());

    // 原地覆写(非 rename),文件 mtime 变、目录 mtime 不变。
    const auto marker = fs::path(tmp.path()) / hash / "workspace.json";
    const auto dir_mtime_before = fs::last_write_time(fs::path(tmp.path()) / hash);
    {
        std::ofstream ofs(marker.string(), std::ios::trunc);
        nlohmann::json j;
        j["cwd"] = "/tmp/a";
        j["name"] = "A rewritten in place";
        j["desktop_visible"] = true;
        ofs << j.dump();
    }
    fs::last_write_time(fs::path(tmp.path()) / hash, dir_mtime_before);

    warm.scan(tmp.path());
    ASSERT_TRUE(warm.get(hash).has_value());
    EXPECT_EQ(warm.get(hash)->name, "A") << "目录没动过时不应重读 workspace.json";

    // force_full_scan 丢掉缓存,重新读盘。
    warm.force_full_scan(tmp.path());
    ASSERT_TRUE(warm.get(hash).has_value());
    EXPECT_EQ(warm.get(hash)->name, "A rewritten in place");
}

// 触发场景:用户行内重命名 workspace,或把它从列表隐藏。
// 期望行为:立刻生效。这两条走的都是 write_workspace_json 的 tmp+rename,
// rename 会推进所在目录的 mtime,缓存据此失效;set_name/hide 另外还会主动
// 丢掉该 hash 的 probe,不把正确性押在文件系统的 mtime 行为上。
TEST(WorkspaceRegistryProbeCache, RenameAndHideTakeEffectImmediately) {
    TmpProjectsDir tmp;
    WorkspaceRegistry reg;
    auto created = reg.register_new(tmp.path(), "/home/u/cache-rename");
    reg.scan(tmp.path());
    ASSERT_TRUE(reg.get(created.hash).has_value());

    ASSERT_TRUE(reg.set_name(tmp.path(), created.hash, "Renamed"));
    reg.scan(tmp.path());
    ASSERT_TRUE(reg.get(created.hash).has_value());
    EXPECT_EQ(reg.get(created.hash)->name, "Renamed");

    ASSERT_TRUE(reg.hide(tmp.path(), created.hash));
    reg.scan(tmp.path());
    EXPECT_FALSE(reg.get(created.hash).has_value());
}

// 触发场景:一个目录没有 marker(纯 TUI 历史目录)。这类目录占了那 16568 个
// 里的绝大多数,负缓存正是省下 267ms 的关键。
// 期望行为:负缓存不能把「后来补上的 marker」挡住 —— 新建 workspace.json
// 会推进所在目录的 mtime,必须被重新读到。
TEST(WorkspaceRegistryProbeCache, MarkerAddedLaterIsStillPickedUp) {
    TmpProjectsDir tmp;
    const std::string hash = "bbbbbbbbbbbbbbbb";
    fs::create_directories(fs::path(tmp.path()) / hash);

    WorkspaceRegistry reg;
    reg.scan(tmp.path());
    ASSERT_FALSE(reg.get(hash).has_value());

    seed_workspace_json(tmp.path(), hash, "/tmp/b", "B");
    reg.scan(tmp.path());
    ASSERT_TRUE(reg.get(hash).has_value());
    EXPECT_EQ(reg.get(hash)->name, "B");
}

// 触发场景:workspace 目录被外部删掉。
// 期望行为:从注册表消失,并且它的 probe 也不能留在缓存里 —— 否则同名目录
// 再出现时会命中一条陈旧的结论。
TEST(WorkspaceRegistryProbeCache, RemovedDirectoryLeavesTheRegistry) {
    TmpProjectsDir tmp;
    const std::string hash = "cccccccccccccccc";
    seed_workspace_json(tmp.path(), hash, "/tmp/c", "C");

    WorkspaceRegistry reg;
    reg.scan(tmp.path());
    ASSERT_TRUE(reg.get(hash).has_value());

    fs::remove_all(fs::path(tmp.path()) / hash);
    reg.scan(tmp.path());
    EXPECT_FALSE(reg.get(hash).has_value());

    // 同名目录带着不同内容回来,必须读到新内容而不是旧缓存。
    seed_workspace_json(tmp.path(), hash, "/tmp/c2", "C2");
    reg.scan(tmp.path());
    ASSERT_TRUE(reg.get(hash).has_value());
    EXPECT_EQ(reg.get(hash)->name, "C2");
}
