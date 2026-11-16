// 覆盖 src/web/handlers/fs_browser_handler.cpp 的纯函数(openspec add-web-path-picker):
//   - normalize_browse_path / browse_parent_path:路径显示形态归一,不 canonical
//   - browse_directory:目录优先排序、隐藏开关、噪音目录照常列出、错误分类、条目上限、
//     Windows junction 保留原样前缀并附 link_target
//   - enumerate_roots / home_directory / desktop_directory / host_os_name
//   - folder_name_problem / create_browse_directory:选择器底栏「New Folder」建一层子目录
//
// 与 files_handler_test 的关系:这是「选择器」语义(全盘、完整路径、不过滤噪音与链接),
// 不是「文件树」语义;两套函数刻意不共享过滤逻辑,所以测试也各自独立。
// 测试均在 tmp dir 下,RAII 清理,跨平台;junction 用例只在 Windows 上跑。

#include <gtest/gtest.h>

#include "web/handlers/fs_browser_handler.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace fs = std::filesystem;

using acecode::web::browse_directory;
using acecode::web::create_browse_directory;
using acecode::web::folder_name_problem;
using acecode::web::FsMkdirError;
using acecode::web::FsMkdirErrorKind;
using acecode::web::FsMkdirResult;
using acecode::web::browse_parent_path;
using acecode::web::desktop_directory;
using acecode::web::enumerate_roots;
using acecode::web::FsBrowseEntry;
using acecode::web::FsBrowseError;
using acecode::web::FsBrowseErrorKind;
using acecode::web::FsBrowseResult;
using acecode::web::home_directory;
using acecode::web::host_os_name;
using acecode::web::normalize_browse_path;

