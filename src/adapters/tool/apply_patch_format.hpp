#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace acecode::apply_patch {

// Codex / opencode 补丁语言(openspec add-gpt-apply-patch-adaptation)的纯逻辑层:
// 解析信封与三种文件操作、按上下文推导 Update 后的新内容。不碰文件系统,
// 工具层(apply_patch_tool.cpp)负责读写。语义对齐 codex-rs `apply-patch` 与
// opencode `src/patch/index.ts`,偏离处见 openspec design.md D2。

enum class HunkKind {
    Add,
    Delete,
    Update,
};

struct UpdateChunk {
    std::vector<std::string> old_lines;
    std::vector<std::string> new_lines;
    // `@@` 之后的锚点文本(已 trim);空 = 无锚点。
    std::string change_context;
    // 遇到 `*** End of File`:匹配时优先从文件末尾对齐。
    bool is_end_of_file = false;
};

struct PatchHunk {
    HunkKind kind = HunkKind::Update;
    // 补丁里写的原始路径(相对或绝对,未解析)。
    std::string path;
    // 仅 Update:`*** Move to:` 目标;空 = 原地更新。
    std::string move_path;
    // 仅 Add:LF 文本,非空时以换行结尾。
    std::string contents;
    // 仅 Update。
    std::vector<UpdateChunk> chunks;
};

struct ParsedPatch {
    std::vector<PatchHunk> hunks;
};

struct ParseResult {
    bool success = false;
    std::string error;
    ParsedPatch patch;
};

// 解析补丁全文。接受 CRLF、外层 heredoc(`apply_patch <<'EOF' ... EOF`)与
// markdown 代码围栏包裹。缺信封 / 缺 header / 未知行前缀 → 失败并带行号。
// 注意:信封内零操作也算 success(hunks 为空),由调用方决定报 empty patch。
ParseResult parse_patch(const std::string& patch_text);

struct DeriveResult {
    bool success = false;
    std::string error;
    // LF 文本。结尾换行:原文有则有、原文为空则加、原文无则不加。
    std::string content;
};

// 把 Update 的 chunks 应用到原文(LF 文本)。锚点先 seek,old_lines 自锚点后
// 匹配,四级容错(精确 → 去尾空白 → 去两端空白 → Unicode 标点归一),失败时
// 去掉尾部空行重试一次;替换按起始行排序后倒序应用。
DeriveResult derive_new_contents(const std::string& file_path,
                                 const std::vector<UpdateChunk>& chunks,
                                 const std::string& original_lf_text);

// 从工具参数 JSON 取补丁文本:`input` 优先,其次 `patchText` / `patch`。
// 解析失败或都缺 → 空串。
std::string patch_text_from_arguments(const std::string& arguments_json);

struct PatchFileHeader {
    HunkKind kind = HunkKind::Update;
    std::string path;
    std::string move_path;
};

// 只扫 header 行(不校验信封与正文),给调用行预览 / 权限确认框列文件用。
std::vector<PatchFileHeader> summarize_patch_headers(const std::string& patch_text);

// 相对路径按 cwd 解析,绝对路径照收;结果 lexically_normal 的 UTF-8 原生形态。
// cwd 为空时相对路径原样返回。
std::string resolve_patch_path(const std::string& raw_path, const std::string& cwd);

// AgentLoop 权限 / 边界校验用:补丁涉及的全部路径(Add / Update / Delete 的
// 路径 + Move 目标),已按 cwd 解析,按出现顺序去重。解析失败返回空(工具随后
// 会以同一解析器失败,不会落盘)。
std::vector<std::string> extract_target_paths(const std::string& arguments_json,
                                              const std::string& cwd);

// 单行比较用的 Unicode 标点归一(弯引号 → ASCII 引号、各种破折号 → '-'、
// 省略号 → "..."、不换行空格 → ' ')。公开出来便于单测。
std::string normalize_unicode_punctuation(const std::string& text);

} // namespace acecode::apply_patch
