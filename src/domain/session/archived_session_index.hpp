#pragma once

#include "session_storage.hpp"

#include <functional>
#include <string>
#include <vector>

namespace acecode {

// 已归档会话索引:<数据目录>/archived-sessions.json,与 projects/ 同级。
//
// 归档列表原本要把项目目录里每个 .meta.json 都打开一遍再挑 archived=true 的。
// 上千会话的目录在 Windows 上冷读(每次打开都过一遍杀软)要二三十秒,而里面
// 归档的往往一条都没有。索引只登记归档过的会话,列表只读登记的那几条。
//
// - 登记:SessionStorage::write_meta 每次成功写下 archived=true 之后调用
//   note_archived_session_meta。取消归档、永久删除不碰索引;列表读到 meta 已不是
//   归档状态或文件已不存在时再剔除,所以索引是归档会话的超集。
// - 补建:没标记「已扫描」的目录第一次列出时整目录扫一遍,扫到的归档会话并进
//   索引后再标记,这一次仍然要整读,之后不再整读。扫描不持锁,提交时在文件锁里
//   与索引现有内容取并集;扫描期间别处归档的会话由它自己的登记补上,不会丢。
// - 只认标准布局 <根>/projects/<目录名>/<id>.meta.json。别的目录不建索引,
//   列表退回整目录扫描。
// - 索引文件损坏、被删或版本不认识,等同于所有目录都没扫描过,自动重建。
// - 目录标记为已扫描之后,再用不认识索引的旧版本归档的会话不会出现在列表里;
//   删掉索引文件即可重建。

// project_dir 对应的索引文件;不是标准布局时返回空串。
std::string archived_session_index_path(const std::string& project_dir);

// write_meta 成功落盘之后调用;meta.archived 为 false 时什么也不做。失败只记日志。
void note_archived_session_meta(const std::string& meta_path, const SessionMeta& meta);

// project_dir 里全部已归档会话的 meta(含子会话,由调用方过滤),顺序不定。
std::vector<SessionMeta> list_archived_session_metadata(const std::string& project_dir);

// projects_root 下全部无工作区的已归档会话,顺序不定。no_workspace_project_dirs
// 只在还没补建时调用,返回每个无工作区会话所在的项目目录。
std::vector<SessionMeta> list_archived_no_workspace_session_metadata(
    const std::string& projects_root,
    const std::function<std::vector<std::string>()>& no_workspace_project_dirs);

} // namespace acecode
