// 覆盖「编辑项目」(多文件夹项目)的持久化:WorkspaceRegistry::update_profile、
// load_workspace_folders、normalize_workspace_extra_folders、is_valid_workspace_icon。
// workspace.json 同时被 Desktop 进程与 daemon 进程各自的注册表缓存读写,所以这里
// 专门覆盖「另一个实例写过的字段不能被本实例的旧缓存冲掉」。全部写盘走 tmp dir。

#include <gtest/gtest.h>

#include "workspace/workspace_registry.hpp"
#include "utils/cwd_hash.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using acecode::compute_cwd_hash;
using acecode::path_to_utf8;
using acecode::desktop::WorkspaceIcon;
using acecode::desktop::WorkspaceMeta;
using acecode::desktop::WorkspaceProfileStatus;
using acecode::desktop::WorkspaceProfileUpdate;
using acecode::desktop::WorkspaceRegistry;
using acecode::desktop::is_valid_workspace_icon;
using acecode::desktop::load_workspace_folders;
using acecode::desktop::load_workspace_metadata;
using acecode::desktop::normalize_workspace_extra_folders;

namespace {

// 每个用例一棵独立的临时树:projects/ 放 workspace.json,main/ 是主文件夹,
// shared_lib/ 与 docs/ 是可添加的附加文件夹。
class ProfileTree {
public:
    ProfileTree() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() /
            ("acecode_workspace_profile_test_" + std::to_string(stamp));
        fs::remove_all(root_);
        fs::create_directories(root_ / "projects");
        fs::create_directories(root_ / "main");
        fs::create_directories(root_ / "shared_lib");
        fs::create_directories(root_ / "docs");
    }
    ~ProfileTree() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    std::string projects() const { return path_to_utf8(root_ / "projects"); }
    std::string main() const { return path_to_utf8(root_ / "main"); }
    std::string lib() const { return path_to_utf8(root_ / "shared_lib"); }
    std::string docs() const { return path_to_utf8(root_ / "docs"); }
    fs::path root() const { return root_; }

private:
    fs::path root_;
};

nlohmann::json read_marker(const std::string& projects_dir, const std::string& hash) {
    std::ifstream in(fs::path(projects_dir) / hash / "workspace.json");
    std::stringstream buffer;
    buffer << in.rdbuf();
    return nlohmann::json::parse(buffer.str());
}

WorkspaceProfileUpdate make_update(std::string name,
                                   WorkspaceIcon icon,
                                   std::vector<std::string> folders) {
    WorkspaceProfileUpdate update;
    update.name = std::move(name);
    update.icon = std::move(icon);
    update.extra_folders = std::move(folders);
    return update;
}

} // namespace

// 场景:新注册的项目从没编辑过。
// 期望:workspace.json 里不出现 icon / extra_folders 键 —— 老版本读到的文件与改动前
// 完全一致,未设置 = 省略。
TEST(WorkspaceProfile, FreshMarkerOmitsProfileFields) {
    ProfileTree tree;
    WorkspaceRegistry registry;
    const auto meta = registry.register_new(tree.projects(), tree.main());
    const auto marker = read_marker(tree.projects(), meta.hash);
    EXPECT_FALSE(marker.contains("icon"));
    EXPECT_FALSE(marker.contains("extra_folders"));
    EXPECT_TRUE(meta.icon.empty());
    EXPECT_TRUE(meta.extra_folders.empty());
}

// 场景:「编辑项目」保存名称 + 图标 + 两个附加文件夹。
// 期望:落盘;另一个注册表实例 scan 后读到同样的值;load_workspace_metadata 同样可见。
TEST(WorkspaceProfile, UpdatePersistsNameIconAndFolders) {
    ProfileTree tree;
    WorkspaceRegistry registry;
    const auto meta = registry.register_new(tree.projects(), tree.main());

    WorkspaceMeta saved;
    std::string error;
    const auto status = registry.update_profile(
        tree.projects(), meta.hash,
        make_update(u8"新名字", {"music", "blue"}, {tree.lib(), tree.docs()}),
        &saved, error);
    ASSERT_EQ(status, WorkspaceProfileStatus::Saved) << error;
    EXPECT_EQ(saved.name, u8"新名字");
    EXPECT_EQ(saved.icon.id, "music");
    EXPECT_EQ(saved.icon.color, "blue");
    EXPECT_EQ(saved.extra_folders, (std::vector<std::string>{tree.lib(), tree.docs()}));

    WorkspaceRegistry other;
    other.scan(tree.projects());
    const auto listed = other.get(meta.hash);
    ASSERT_TRUE(listed.has_value());
    EXPECT_EQ(listed->name, u8"新名字");
    EXPECT_EQ(listed->icon, (WorkspaceIcon{"music", "blue"}));
    EXPECT_EQ(listed->extra_folders, saved.extra_folders);

    const auto loaded = load_workspace_metadata(tree.projects(), meta.hash);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->extra_folders, saved.extra_folders);
}