namespace {

// RAII 临时目录:构造时建,析构时 remove_all;随机后缀避免并发测试撞名。
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("acecode_fs_browser_test_" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary);
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// 临时目录的「显示形态」路径 —— 与 browse_directory 返回的 path / 条目前缀同构。
std::string display_path(const fs::path& p) {
    auto normalized = normalize_browse_path(acecode::path_to_utf8(p));
    return normalized ? *normalized : std::string{};
}

std::string lower_ascii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const FsBrowseEntry* find_entry(const FsBrowseResult& r, const std::string& name) {
    for (const auto& e : r.entries) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

std::vector<std::string> names_of(const FsBrowseResult& r) {
    std::vector<std::string> out;
    for (const auto& e : r.entries) out.push_back(e.name);
    return out;
}

} // namespace

// 场景:用户在路径输入框里敲了空串、相对路径、纯空白。期望:一律拒绝(nullopt),
// 路由层据此回 400,而不是把相对路径拼到 daemon 的 cwd 上悄悄列出别的目录。
TEST(FsBrowserHandler, NormalizeRejectsEmptyAndRelativePaths) {
    EXPECT_FALSE(normalize_browse_path("").has_value());
    EXPECT_FALSE(normalize_browse_path("   ").has_value());
    EXPECT_FALSE(normalize_browse_path("relative/dir").has_value());
    EXPECT_FALSE(normalize_browse_path("./here").has_value());
#ifdef _WIN32
    // "C:" 没有斜杠时是「C 盘当前目录」的相对写法,"/foo" 缺盘符,两者在 Windows 上都不算绝对路径。
    EXPECT_FALSE(normalize_browse_path("C:").has_value());
    EXPECT_FALSE(normalize_browse_path("/foo").has_value());
#endif
}

// 场景:同一目录的不同写法(反斜杠、小写盘符、尾斜杠、多余的 ..)。期望:归一到同一个
// 正斜杠形态;盘根保留尾斜杠("C:/" / "/"),其它路径不带尾斜杠。全程不触盘,所以
// 不存在的路径也能归一。
TEST(FsBrowserHandler, NormalizeUnifiesSeparatorsCaseAndTrailingSlash) {
#ifdef _WIN32
    EXPECT_EQ(normalize_browse_path("c:\\users\\shao\\").value(), "C:/users/shao");
    EXPECT_EQ(normalize_browse_path("C:/Users/shao/../shao").value(), "C:/Users/shao");
    EXPECT_EQ(normalize_browse_path("c:\\").value(), "C:/");
    EXPECT_EQ(normalize_browse_path("C:/").value(), "C:/");
    EXPECT_EQ(normalize_browse_path("C:\\Users\\..").value(), "C:/");
#else
    EXPECT_EQ(normalize_browse_path("/home/me/").value(), "/home/me");
    EXPECT_EQ(normalize_browse_path("/home/me/../me").value(), "/home/me");
    EXPECT_EQ(normalize_browse_path("/").value(), "/");
    EXPECT_EQ(normalize_browse_path("/home/..").value(), "/");
#endif
}

// 场景:面包屑「上一级」。期望:普通目录返回父目录的同款显示形态;已经是盘根 / 文件系统根
// 时返回空串 —— 前端据此回到「此电脑」根节点视图,而不是再请求一次同一个盘根。
TEST(FsBrowserHandler, ParentPathStopsAtRoot) {
#ifdef _WIN32
    EXPECT_EQ(browse_parent_path("C:/Users/shao"), "C:/Users");
    EXPECT_EQ(browse_parent_path("C:/Users"), "C:/");
    EXPECT_EQ(browse_parent_path("C:/"), "");
#else
    EXPECT_EQ(browse_parent_path("/home/me"), "/home");
    EXPECT_EQ(browse_parent_path("/home"), "/");
    EXPECT_EQ(browse_parent_path("/"), "");
#endif
}

// 场景:目录里混着大小写不同的子目录与文件。期望:目录在前、文件在后,各自按名称不分
// 大小写排序(与资源管理器一致);每个条目的 path = 目录显示路径 + "/" + name,文件带
// size,所有条目带 modified_ms。
TEST(FsBrowserHandler, ListsDirectoriesFirstCaseInsensitive) {
    TempDir d;
    fs::create_directories(d.path / "b_dir");
    fs::create_directories(d.path / "A_dir");
    write_file(d.path / "z.txt", "zz");
    write_file(d.path / "a.txt", "a");
    write_file(d.path / "10.txt", "1234567890");

    const auto listed = browse_directory(acecode::path_to_utf8(d.path), false);
    ASSERT_TRUE(std::holds_alternative<FsBrowseResult>(listed));
    const auto& r = std::get<FsBrowseResult>(listed);

    EXPECT_EQ(r.path, display_path(d.path));
    EXPECT_FALSE(r.truncated);
    EXPECT_EQ(names_of(r), (std::vector<std::string>{"A_dir", "b_dir", "10.txt", "a.txt", "z.txt"}));

    const auto* a_dir = find_entry(r, "A_dir");
    ASSERT_NE(a_dir, nullptr);
    EXPECT_EQ(a_dir->kind, "dir");
    EXPECT_EQ(a_dir->path, display_path(d.path) + "/A_dir");
    EXPECT_FALSE(a_dir->size.has_value());
    EXPECT_TRUE(a_dir->modified_ms.has_value());

    const auto* ten = find_entry(r, "10.txt");
    ASSERT_NE(ten, nullptr);
    EXPECT_EQ(ten->kind, "file");
    ASSERT_TRUE(ten->size.has_value());
    EXPECT_EQ(*ten->size, 10u);
    EXPECT_FALSE(ten->hidden);
}

// 场景:目录里有点前缀的隐藏项。期望:默认不出现;show_hidden=true 时出现并标 hidden,
// 好让弹窗淡显它们。Windows 上再补一个「无点前缀但文件属性带 HIDDEN」的文件,它同样
// 只在 show_hidden 时出现 —— 资源管理器里的隐藏语义就是这个属性,不是点前缀。
TEST(FsBrowserHandler, HiddenEntriesRequireShowHidden) {
    TempDir d;
    fs::create_directories(d.path / ".hidden_dir");
    write_file(d.path / ".env", "SECRET=1");
    write_file(d.path / "visible.txt", "ok");
#ifdef _WIN32
    write_file(d.path / "attr_hidden.txt", "attr");
    ASSERT_TRUE(::SetFileAttributesW((d.path / "attr_hidden.txt").c_str(), FILE_ATTRIBUTE_HIDDEN));
#endif

    const auto without = browse_directory(acecode::path_to_utf8(d.path), false);
    ASSERT_TRUE(std::holds_alternative<FsBrowseResult>(without));
    const auto& r0 = std::get<FsBrowseResult>(without);
    EXPECT_EQ(names_of(r0), (std::vector<std::string>{"visible.txt"}));

    const auto with = browse_directory(acecode::path_to_utf8(d.path), true);
    ASSERT_TRUE(std::holds_alternative<FsBrowseResult>(with));
    const auto& r1 = std::get<FsBrowseResult>(with);
    const auto* hidden_dir = find_entry(r1, ".hidden_dir");
    ASSERT_NE(hidden_dir, nullptr);
    EXPECT_TRUE(hidden_dir->hidden);
    EXPECT_EQ(hidden_dir->kind, "dir");
    const auto* env = find_entry(r1, ".env");
    ASSERT_NE(env, nullptr);
    EXPECT_TRUE(env->hidden);
    const auto* visible = find_entry(r1, "visible.txt");
    ASSERT_NE(visible, nullptr);
    EXPECT_FALSE(visible->hidden);
#ifdef _WIN32
    const auto* attr_hidden = find_entry(r1, "attr_hidden.txt");
    ASSERT_NE(attr_hidden, nullptr);
    EXPECT_TRUE(attr_hidden->hidden);
#endif
}

// 场景:用户要把 build/ 或 node_modules/ 选成工作区(files_handler 的文件树会把它们
// 永久过滤掉)。期望:选择器照常列出这些噪音目录;.git 只受隐藏规则约束(点前缀)。
TEST(FsBrowserHandler, NoiseDirectoriesAreNotFiltered) {
    TempDir d;
    fs::create_directories(d.path / "node_modules");
    fs::create_directories(d.path / "build");
    fs::create_directories(d.path / "target");
    fs::create_directories(d.path / ".git");

    const auto listed = browse_directory(acecode::path_to_utf8(d.path), false);
    ASSERT_TRUE(std::holds_alternative<FsBrowseResult>(listed));
    const auto& r = std::get<FsBrowseResult>(listed);
    EXPECT_EQ(names_of(r), (std::vector<std::string>{"build", "node_modules", "target"}));

    const auto with_hidden = browse_directory(acecode::path_to_utf8(d.path), true);
    ASSERT_TRUE(std::holds_alternative<FsBrowseResult>(with_hidden));
    EXPECT_NE(find_entry(std::get<FsBrowseResult>(with_hidden), ".git"), nullptr);
}

// 场景:三类坏输入。期望:相对路径 → NotAbsolute(400);不存在 → NotFound(404);
// 指向文件 → NotDirectory(404)。路由层按 kind 映射状态码,前端按状态码给内联提示。
TEST(FsBrowserHandler, ClassifiesErrors) {
    TempDir d;
    write_file(d.path / "plain.txt", "x");

    const auto relative = browse_directory("relative/path", false);
    ASSERT_TRUE(std::holds_alternative<FsBrowseError>(relative));
    EXPECT_EQ(std::get<FsBrowseError>(relative).kind, FsBrowseErrorKind::NotAbsolute);

    const auto missing = browse_directory(acecode::path_to_utf8(d.path / "does-not-exist"), false);
    ASSERT_TRUE(std::holds_alternative<FsBrowseError>(missing));
    EXPECT_EQ(std::get<FsBrowseError>(missing).kind, FsBrowseErrorKind::NotFound);

    const auto file = browse_directory(acecode::path_to_utf8(d.path / "plain.txt"), false);
    ASSERT_TRUE(std::holds_alternative<FsBrowseError>(file));
    EXPECT_EQ(std::get<FsBrowseError>(file).kind, FsBrowseErrorKind::NotDirectory);
}

// 场景:目录子项数量超过上限(线上 5000,这里把上限调成 3 免得建几千个文件)。
// 期望:只返回上限内的条目并标 truncated;上限内的目录不标。隐藏项被过滤掉时不占名额。
TEST(FsBrowserHandler, TruncatesAtMaxEntries) {
    TempDir d;
    for (int i = 0; i < 5; ++i) write_file(d.path / ("f" + std::to_string(i) + ".txt"), "x");
    write_file(d.path / ".skipped", "hidden");

    const auto capped = browse_directory(acecode::path_to_utf8(d.path), false, 3);
    ASSERT_TRUE(std::holds_alternative<FsBrowseResult>(capped));
    const auto& r = std::get<FsBrowseResult>(capped);
    EXPECT_TRUE(r.truncated);
    EXPECT_EQ(r.entries.size(), 3u);

    const auto full = browse_directory(acecode::path_to_utf8(d.path), false);
    ASSERT_TRUE(std::holds_alternative<FsBrowseResult>(full));
    EXPECT_FALSE(std::get<FsBrowseResult>(full).truncated);
    EXPECT_EQ(std::get<FsBrowseResult>(full).entries.size(), 5u);
}

// 场景:同一目录用反斜杠 / 小写盘符 / 带尾斜杠三种写法请求。期望:都列出同一目录,
// 且返回的 path 与条目前缀都是归一后的同一个正斜杠形态 —— 前端拿 path 定位面包屑,
// 拿条目 path 直接交给注册工作区接口,形态必须稳定。
TEST(FsBrowserHandler, PathSpellingsResolveToSameListing) {
    TempDir d;
    fs::create_directories(d.path / "child");
    const std::string canonical_display = display_path(d.path);

    std::vector<std::string> spellings;
    spellings.push_back(acecode::path_to_utf8(d.path) + "/");
#ifdef _WIN32
    {
        std::string back = acecode::path_to_utf8(d.path);
        std::replace(back.begin(), back.end(), '/', '\\');
        spellings.push_back(back + "\\");
        std::string lowered_drive = back;
        lowered_drive[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(lowered_drive[0])));
        spellings.push_back(lowered_drive);
    }
#endif
    for (const auto& spelling : spellings) {
        const auto listed = browse_directory(spelling, false);
        ASSERT_TRUE(std::holds_alternative<FsBrowseResult>(listed)) << spelling;
        const auto& r = std::get<FsBrowseResult>(listed);
        EXPECT_EQ(r.path, canonical_display) << spelling;
        const auto* child = find_entry(r, "child");
        ASSERT_NE(child, nullptr) << spelling;
        EXPECT_EQ(child->path, canonical_display + "/child") << spelling;
    }
}

