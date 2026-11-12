#pragma once

// bash 命令结束后,尽力找出它生成的文件,供摘要面板的「产物」分组使用
// (session/session_outputs.hpp)。脚本写文件不经过文件工具,会话记录里没有
// 路径,只能事后找。找不到就算了:这是尽力而为,不保证不漏。
//
// 两条线索,都只认文档 / 表格 / 图片这类扩展名,并且要求文件的修改时间
// 不早于命令开始:
//   1. 命令本身和输出末尾提到的路径(`-o report.pdf`、`Saved to D:\x\a.xlsx`),
//      工作目录外的文件只能靠这条;
//   2. 扫一遍工作目录:点开头的目录、node_modules 之类整个跳过;build / dist /
//      out 这类构建目录只看第一层;有条数和时间上限,先扫浅层。
//
// 两种情况不找:只读命令(分类器判 KnownSafe);整条命令只有构建 / 包管理 /
// 版本控制工具(git checkout、pnpm build 会一次改动一批文档类文件,不是产物)。
// 一条命令改动的文档类文件超过 bulk_threshold 个,也当作批量操作,扫描结果作废
// (明确提到的路径仍保留)。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace acecode {

struct ShellOutputProbe {
    bool enabled = false;
    std::string cwd;
    std::int64_t started_ms = 0;   // 命令开始时刻,Unix 毫秒
};

struct ShellOutputScanLimits {
    std::size_t max_entries = 6000;     // 最多看这么多个目录项
    int max_millis = 120;               // 扫描最多花这么久
    std::size_t max_depth = 8;
    std::size_t bulk_threshold = 12;
    std::size_t max_mentions = 8;       // 「提到的路径」最多采纳几个
};

// 纯判定:这条命令值不值得找产物。
bool shell_command_may_produce_files(const std::string& command);

// 纯函数:命令行里像产物路径的单词(去引号后;`--out=a.pdf` 取等号后面)。
std::vector<std::string> shell_command_path_mentions(const std::string& command);

// 纯函数:一段输出文本里像产物路径的片段。每处扩展名给出紧凑的那一段,
// 同一行左边有绝对路径起点时再给出「从起点到扩展名」的整段(路径里带空格)。
std::vector<std::string> shell_text_path_mentions(std::string_view text);

// 命令开始前调用,记下开始时刻。
ShellOutputProbe begin_shell_output_probe(const std::string& command, const std::string& cwd);
// 后台命令:开始时刻由登记表给出。
ShellOutputProbe make_shell_output_probe(const std::string& command, const std::string& cwd,
                                         std::int64_t started_ms);

// 命令结束后调用。返回绝对路径(UTF-8),提到的在前、扫到的在后(按路径排序)。
std::vector<std::string> collect_shell_output_files(const ShellOutputProbe& probe,
                                                    const std::string& command,
                                                    std::string_view output,
                                                    const ShellOutputScanLimits& limits = {});

} // namespace acecode
