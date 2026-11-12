// bash 命令产物探测(src/adapters/tool/shell_output_files.cpp)的单测。
// 背景:脚本写文件不经过文件工具,会话记录里没有路径;bash 结束后按
// 「命令 / 输出里提到的路径」+「扫一遍工作目录找刚改过的文档类文件」两条线索
// 尽力找出来,记进工具结果的 metadata.output_files,摘要面板的「产物」读它。
// 这里分三层测:哪些命令值得找(纯判定)、路径片段怎么提(纯函数)、
// 真实目录上的收集结果(含 bash 工具端到端)。

#include "tool/bash_tool.hpp"
#include "tool/shell_output_files.hpp"
#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

using acecode::collect_shell_output_files;
using acecode::make_shell_output_probe;
using acecode::shell_command_may_produce_files;
using acecode::shell_command_path_mentions;
using acecode::shell_text_path_mentions;

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

struct WorkDir {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("ace-shell-outputs-" + acecode::generate_uuid());
    WorkDir() { std::filesystem::create_directories(root); }
    ~WorkDir() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::string utf8() const { return acecode::path_to_utf8(root); }
    // age_seconds > 0:把修改时间拨到命令开始之前,模拟「早就在那儿的文件」。
    std::string touch(const std::string& relative, int age_seconds = 0) const {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << "content";
        if (age_seconds > 0) {
            std::filesystem::last_write_time(
                path, std::filesystem::last_write_time(path) - std::chrono::seconds(age_seconds));
        }
        return acecode::path_to_utf8(path.lexically_normal());
    }
};

// 结果里的路径相对工作目录的写法(正斜杠),方便断言。
std::vector<std::string> relative_to(const WorkDir& dir, const std::vector<std::string>& files) {
    std::vector<std::string> out;
    for (const auto& file : files) {
        out.push_back(acecode::path_to_utf8_generic(
            acecode::path_from_utf8(file).lexically_relative(dir.root)));
    }
    return out;
}

} // namespace

// 场景:决定一条命令结束后要不要去找产物。
// 期望:跑脚本、带输出重定向的要找;只读命令、纯构建 / 包管理 / 版本控制命令不找
// (它们一次改动一批文档类文件,不是产物);cd 之类的铺垫不影响后面的脚本;
// `uv run` 这类是在跑用户的程序,照常找。
TEST(ShellOutputFiles, DecidesWhichCommandsAreWorthScanning) {
    EXPECT_TRUE(shell_command_may_produce_files("python gen_report.py"));
    EXPECT_TRUE(shell_command_may_produce_files("cd reports && python gen.py"));
    EXPECT_TRUE(shell_command_may_produce_files("OUT=1 python gen.py"));
    EXPECT_TRUE(shell_command_may_produce_files("uv run report.py"));
    // 回归:echo 被当成「不会生成文件的铺垫」,连 `echo x > notes.md` 也跳过了。
    EXPECT_TRUE(shell_command_may_produce_files("echo hello > notes.md"));

    EXPECT_FALSE(shell_command_may_produce_files(""));
    EXPECT_FALSE(shell_command_may_produce_files("ls -la"));
    EXPECT_FALSE(shell_command_may_produce_files("git pull"));
    EXPECT_FALSE(shell_command_may_produce_files("cd web && pnpm build"));
    EXPECT_FALSE(shell_command_may_produce_files("uv sync"));
}

// 场景:命令行里写明了输出位置(位置参数、--out=、带空格的引号路径、重定向目标)。
// 期望:文档类扩展名的单词都取出来(去引号);脚本本身(.py)和网址不取。
TEST(ShellOutputFiles, CommandMentionsPickDeliverablePathsOnly) {
    const auto mentions = shell_command_path_mentions(
        "python gen.py --out=reports/q3.xlsx \"C:\\My Docs\\a b.pdf\" > log.txt");
    EXPECT_TRUE(contains(mentions, "reports/q3.xlsx"));
    EXPECT_TRUE(contains(mentions, "C:\\My Docs\\a b.pdf"));
    EXPECT_TRUE(contains(mentions, "log.txt"));
    EXPECT_FALSE(contains(mentions, "gen.py"));

    const auto download = shell_command_path_mentions("curl https://example.com/a.pdf -o a.pdf");
    EXPECT_EQ(download, (std::vector<std::string>{"a.pdf"}));
}

// 场景:脚本在输出里打印了保存位置(英文、中文带全角标点、Python repr 的引号)。
// 期望:取出紧凑片段;路径里带空格时,同一行左边有盘符起点就再给出整段;
// 网址、a.md.bak 这种后面还接着扩展名的不取。
TEST(ShellOutputFiles, TextMentionsHandleSpacesPunctuationAndUrls) {
    EXPECT_EQ(shell_text_path_mentions("Saved to out/report.xlsx"),
              (std::vector<std::string>{"out/report.xlsx"}));
    EXPECT_EQ(shell_text_path_mentions("result: out.csv, rows=3\n"),
              (std::vector<std::string>{"out.csv"}));
    EXPECT_EQ(shell_text_path_mentions("saved:out.csv"), (std::vector<std::string>{"out.csv"}));
    EXPECT_EQ(shell_text_path_mentions("wrote 'C:\\data\\a.csv' ok"),
              (std::vector<std::string>{"C:\\data\\a.csv"}));

    // 全角冒号(EF BC 9A)和句号紧贴路径,路径里还有空格。字节写法免得被换成半角。
    const std::string chinese = std::string("\xE5\xB7\xB2\xE4\xBF\x9D\xE5\xAD\x98\xE5\x88\xB0") +
        "\xEF\xBC\x9A" + "D:\\data\\q3 report.xlsx" + "\xE3\x80\x82";
    const auto mentions = shell_text_path_mentions(chinese);
    EXPECT_TRUE(contains(mentions, "report.xlsx"));
    EXPECT_TRUE(contains(mentions, "D:\\data\\q3 report.xlsx"));

    EXPECT_TRUE(shell_text_path_mentions("see https://example.com/files/a.pdf").empty());
    EXPECT_TRUE(shell_text_path_mentions("backup at notes.md.bak").empty());
    EXPECT_TRUE(shell_text_path_mentions("version 1.2 of tool.exe").empty());
}