#ifdef _WIN32
// 场景(本机实况):C:\Users\shao 是指向 N:\Users\shao 的 junction。期望:
//   1. 列父目录时 junction 条目照常出现,kind=dir,link_target 指向真实目标;
//   2. 条目 path 保持父目录的原样前缀(不被替换成目标盘符);
//   3. 浏览 junction 自身时 result.path 仍是链接路径,而列出的却是目标目录的子项 ——
//      这样用户一路点进去再确认,拿到的是 C:/... 写法,与原生对话框一致。
// mklink /J 不需要管理员权限;环境里没有 cmd 时跳过。
TEST(FsBrowserHandler, JunctionKeepsOriginalPrefixAndReportsTarget) {
    TempDir d;
    const fs::path target = d.path / "target";
    fs::create_directories(target / "inside");
    const fs::path link = d.path / "link";
    const std::string cmd = "cmd /c mklink /J \"" + acecode::path_to_utf8(link) + "\" \"" +
                            acecode::path_to_utf8(target) + "\" >nul 2>&1";
    if (std::system(cmd.c_str()) != 0 || !fs::exists(link)) {
        GTEST_SKIP() << "mklink /J unavailable in this environment";
    }

    const auto parent = browse_directory(acecode::path_to_utf8(d.path), false);
    ASSERT_TRUE(std::holds_alternative<FsBrowseResult>(parent));
    const auto& pr = std::get<FsBrowseResult>(parent);
    const auto* entry = find_entry(pr, "link");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->kind, "dir");
    EXPECT_EQ(entry->path, display_path(d.path) + "/link");
    ASSERT_TRUE(entry->link_target.has_value());
    EXPECT_EQ(lower_ascii(*entry->link_target), lower_ascii(display_path(target)));

    const auto inner = browse_directory(display_path(d.path) + "/link", false);
    ASSERT_TRUE(std::holds_alternative<FsBrowseResult>(inner));
    const auto& ir = std::get<FsBrowseResult>(inner);
    EXPECT_EQ(ir.path, display_path(d.path) + "/link");
    const auto* inside = find_entry(ir, "inside");
    ASSERT_NE(inside, nullptr);
    EXPECT_EQ(inside->path, display_path(d.path) + "/link/inside");
}
#endif

