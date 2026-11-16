#pragma once

// /api/fs 的纯函数业务逻辑 — Web「选择文件 / 文件夹」弹窗(openspec add-web-path-picker)
// 的后端。它与 files_handler(/api/files)的四条语义刻意相反,所以是独立模块而不是
// 在旧端点上加开关:
//
//   files_handler(/api/files)              fs_browser_handler(/api/fs)
//   cwd 必须在 workspace 白名单内          已鉴权即可浏览任意绝对目录
//   返回相对 cwd 的路径                    返回完整路径(正斜杠)
//   噪音目录(node_modules/build)恒过滤     不过滤 —— 选择器场景下用户可能就是要选它们
//   junction / 符号链接目录恒过滤          照常列出并附 link_target
//
// 路径只做词法归一(lexically_normal + 正斜杠 + 盘符大写),**不 canonical**:用户经
// C:\Users\shao(本机是指向 N:\Users\shao 的 junction)进入后,交回的仍是
// C:/Users/shao/...,与 Desktop 原生对话框的返回一致,compute_cwd_hash 才会落到同一个
// workspace;weakly_canonical 会把盘符改掉,两次选同一目录就注册出两个工作区。
//
// 隐藏 = 名称以 '.' 开头,或(Windows)文件属性带 HIDDEN / SYSTEM;show_hidden 一起
// 放行并在条目上标 hidden=true。单次列举上限 kFsBrowseMaxEntries,超出置 truncated。
// 模块无 Crow 依赖,routes_fs.cpp 只做 HTTP 解析 + 序列化。
//
// 唯一的写操作是选择器底栏「New Folder」用的 create_browse_directory:在一个已存在的目录
// 下建一层子目录。返回路径沿用同样的词法归一、不 canonical(理由同上);名字不合法直接
// 拒绝,不替用户改名;不递归建父目录,不覆盖已有项目。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace acecode::web {

constexpr std::size_t kFsBrowseMaxEntries = 5000;

struct FsBrowseEntry {
    std::string name;
    std::string path;   // 完整路径:父目录的归一路径 + "/" + name(不解析链接)
    std::string kind;   // "dir" | "file"
    bool hidden = false;
    std::optional<std::uint64_t> size;         // 仅 kind="file"
    std::optional<std::int64_t>  modified_ms;  // unix epoch ms
    std::optional<std::string>   link_target;  // junction / 符号链接目录的目标;取不到则省略
};

struct FsBrowseResult {
    std::string path;    // 归一后的目录路径(正斜杠,盘根保留尾斜杠:"C:/" / "/")
    std::string parent;  // 父目录;path 已是根时为空串
    bool truncated = false;
    std::vector<FsBrowseEntry> entries;
};

enum class FsBrowseErrorKind {
    NotAbsolute,   // 空串 / 相对路径 → HTTP 400
    NotFound,      // 不存在 → HTTP 404
    NotDirectory,  // 存在但不是目录 → HTTP 404
    AccessDenied,  // 权限拒绝 → HTTP 403
    IoError,       // 其他读盘失败 → HTTP 500
};

struct FsBrowseError {
    FsBrowseErrorKind kind;
    std::string message;  // 给日志 / body.detail
};

// 把用户给的路径归一成显示形态:反斜杠 → 正斜杠、lexically_normal、Windows 盘符大写、
// 剥尾斜杠(盘根除外)。**不**触盘、**不** canonical。空串或相对路径 → nullopt。
std::optional<std::string> normalize_browse_path(const std::string& path_utf8);

// normalized 的父目录(同样的显示形态);normalized 已是根("C:/" / "/")→ 空串。
std::string browse_parent_path(const std::string& normalized);

// 列出 path_utf8 的直接子项(目录 + 文件),不递归。见文件头注释的语义表。
// max_entries 只在单测里改小;线上恒为 kFsBrowseMaxEntries。
std::variant<FsBrowseResult, FsBrowseError>
browse_directory(const std::string& path_utf8,
                 bool show_hidden,
                 std::size_t max_entries = kFsBrowseMaxEntries);

enum class FsMkdirErrorKind {
    InvalidName,         // 空名 / . / .. / 含分隔符或控制字符;Windows 另含 <>:"|?* 、句点结尾、保留设备名 → 400
    NotAbsolute,         // parent 为空串 / 相对路径 → 400
    ParentNotFound,      // parent 不存在 → 404
    ParentNotDirectory,  // parent 存在但不是目录 → 404
    AccessDenied,        // 权限拒绝 → 403
    AlreadyExists,       // 同名项目(目录或文件)已存在 → 409
    IoError,             // 其他失败(名字过长、磁盘只读等)→ 500
};

struct FsMkdirError {
    FsMkdirErrorKind kind;
    std::string message;  // 给日志 / body.detail
};

struct FsMkdirResult {
    std::string path;  // 新目录的显示形态路径:parent 的归一路径 + "/" + name
    std::string name;  // 实际使用的名字(去掉首尾空白之后)
};

// 名字的问题描述;合法返回空串。名字两端的 ASCII 空白由调用方先去掉。
// windows_rules 只在单测里显式传入,线上按编译平台决定。
std::string folder_name_problem(const std::string& name, bool windows_rules);
bool folder_name_uses_windows_rules();

// 在 parent_utf8 下建名为 name_utf8 的子目录(只建一层)。见文件头注释。
std::variant<FsMkdirResult, FsMkdirError>
create_browse_directory(const std::string& parent_utf8, const std::string& name_utf8);

struct FsRootEntry {
    std::string path;        // "C:/" | "/" | "/Volumes/Data"
    std::string label;       // 卷标;取不到为空,前端自行组合「本地磁盘 (C:)」这类文案
    std::string drive_type;  // fixed | removable | remote | cdrom | ramdisk | root | volume
    std::optional<std::uint64_t> total_bytes;
    std::optional<std::uint64_t> free_bytes;
};

// Windows:每个逻辑盘符(NO_ROOT_DIR / UNKNOWN 跳过,远程盘不查卷标与容量);
// POSIX:"/";macOS 追加 /Volumes 下的直接子目录。枚举期间屏蔽系统的
// 「驱动器中没有磁盘」模态框,任何单盘失败只影响该盘的可选字段。
std::vector<FsRootEntry> enumerate_roots();

std::string host_name();                                       // 取不到为空
std::string host_os_name();                                    // "windows" | "macos" | "linux"
std::string home_directory();                                  // 归一形态;取不到为空
std::optional<std::string> desktop_directory(const std::string& home);  // home/Desktop 存在时

} // namespace acecode::web
