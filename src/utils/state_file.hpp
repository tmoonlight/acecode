#pragma once

// 跨会话进程状态(once-only 提示标记等),持久化到 ~/.acecode/state.json。
// 跟 config.json 区分:config.json 是用户编辑面,state.json 是 ACECode 自己
// 写的运行时状态(同一个数据目录下,通过 paths::resolve_data_dir(RunMode)
// 得到)。
//
// 设计取舍见 openspec/changes/add-legacy-terminal-fallback/design.md
// Decision 4。各模块的专用 helper 管理自己的字段与校验规则;本层只提供
// 通用读写、跨进程互斥和原子更新,业务调用方不直接操作 state.json。

#include <functional>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace acecode {

// 读 state.json 中 key 对应的 bool。文件不存在 / 损坏 / key 缺失 / 类型不符
// 都返回 false。永不抛异常。
bool read_state_flag(const std::string& key);

// Pause background state writes during data-directory migration; reads continue.
void set_state_file_writes_paused(bool paused);

// Checked variant for callers that must report persistence failure (for
// example, an HTTP endpoint). The read-modify-write sequence is serialized
// with all other state_file operations in this process.
bool try_write_state_flag(const std::string& key, bool value);

struct StateFlagClaimResult {
    bool claimed = false;
    bool persisted = false;
};

// Atomically across threads and processes changes a missing/false flag to true.
// Exactly one concurrent caller observes claimed=true. State writers share the
// same interprocess lock so a later read-modify-write cannot erase the claim.
// A failed durable write never grants the claim.
StateFlagClaimResult try_claim_state_flag(const std::string& key);

// 测试专用:覆盖 state.json 的解析路径。传空串清除覆盖,回到从
// resolve_data_dir(get_run_mode()) 计算的默认。生产代码不应调用。
void set_state_file_path_for_test(const std::string& path);

// 写 state.json:把 key 设为 value,保留其它已有 key。
// 文件损坏(非合法 JSON)时整体覆盖并写一条 LOG_WARN。
// 写失败(权限 / 只读盘等)只 LOG_WARN,不阻断调用方。
//
// 用法:write_state_flag("legacy_terminal_hint_shown", true);
void write_state_flag(const std::string& key, bool value);

// 各模块共用的状态存储入口:
// read_state_json:进程内锁下读取整份 state.json,缺失 / 损坏视为空对象。
nlohmann::json read_state_json();
// update_state_json:进程内锁 + 跨进程文件锁下「读-改-写」;mutate 在本次调用内同步执行,
// 不保存回调,回调中不得重入 state_file API。mutate 返回 false 表示放弃写入
// (仍视为成功)。损坏的 state.json 在此整体重写并记 LOG_WARN;拿不到锁或写失败返回 false。
bool update_state_json(const std::function<bool(nlohmann::json& state)>& mutate);

} // namespace acecode