// 场景:弹窗打开时请求根节点。期望:至少一个根;每个根都是正斜杠绝对路径(Windows 形如
// "C:/"),drive_type 非空;主目录非空且不带尾斜杠;操作系统名是三个已知值之一。
TEST(FsBrowserHandler, RootsAreAbsoluteForwardSlashPaths) {
    const auto roots = enumerate_roots();
    ASSERT_FALSE(roots.empty());
    for (const auto& r : roots) {
        EXPECT_EQ(r.path.find('\\'), std::string::npos) << r.path;
        EXPECT_TRUE(acecode::path_from_utf8(r.path).is_absolute()) << r.path;
        EXPECT_FALSE(r.drive_type.empty()) << r.path;
#ifdef _WIN32
        ASSERT_EQ(r.path.size(), 3u) << r.path;
        EXPECT_TRUE(std::isupper(static_cast<unsigned char>(r.path[0]))) << r.path;
        EXPECT_EQ(r.path.substr(1), ":/") << r.path;
#endif
    }

    const std::string home = home_directory();
    ASSERT_FALSE(home.empty());
    EXPECT_EQ(home.find('\\'), std::string::npos);
    EXPECT_TRUE(home.size() <= 3 || home.back() != '/') << home;

    const std::string os = host_os_name();
    EXPECT_TRUE(os == "windows" || os == "macos" || os == "linux") << os;
}