// 场景:用户把主文件夹本身、重复路径(尾部斜杠 / 反斜杠形态)都加进来了。
// 期望:静默去重,只剩一项;顺序保持第一次出现的位置。
TEST(WorkspaceProfile, UpdateDedupesMainAndDuplicateFolders) {
    ProfileTree tree;
    WorkspaceRegistry registry;
    const auto meta = registry.register_new(tree.projects(), tree.main());

    std::string lib_variant = tree.lib() + "/";
#ifdef _WIN32
    for (auto& c : lib_variant) if (c == '/') c = '\\';
#endif
    WorkspaceMeta saved;
    std::string error;
    ASSERT_EQ(registry.update_profile(
                  tree.projects(), meta.hash,
                  make_update("main", {}, {tree.main(), tree.lib(), lib_variant}),
                  &saved, error),
              WorkspaceProfileStatus::Saved) << error;
    EXPECT_EQ(saved.extra_folders, (std::vector<std::string>{tree.lib()}));
}

// 场景:请求里带相对路径或已不存在的目录(REST 可被直接调用,绕过前端选择器)。
// 期望:Invalid + 原因;磁盘上的名称与附加文件夹都不变。
TEST(WorkspaceProfile, UpdateRejectsRelativeOrMissingFolderWithoutWriting) {
    ProfileTree tree;
    WorkspaceRegistry registry;
    const auto meta = registry.register_new(tree.projects(), tree.main());

    std::string error;
    EXPECT_EQ(registry.update_profile(tree.projects(), meta.hash,
                  make_update("x", {}, {"relative/dir"}), nullptr, error),
              WorkspaceProfileStatus::Invalid);
    EXPECT_NE(error.find("absolute"), std::string::npos);

    error.clear();
    EXPECT_EQ(registry.update_profile(tree.projects(), meta.hash,
                  make_update("x", {}, {path_to_utf8(tree.root() / "gone")}), nullptr, error),
              WorkspaceProfileStatus::Invalid);
    EXPECT_NE(error.find("does not exist"), std::string::npos);

    const auto marker = read_marker(tree.projects(), meta.hash);
    EXPECT_EQ(marker["name"], meta.name);
    EXPECT_FALSE(marker.contains("extra_folders"));
}

// 场景:空名称、非法图标字符(大写 / 路径符号)。期望:Invalid,不写盘。
TEST(WorkspaceProfile, UpdateRejectsEmptyNameAndInvalidIcon) {
    ProfileTree tree;
    WorkspaceRegistry registry;
    const auto meta = registry.register_new(tree.projects(), tree.main());
    std::string error;
    EXPECT_EQ(registry.update_profile(tree.projects(), meta.hash,
                  make_update("", {}, {}), nullptr, error),
              WorkspaceProfileStatus::Invalid);
    EXPECT_EQ(registry.update_profile(tree.projects(), meta.hash,
                  make_update("x", {"../evil", "red"}, {}), nullptr, error),
              WorkspaceProfileStatus::Invalid);
    EXPECT_EQ(read_marker(tree.projects(), meta.hash)["name"], meta.name);
}

// 场景:hash 目录名与 marker 里的 cwd 对不上(手工拷贝的目录,或拼出来的 hash)。
// 期望:NotFound —— 不能往一个身份不符的目录写 workspace.json。
TEST(WorkspaceProfile, UpdateRejectsHashThatDoesNotMatchCwd) {
    ProfileTree tree;
    const std::string bogus = "0000000000000000";
    fs::create_directories(fs::path(tree.projects()) / bogus);
    {
        std::ofstream out(fs::path(tree.projects()) / bogus / "workspace.json");
        out << nlohmann::json{{"cwd", tree.main()}, {"name", "x"}, {"desktop_visible", true}}.dump();
    }
    ASSERT_NE(compute_cwd_hash(tree.main()), bogus);
    WorkspaceRegistry registry;
    registry.scan(tree.projects());
    std::string error;
    EXPECT_EQ(registry.update_profile(tree.projects(), bogus,
                  make_update("y", {}, {}), nullptr, error),
              WorkspaceProfileStatus::NotFound);
}