// 场景:python 脚本在工作目录里生成了几个文件,没有打印任何路径。
// 期望:命令开始后改过的文档类文件被找到;早就存在的文档、代码文件、依赖 /
// 点目录里的文件不算;build / out 这类构建目录只看第一层。
TEST(ShellOutputFiles, ScanFindsFreshDeliverablesInWorkspace) {
    WorkDir dir;
    dir.touch("existing.md", 3600);
    const auto probe = make_shell_output_probe("python gen.py", dir.utf8(), now_ms());
    ASSERT_TRUE(probe.enabled);
    dir.touch("charts/sales.png");
    dir.touch("out/report.xlsx");
    dir.touch("out/cache/deep.pdf");            // 构建目录的第二层:不看
    dir.touch("src/main.py");                   // 代码
    dir.touch("node_modules/pkg/README.md");    // 依赖目录
    dir.touch(".cache/notes.md");               // 点目录

    const auto files = relative_to(dir, collect_shell_output_files(probe, "python gen.py", "done"));
    EXPECT_EQ(files, (std::vector<std::string>{"charts/sales.png", "out/report.xlsx"}));
}

// 场景:一条命令改动了一大批文档类文件(构建、批量转换)。
// 期望:超过 bulk_threshold 就把扫描结果整个作废,不拿一堆构建产物刷屏;
// 但命令里明确写出来的那个输出文件仍然保留。
TEST(ShellOutputFiles, BulkChangesAreDroppedButExplicitMentionSurvives) {
    WorkDir dir;
    const auto probe = make_shell_output_probe("python build_site.py -o summary.md", dir.utf8(), now_ms());
    for (int i = 0; i < 20; ++i) dir.touch("pages/page" + std::to_string(i) + ".html");
    dir.touch("summary.md");

    const auto files = relative_to(dir,
        collect_shell_output_files(probe, "python build_site.py -o summary.md", ""));
    EXPECT_EQ(files, (std::vector<std::string>{"summary.md"}));
}

// 场景:脚本把结果写到工作目录外面,并在输出里打印了绝对路径;同时命令行里
// 提到了一个只是被读取的旧数据文件。
// 期望:工作目录外的新文件靠「输出里提到」被找到;旧文件(修改时间早于命令
// 开始)不算 —— 提到不等于生成。
TEST(ShellOutputFiles, MentionsReachOutsideWorkspaceAndIgnoreStaleFiles) {
    WorkDir dir;
    WorkDir elsewhere;
    dir.touch("input.csv", 3600);
    const auto probe = make_shell_output_probe("python analyze.py input.csv", dir.utf8(), now_ms());
    const std::string exported = elsewhere.touch("export/result.pdf");

    const auto files = collect_shell_output_files(
        probe, "python analyze.py input.csv", "Saved to " + exported + "\n");
    EXPECT_EQ(files, (std::vector<std::string>{exported}));
}

// 场景:git / 只读命令运行期间,工作目录里正好有文档被改了(别的会话、用户自己)。
// 期望:这类命令的探测是关着的,不把无关改动算到它头上。
TEST(ShellOutputFiles, DisabledProbeReportsNothing) {
    WorkDir dir;
    const auto probe = make_shell_output_probe("git status", dir.utf8(), now_ms());
    EXPECT_FALSE(probe.enabled);
    dir.touch("notes.md");
    EXPECT_TRUE(collect_shell_output_files(probe, "git status", "").empty());
}

// 场景:端到端。bash 工具执行一条写文件的命令。
// 期望:工具结果的 metadata.output_files 里有这个文件的绝对路径,模型看到的
// 正文不变(不往输出里加东西)。
TEST(ShellOutputFiles, BashToolRecordsOutputFilesInMetadata) {
    WorkDir dir;
    acecode::ToolImpl tool = acecode::create_bash_tool();
    acecode::ToolContext ctx;
    ctx.cwd = dir.utf8();
    const nlohmann::json args = {{"command", "echo hello > report.txt"}};
    const acecode::ToolResult result = tool.execute(args.dump(), ctx);

    ASSERT_TRUE(result.success) << result.output;
    ASSERT_TRUE(result.metadata.contains("output_files")) << result.metadata.dump();
    const auto files = relative_to(dir, result.metadata["output_files"].get<std::vector<std::string>>());
    EXPECT_EQ(files, (std::vector<std::string>{"report.txt"}));
    EXPECT_EQ(result.output.find("report.txt"), std::string::npos);
}
