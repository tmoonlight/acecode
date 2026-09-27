#pragma once

#include <string>

namespace acecode {

// desktop multi-workspace: 上次活跃 workspace 的 cwd_hash。
// 读: 文件不存在 / 字段缺失 / 类型不符 → 空字符串。永不抛异常。
// 写: 原子写,保留其他 key;失败只 LOG_WARN 不阻断。
std::string read_last_active_workspace_hash();
void write_last_active_workspace_hash(const std::string& hash);

// desktop home page: 首页新建会话选择器上次选中的 workspace cwd_hash。
// 空字符串是有效值,表示"不使用工作区"。
std::string read_last_home_workspace_hash();
void write_last_home_workspace_hash(const std::string& hash);

} // namespace acecode