// 回归场景:Desktop 与 daemon 各持一份注册表缓存。daemon 经「编辑项目」写入图标与
// 附加文件夹后,Desktop 用自己更早的缓存执行「重命名项目」/「从项目列表移除」。
// 期望:图标与附加文件夹保留。bug 表现:改名一次,附加文件夹与图标就消失了。
TEST(WorkspaceProfile, RenameAndHideFromStaleRegistryPreserveProfile) {
    ProfileTree tree;
    WorkspaceRegistry desktop;
    const auto meta = desktop.register_new(tree.projects(), tree.main());

    WorkspaceRegistry daemon;
    daemon.scan(tree.projects());
    std::string error;
    ASSERT_EQ(daemon.update_profile(tree.projects(), meta.hash,
                  make_update("edited", {"brain", "purple"}, {tree.lib()}), nullptr, error),
              WorkspaceProfileStatus::Saved) << error;

    ASSERT_TRUE(desktop.set_name(tree.projects(), meta.hash, "renamed"));
    auto marker = read_marker(tree.projects(), meta.hash);
    EXPECT_EQ(marker["name"], "renamed");
    EXPECT_EQ(marker["icon"]["id"], "brain");
    EXPECT_EQ(marker["extra_folders"], nlohmann::json::array({tree.lib()}));

    ASSERT_TRUE(desktop.hide(tree.projects(), meta.hash));
    marker = read_marker(tree.projects(), meta.hash);
    EXPECT_FALSE(marker["desktop_visible"].get<bool>());
    EXPECT_EQ(marker["icon"]["color"], "purple");
    EXPECT_EQ(marker["extra_folders"], nlohmann::json::array({tree.lib()}));
}

// 场景:会话侧按 project dir 读附加文件夹,其中一个目录后来被删掉了。
// 期望:主文件夹原样返回;已删除的附加文件夹被过滤(不再告诉模型、不再放行写入);
// project dir 下没有 workspace.json 时两者皆空。
TEST(WorkspaceProfile, LoadWorkspaceFoldersSkipsDeletedFolders) {
    ProfileTree tree;
    WorkspaceRegistry registry;
    const auto meta = registry.register_new(tree.projects(), tree.main());
    std::string error;
    ASSERT_EQ(registry.update_profile(tree.projects(), meta.hash,
                  make_update("p", {}, {tree.lib(), tree.docs()}), nullptr, error),
              WorkspaceProfileStatus::Saved) << error;
    fs::remove_all(tree.root() / "docs");

    const auto folders = load_workspace_folders(path_to_utf8(fs::path(tree.projects()) / meta.hash));
    EXPECT_EQ(folders.main_folder, tree.main());
    EXPECT_EQ(folders.extra_folders, (std::vector<std::string>{tree.lib()}));

    const auto none = load_workspace_folders(path_to_utf8(fs::path(tree.projects()) / "ffffffffffffffff"));
    EXPECT_TRUE(none.main_folder.empty());
    EXPECT_TRUE(none.extra_folders.empty());
}

// 场景:规范化附加文件夹时超过上限。期望:Invalid 并说明上限,不静默截断。
TEST(WorkspaceProfile, NormalizeRejectsTooManyFolders) {
    ProfileTree tree;
    std::vector<std::string> input;
    for (int i = 0; i < 33; ++i) {
        const auto dir = tree.root() / ("extra_" + std::to_string(i));
        fs::create_directories(dir);
        input.push_back(path_to_utf8(dir));
    }
    std::vector<std::string> output;
    std::string error;
    EXPECT_FALSE(normalize_workspace_extra_folders(tree.main(), input, output, error));
    EXPECT_NE(error.find("max 32"), std::string::npos);
}

// 场景:图标字段字符集。期望:与前端 lib/workspaceIcons.js 的键约定一致;
// 空 id 只能配空 color(= 未设置)。
TEST(WorkspaceProfile, IconCharsetValidation) {
    EXPECT_TRUE(is_valid_workspace_icon({}));
    EXPECT_TRUE(is_valid_workspace_icon({"graduation-cap", "default"}));
    EXPECT_TRUE(is_valid_workspace_icon({"music", ""}));
    EXPECT_FALSE(is_valid_workspace_icon({"", "red"}));
    EXPECT_FALSE(is_valid_workspace_icon({"Music", "red"}));
    EXPECT_FALSE(is_valid_workspace_icon({"a/b", "red"}));
    EXPECT_FALSE(is_valid_workspace_icon({std::string(41, 'a'), "red"}));
}
