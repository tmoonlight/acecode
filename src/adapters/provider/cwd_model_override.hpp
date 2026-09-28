// cwd_model_override: 每工作目录 model override 文件的读写。
// 对应 openspec/changes/model-profiles 的 Section 3。
// 文件位置:`~/.acecode/projects/<cwd_hash>/model_override.json`
// schema:`{"model_name": "<name>"}`
#pragma once

#include <optional>
#include <string>

namespace acecode {

// 所有 cwd 参数都是 UTF-8 编码的 std::string,与 SessionStorage::get_project_dir
// 同一口径。这里刻意不收 std::filesystem::path:调用方手里全是 UTF-8 字符串,
// 一旦签名是 path,MSVC 会按系统 ANSI 代码页做隐式 string→path 转换,中文路径
// 要么解成乱码算错 hash(override 静默失效),要么 MultiByteToWideChar 直接抛
// std::system_error —— daemon 的新建/恢复会话路由因此返回裸 500。

// 读 override。文件不存在 → nullopt;malformed / 缺字段 / 类型错误 →
// nullopt + log warning(MUST NOT 抛异常,不阻塞启动)。
// Windows 仅在 UTF-8 键缺失时兼容读取旧代码页键,读取时不改写文件。
std::optional<std::string> load_cwd_model_override(const std::string& cwd_utf8);

// 原子写(tmp + rename,Windows 下 rename 失败回退 remove+rename)。
// 复用 `session_storage.cpp::compute_project_hash(cwd)` 得到 <cwd_hash>。
void save_cwd_model_override(const std::string& cwd_utf8, const std::string& name);

// 删除 override 文件及 Windows 旧代码页副本;文件不存在为 no-op。
void remove_cwd_model_override(const std::string& cwd_utf8);

// 便捷函数:返回 override 文件的绝对路径(UTF-8)。测试可用。不保证父目录存在。
std::string cwd_model_override_path(const std::string& cwd_utf8);

} // namespace acecode