// 场景:主目录下没有 Desktop 子目录(服务器账号常见)。期望:桌面快捷入口省略而不是给一个
// 点进去 404 的路径;建出 Desktop 后再查则返回它的显示形态路径;空 home 直接省略。
TEST(FsBrowserHandler, DesktopDirectoryIsOmittedWhenMissing) {
    TempDir fake_home;
    const std::string home = display_path(fake_home.path);
    EXPECT_FALSE(desktop_directory(home).has_value());
    EXPECT_FALSE(desktop_directory("").has_value());

    fs::create_directories(fake_home.path / "Desktop");
    const auto desktop = desktop_directory(home);
    ASSERT_TRUE(desktop.has_value());
    EXPECT_EQ(*desktop, home + "/Desktop");
}

namespace {

FsMkdirErrorKind mkdir_error_kind(const std::variant<FsMkdirResult, FsMkdirError>& r) {
    return std::get<FsMkdirError>(r).kind;
}

} // namespace

// 场景:用户在选择器里点「New Folder」,输入框里的名字带首尾空格、是中文。期望:去掉首尾空格
// 后在 parent 下建出这一层目录;返回的 path 是 parent 的显示形态 + "/" + 名字(正斜杠、
// 不 canonical),name 是去空格后的实际名字。中文名同时守住 UTF-8 → 宽字符路径的转换。
TEST(FsBrowserHandler, MkdirCreatesOneLevelAndReturnsDisplayPath) {
    TempDir d;
    const auto made = create_browse_directory(acecode::path_to_utf8(d.path), "  新建文件夹  ");
    ASSERT_TRUE(std::holds_alternative<FsMkdirResult>(made));
    const auto& r = std::get<FsMkdirResult>(made);
    EXPECT_EQ(r.name, "新建文件夹");
    EXPECT_EQ(r.path, display_path(d.path) + "/新建文件夹");
    EXPECT_TRUE(fs::is_directory(acecode::path_from_utf8(r.path)));

    // 新目录随即出现在列表里,前端据此重读当前目录并选中它。
    const auto listed = browse_directory(display_path(d.path), false);
    ASSERT_TRUE(std::holds_alternative<FsBrowseResult>(listed));
    ASSERT_NE(find_entry(std::get<FsBrowseResult>(listed), "新建文件夹"), nullptr);
}

// 场景:同名的目录或文件已经存在(Windows 上只差大小写也算同名)。期望:AlreadyExists(→ 409),
// 不覆盖、不改动已有内容;已有文件仍是文件。
TEST(FsBrowserHandler, MkdirRefusesExistingNames) {
    TempDir d;
    fs::create_directories(d.path / "taken");
    write_file(d.path / "note.txt", "keep");
    const std::string parent = acecode::path_to_utf8(d.path);

    EXPECT_EQ(mkdir_error_kind(create_browse_directory(parent, "taken")),
              FsMkdirErrorKind::AlreadyExists);
    EXPECT_EQ(mkdir_error_kind(create_browse_directory(parent, "note.txt")),
              FsMkdirErrorKind::AlreadyExists);
    EXPECT_TRUE(fs::is_regular_file(d.path / "note.txt"));
#ifdef _WIN32
    EXPECT_EQ(mkdir_error_kind(create_browse_directory(parent, "TAKEN")),
              FsMkdirErrorKind::AlreadyExists);
#endif
}

// 场景:parent 是相对路径、不存在、或其实是个文件。期望:分别是 NotAbsolute / ParentNotFound /
// ParentNotDirectory;不存在的 parent **不会**被顺手递归建出来(只建一层)。
TEST(FsBrowserHandler, MkdirClassifiesParentErrors) {
    TempDir d;
    write_file(d.path / "file.txt", "x");

    EXPECT_EQ(mkdir_error_kind(create_browse_directory("relative/dir", "a")),
              FsMkdirErrorKind::NotAbsolute);
    EXPECT_EQ(mkdir_error_kind(create_browse_directory("", "a")),
              FsMkdirErrorKind::NotAbsolute);

    const fs::path missing = d.path / "missing";
    EXPECT_EQ(mkdir_error_kind(create_browse_directory(acecode::path_to_utf8(missing), "a")),
              FsMkdirErrorKind::ParentNotFound);
    EXPECT_FALSE(fs::exists(missing));

    EXPECT_EQ(mkdir_error_kind(create_browse_directory(
                  acecode::path_to_utf8(d.path / "file.txt"), "a")),
              FsMkdirErrorKind::ParentNotDirectory);
}

