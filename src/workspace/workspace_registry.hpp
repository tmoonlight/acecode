#pragma once

// desktop 端的 workspace 注册表。每个"workspace"对应一个 cwd 目录,在磁盘上
// 落地为 .acecode/projects/<cwd_hash>/workspace.json。
//
// 文件格式:
//   { "cwd": "<absolute-path>", "name": "<display-name>", "desktop_visible": true|false,
//     "icon": {"id": "<icon-id>", "color": "<color-key>"},      // 可选,缺省 = 默认文件夹图标
//     "extra_folders": ["<absolute-path>", ...] }               // 可选,「编辑项目」里添加的附加文件夹
//
// cwd 是主文件夹,不可改;extra_folders 是附加工作目录 —— AgentLoop 每回合从会话
// 的 project dir 读它(load_workspace_folders),系统提示列出、文件工具与 bash 沙箱
// 把它们当作可写根。
// name 可由用户行内重命名;缺失或损坏时回退到 fs::path(cwd).filename()(再不济
// 用 root_name 与字面常量"workspace")。
//
// scan() 只读取 desktop_visible=true 的 workspace.json。缺失 marker 的老 TUI
// history 与显式 desktop_visible=false 的目录都不会出现在 Desktop startup 列表。
//
// 所有操作线程安全,内部用 std::mutex 保护 entries_。所有写盘走 atomic_write
// (.tmp + rename),失败时内存 cache 回滚。

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode::desktop {

// 侧栏项目行的自定义图标。id 与 color 只是前端图标表 / 色板的键,后端只校验
// 字符集;id 为空 = 未设置,前端显示默认的展开 / 折叠文件夹图标。
struct WorkspaceIcon {
    std::string id;
    std::string color;
    bool empty() const { return id.empty(); }
    bool operator==(const WorkspaceIcon& other) const {
        return id == other.id && color == other.color;
    }
};

struct WorkspaceMeta {
    std::string hash; // 16 位 hex,= compute_cwd_hash(cwd)
    std::string cwd;  // 绝对路径,正斜杠规范化前的原始字符串
    std::string name; // 显示名;用户未指定时 = default_workspace_name(cwd)
    bool desktop_visible = false; // true = Desktop startup may list this workspace
    WorkspaceIcon icon;
    std::vector<std::string> extra_folders; // 附加工作目录(绝对路径,已去重)
};

enum class WorkspaceOrderStatus {
    Saved,
    InvalidOrder,
    Conflict,
    WriteFailed,
};

// 「编辑项目」一次保存的全部可编辑字段。主文件夹(cwd)不在其中 —— 它不可改。
struct WorkspaceProfileUpdate {
    std::string name;
    WorkspaceIcon icon;
    std::vector<std::string> extra_folders;
};

enum class WorkspaceProfileStatus {
    Saved,
    NotFound,
    Invalid,
    WriteFailed,
};

class WorkspaceRegistry {
public:
    WorkspaceRegistry() = default;

    // 扫 projects_dir 下所有子目录,把可读 + 含 cwd 字段 + desktop_visible=true
    // 的 workspace.json 入册。
    //
    // 每次都完整枚举目录,可见性语义与逐个重读时完全一致;省掉的只是对没动过
    // 的目录重复探 workspace.json。projects_dir 下每个用过的 cwd 都留了一个
    // hash 目录(实测 16568 个),而带 marker 的只有几十个 —— 实测枚举全部目录
    // 连 mtime 只要 12.8ms,逐个探 workspace.json 却要 267ms。侧边栏 5 秒轮询
    // 里 /api/workspaces 与 /api/pinned-sessions/order 各调一次 scan,那 267ms
    // 会一直占着连接,把用户点击展开的请求挤到队尾。
    //
    // 缓存以每个 hash 目录自身的 mtime 为键(目录枚举顺带返回,不产生额外
    // syscall),并且连「这个目录没有 marker」也一并缓存。新目录不在缓存里必然
    // 被读到;marker 的新增/重命名/隐藏都走 atomic_file 的 tmp+rename,rename
    // 会推进所在目录的 mtime,同样必然被读到。文件系统时间戳有粒度(Windows
    // 上约 15ms),所以最近两秒内动过的目录一律不信任缓存 —— 否则与上次扫描
    // 落在同一 tick 的那次写入会被永久漏掉。
    void scan(const std::string& projects_dir);

    // 丢弃缓存后重扫。诊断用;正常路径不需要,scan() 本身已是精确的。
    void force_full_scan(const std::string& projects_dir);

    // 当前快照。线程安全(内部加锁后 copy)。
    std::vector<WorkspaceMeta> list() const;

    // Persist a complete permutation of the currently visible hashes. Hidden
    // hashes retain their slots in projects_dir/workspace_order.json. Validation,
    // atomic write, and cache update are serialized; failures do not save order.
    WorkspaceOrderStatus set_order(const std::string& projects_dir,
                                  const std::vector<std::string>& hashes);

    // 查指定 hash;不存在返回 nullopt。
    std::optional<WorkspaceMeta> get(const std::string& hash) const;

    // 给一个 cwd 注册 workspace:
    //   - 已存在(hash 已知)→ 返回已有 meta,不动磁盘
    //   - 新 → 建目录 + 写 workspace.json(name = default,desktop_visible=true),
    //     入册,返回新 meta
    // projects_dir 不会被扫(scan 是一次性);此处用调用方传入的根目录定位写入位置。
    WorkspaceMeta register_new(const std::string& projects_dir, const std::string& cwd);

    // 行内重命名:校验 name 非空 + hash 已知 → 原子写 workspace.json → 更新内存。
    // name 空、hash 未知、写盘失败均返回 false 并保持内存 cache 不变。
    bool set_name(const std::string& projects_dir, const std::string& hash, const std::string& name);

    // 「编辑项目」保存:名称 + 图标 + 附加文件夹整体替换,原子写 workspace.json。
    // 先从磁盘重读再改 —— Desktop 与 daemon 各持一份注册表缓存,拿内存那份写回
    // 会把另一个进程刚写的字段冲掉。附加文件夹经 normalize_workspace_extra_folders
    // 校验;失败时 error 带原因、磁盘与缓存都不动。成功时 out(若非空)为新 meta。
    WorkspaceProfileStatus update_profile(const std::string& projects_dir,
                                          const std::string& hash,
                                          const WorkspaceProfileUpdate& update,
                                          WorkspaceMeta* out,
                                          std::string& error);

    // 从 Desktop 项目列表隐藏 workspace:把 workspace.json 的 desktop_visible 写为 false,
    // 并从内存可见 cache 移除。不会删除 hash 目录、session 文件或用户项目文件。
    bool hide(const std::string& projects_dir, const std::string& hash);

    // 测试 hook:让单测注入空 registry(scan 一个临时目录),不必涉及共享状态。

private:
    // 一个 hash 目录上次被读取时的结论。has_marker=false 是负缓存:该目录没有
    // 可见 marker,下次目录 mtime 没变就不必再探一次文件。
    struct DirProbe {
        std::int64_t mtime = 0;
        bool has_marker = false;
        WorkspaceMeta meta;
    };

    void scan_locked(const std::string& projects_dir);

    mutable std::mutex mu_;
    std::unordered_map<std::string, WorkspaceMeta> entries_;
    std::unordered_map<std::string, DirProbe> dir_probes_;
    std::vector<std::string> workspace_order_;
};

// Read one on-disk workspace marker without applying the Desktop visibility
// filter and without mutating the registry or marker. Search/navigation code
// uses this to treat workspace identity as session metadata while keeping the
// sidebar's visible-workspace list unchanged.
std::optional<WorkspaceMeta> load_workspace_metadata(
    const std::string& projects_dir,
    const std::string& hash);

// 会话侧读取:给定会话的 project dir(= <projects_dir>/<hash>),返回主文件夹与
// 仍然存在的附加文件夹。不看 desktop_visible —— 从列表移除的项目只是不显示,
// 在该目录里起的会话照样沿用它的附加文件夹。文件缺失 / 损坏 → 两者皆空。
struct WorkspaceFolders {
    std::string main_folder;
    std::vector<std::string> extra_folders;
};
WorkspaceFolders load_workspace_folders(const std::string& project_dir);

// 校验并规范化附加文件夹:每项必须是存在的绝对目录;去掉尾部分隔符;与主文件夹
// 相同或彼此重复(Windows 上大小写 / 斜杠形态不敏感)的静默去掉;上限 32 项。
// 返回 false 时 error 为第一条不合格原因。
bool normalize_workspace_extra_folders(const std::string& main_folder,
                                       const std::vector<std::string>& input,
                                       std::vector<std::string>& output,
                                       std::string& error);

// 图标字段的字符集校验:id 为 [a-z0-9-]{1,40},color 为 [a-z0-9-]{0,24}。
// 空 id 合法(= 未设置)。
bool is_valid_workspace_icon(const WorkspaceIcon& icon);

// 确保某 cwd 的 workspace.json 存在,但不把它暴露给 Desktop startup。若文件
// 已存在,不读取也不覆盖,避免 TUI/daemon 启动把用户在 Desktop 里改过的 name
// 或 desktop_visible marker 冲掉。
bool ensure_workspace_metadata(const std::string& projects_dir, const std::string& cwd);

// 默认命名策略 — 暴露为公共符号便于直接单测。
//   1. fs::path(cwd).filename() 非空 → 用它
//   2. 否则 fs::path(cwd).root_name()(处理 "C:\\" / "/")
//   3. 否则字面常量 "workspace"
std::string default_workspace_name(const std::string& cwd);

// 校验 workspace hash 是否与 cwd 的规范化 hash 一致。hash 为空或 cwd 为空
// 视为不一致;用于共享 daemon 的 workspace-scoped API 入参防错。
bool workspace_hash_matches_cwd(const std::string& hash, const std::string& cwd);

} // namespace acecode::desktop
