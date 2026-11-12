#pragma once

// 会话产物的读盘一侧:增量扫描会话记录取候选,跟到子代理会话,再到磁盘上
// 核对文件还在不在。纯逻辑见 session_outputs.hpp。
//
// 成本:每个记录文件只在第一次查询时整读一遍,之后只读新追加的部分(按文件
// 身份 + 已读偏移 + 文件头指纹判断能否接着读;压缩 / 回退把文件换掉后从头再读)。
// 行先过 jsonl_line_may_carry_outputs 预筛,绝大多数行不解析 JSON。

#include "session_outputs.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode {

struct SessionOutputFile {
    std::string path;       // 归一后的绝对路径(UTF-8)
    std::string relative;   // 在会话文件夹内时的相对路径(正斜杠),否则为空
    SessionOutputSource source = SessionOutputSource::Write;
    std::uint64_t size = 0;
    std::int64_t modified_ms = 0;   // Unix 毫秒(秒级精度)
};

struct SessionOutputsQuery {
    std::string session_id;
    // 会话记录所在目录(SessionStorage::get_project_dir);子代理会话与主会话同目录。
    std::string project_dir;
    // 解析记录里相对路径用的工作目录。
    std::string cwd;
    // 会话文件夹:工作目录、worktree、附加文件夹。不必归一,这里统一处理。
    std::vector<std::string> folders;
    std::size_t limit = 50;
};

struct SessionOutputsResult {
    std::vector<SessionOutputFile> files;   // 按修改时间新 → 旧
    bool has_more = false;
};

class SessionOutputsIndex {
public:
    SessionOutputsResult query(const SessionOutputsQuery& query);

private:
    struct Scan {
        std::string identity;
        std::string prefix;
        std::uint64_t offset = 0;
        std::uint64_t used = 0;
        std::vector<SessionOutputCandidate> candidates;   // 旧 → 新
        std::vector<std::string> children;
    };

    Scan scan_session(const std::string& jsonl_path);
    static void advance(Scan& scan, const std::string& jsonl_path);

    std::mutex mu_;
    std::uint64_t tick_ = 0;
    std::unordered_map<std::string, Scan> scans_;
};

} // namespace acecode