// 场景:名字非法。期望:InvalidName(→ 400)且磁盘上什么都没建;名字非法优先于 parent
// 是否存在被判定。
TEST(FsBrowserHandler, MkdirRejectsInvalidNamesWithoutTouchingDisk) {
    TempDir d;
    const std::string parent = acecode::path_to_utf8(d.path);
    for (const std::string& bad : std::vector<std::string>{"", "   ", ".", "..", "a/b", "a\\b", "tab\there"}) {
        EXPECT_EQ(mkdir_error_kind(create_browse_directory(parent, bad)),
                  FsMkdirErrorKind::InvalidName) << bad;
    }
    EXPECT_TRUE(fs::is_empty(d.path));
}

// 场景:名字规则按平台不同。期望:
//   - 所有平台:空串、. / ..、分隔符、控制字符不合法;中文、空格夹在中间、点开头合法;
//   - Windows 规则另外拒绝 <>:"|?* 、句点或空格结尾(Windows 会静默剥掉,建出来的名字对不上)、
//     保留设备名(带扩展名、大小写不同也算:"con.txt"、"Lpt9");
//   - POSIX 规则下这些 Windows 专属限制都放行("a:b"、"CON" 在 Linux 上是普通名字)。
// "COM0" / "COM10" 不是保留名,守住只认 1..9 的边界。
TEST(FsBrowserHandler, FolderNameRulesFollowPlatform) {
    for (const bool win : {true, false}) {
        EXPECT_FALSE(folder_name_problem("", win).empty());
        EXPECT_FALSE(folder_name_problem(".", win).empty());
        EXPECT_FALSE(folder_name_problem("..", win).empty());
        EXPECT_FALSE(folder_name_problem("a/b", win).empty());
        EXPECT_FALSE(folder_name_problem("a\\b", win).empty());
        EXPECT_FALSE(folder_name_problem(std::string("a\x01" "b"), win).empty());
        EXPECT_TRUE(folder_name_problem("新建文件夹 (2)", win).empty());
        EXPECT_TRUE(folder_name_problem(".config", win).empty());
        EXPECT_TRUE(folder_name_problem("COM0", win).empty());
        EXPECT_TRUE(folder_name_problem("COM10", win).empty());
    }
    for (const char* bad : {"a<b", "a>b", "a:b", "a\"b", "a|b", "a?b", "a*b",
                            "trailing.", "trailing ", "CON", "con.txt", "Lpt9", "nul", "AUX"}) {
        EXPECT_FALSE(folder_name_problem(bad, true).empty()) << bad;
    }
    for (const char* ok : {"a:b", "trailing.", "CON", "a*b"}) {
        EXPECT_TRUE(folder_name_problem(ok, false).empty()) << ok;
    }
}

#ifdef _WIN32
// 场景:用户经 junction 进入目录后点「New Folder」(本机 C:\Users\shao → N:\Users\shao)。
// 期望:返回的 path 保留链接写法(不被换成目标盘符),这样随后点「选择」注册的工作区与原生
// 对话框一致;目录实际落在 junction 目标里。
TEST(FsBrowserHandler, MkdirThroughJunctionKeepsLinkSpelling) {
    TempDir d;
    const fs::path target = d.path / "target";
    fs::create_directories(target);
    const fs::path link = d.path / "link";
    const std::string cmd = "cmd /c mklink /J \"" + acecode::path_to_utf8(link) + "\" \"" +
                            acecode::path_to_utf8(target) + "\" >nul 2>&1";
    if (std::system(cmd.c_str()) != 0 || !fs::exists(link)) {
        GTEST_SKIP() << "mklink /J unavailable in this environment";
    }

    const auto made = create_browse_directory(display_path(d.path) + "/link", "fresh");
    ASSERT_TRUE(std::holds_alternative<FsMkdirResult>(made));
    EXPECT_EQ(std::get<FsMkdirResult>(made).path, display_path(d.path) + "/link/fresh");
    EXPECT_TRUE(fs::is_directory(target / "fresh"));
}
#endif
