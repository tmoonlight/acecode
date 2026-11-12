// 会话「产物」(摘要面板的产物分组)的单测:
//   - session_outputs.hpp 的纯逻辑:从工具结果元数据取候选、扩展名判定、
//     JSONL 行预筛、过滤去重;
//   - session_outputs_index.hpp 的读盘一侧:增量扫描会话记录、跟到子代理会话、
//     核对文件是否还在、按修改时间排序。
// 背景:产物列表完全由会话记录推导,不改写文件的工具;这里的用例就是
// 「记录里长什么样 → 列表里出什么」的契约。

#include "session/session_outputs.hpp"
#include "session/session_outputs_index.hpp"
#include "session/session_serializer.hpp"
#include "session/session_storage.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

using acecode::ChatMessage;
using acecode::SessionOutputCandidate;
using acecode::SessionOutputRoots;
using acecode::SessionOutputSource;
using nlohmann::json;

// file_write 的工具结果:tool_result_committer 把 ToolSummary 编码进 metadata.tool_summary。
ChatMessage write_result(const std::string& path, const char* verb = "Created", bool success = true) {
    ChatMessage message;
    message.role = "tool";
    message.tool_call_id = "call-write";
    message.content = "Created file: " + path;
    message.metadata = {
        {"tool_success", success},
        {"tool_summary", {{"verb", verb}, {"object", path}, {"icon", ""}, {"metrics", json::array()}}},
    };
    return message;
}

// bash 的工具结果:shell_output_files 找到的文件在 metadata.output_files。
ChatMessage bash_result(const std::vector<std::string>& files, bool success = true) {
    ChatMessage message;
    message.role = "tool";
    message.tool_call_id = "call-bash";
    message.content = "(no output)";
    message.metadata = {{"tool_success", success}, {"exit_code", success ? 0 : 1}, {"output_files", files}};
    return message;
}

struct OutputsFixture {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("ace-outputs-" + acecode::generate_uuid());
    std::filesystem::path project = root / "project";   // 会话记录目录
    std::filesystem::path work = root / "work";         // 会话工作目录
    OutputsFixture() {
        std::filesystem::create_directories(project);
        std::filesystem::create_directories(work);
    }
    ~OutputsFixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::string jsonl(const std::string& session_id) const {
        return acecode::SessionStorage::session_path(acecode::path_to_utf8(project), session_id);
    }
    std::string touch(const std::string& relative, int age_seconds = 0) const {
        const auto path = work / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << "content";
        if (age_seconds > 0) {
            std::filesystem::last_write_time(
                path, std::filesystem::last_write_time(path) - std::chrono::seconds(age_seconds));
        }
        return acecode::path_to_utf8(path);
    }
    acecode::SessionOutputsQuery query(const std::string& session_id) const {
        acecode::SessionOutputsQuery q;
        q.session_id = session_id;
        q.project_dir = acecode::path_to_utf8(project);
        q.cwd = acecode::path_to_utf8(work);
        q.folders = {q.cwd};
        return q;
    }
};

std::vector<std::string> relatives(const acecode::SessionOutputsResult& result) {
    std::vector<std::string> out;
    for (const auto& file : result.files) out.push_back(file.relative);
    return out;
}

} // namespace

// 场景:判定哪些文件名算「拿来看 / 交付」的产物。
// 期望:文档、表格、图片等扩展名(不分大小写)算;代码、无扩展名、点文件不算;
// 正反斜杠路径都只看最后一段。
TEST(SessionOutputs, DeliverableExtensionsAreDocumentsNotCode) {
    EXPECT_TRUE(acecode::is_deliverable_file_name("report.md"));
    EXPECT_TRUE(acecode::is_deliverable_file_name("C:\\out\\Q3.XLSX"));
    EXPECT_TRUE(acecode::is_deliverable_file_name("/tmp/chart.svg"));
    EXPECT_FALSE(acecode::is_deliverable_file_name("src/main.cpp"));
    EXPECT_FALSE(acecode::is_deliverable_file_name("Makefile"));
    EXPECT_FALSE(acecode::is_deliverable_file_name(".md"));
    // 目录名里有点、文件本身没有扩展名:不能把目录的「扩展名」当成文件的。
    EXPECT_FALSE(acecode::is_deliverable_file_name("docs.md/readme"));
}

// 场景:file_write 成功的结果(新建 = Created,覆写 = Wrote)。
// 期望:都产出一条 Write 候选;写入失败(tool_success=false)和编辑(Edited)不算。
TEST(SessionOutputs, FileWriteSummaryYieldsCandidateOnlyOnSuccess) {
    auto created = acecode::session_output_candidates(write_result("C:/w/a.md"));
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(created[0].path, "C:/w/a.md");
    EXPECT_EQ(created[0].source, SessionOutputSource::Write);

    EXPECT_EQ(acecode::session_output_candidates(write_result("C:/w/a.md", "Wrote")).size(), 1u);
    EXPECT_TRUE(acecode::session_output_candidates(write_result("C:/w/a.md", "Created", false)).empty());
    EXPECT_TRUE(acecode::session_output_candidates(write_result("C:/w/a.md", "Edited")).empty());
}

// 场景:apply_patch 一次改多个文件,metadata.files 里有 add / update / delete。
// 期望:只有新建(add)算产物;改已有文件相当于编辑,归「变更」。
TEST(SessionOutputs, ApplyPatchCountsOnlyAddedFiles) {
    ChatMessage message;
    message.role = "tool";
    message.metadata = {
        {"tool_success", true},
        {"tool_summary", {{"verb", "Patched"}, {"object", "3 files"}}},
        {"files", json::array({
            {{"path", "C:/w/new.md"}, {"type", "add"}},
            {{"path", "C:/w/old.md"}, {"type", "update"}},
            {{"path", "C:/w/gone.md"}, {"type", "delete"}},
        })},
    };
    const auto candidates = acecode::session_output_candidates(message);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].path, "C:/w/new.md");
    EXPECT_EQ(candidates[0].source, SessionOutputSource::Patch);
}

// 场景:bash 跑脚本生成了文件,但脚本最后以非零退出码结束。
// 期望:文件已经在磁盘上了,仍然算产物(不看 tool_success)。
TEST(SessionOutputs, BashOutputFilesCountEvenWhenCommandFailed) {
    const auto candidates = acecode::session_output_candidates(bash_result({"C:/w/chart.png"}, false));
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].source, SessionOutputSource::Shell);
}

// 场景:后台命令结束后的完成通知(user 角色 + metadata.background_task)。
// 期望:通知里的 output_files 算产物;普通 user 消息不产出候选。
TEST(SessionOutputs, BackgroundCompletionNoticeCarriesOutputFiles) {
    ChatMessage notice;
    notice.role = "user";
    notice.metadata = {{"background_task", {{"id", "bg1"}, {"output_files", {"C:/w/long.csv"}}}}};
    const auto candidates = acecode::session_output_candidates(notice);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].path, "C:/w/long.csv");

    ChatMessage plain;
    plain.role = "user";
    plain.content = "please write report.md";
    EXPECT_TRUE(acecode::session_output_candidates(plain).empty());
}

// 场景:扫描会话记录时先用子串预筛,只解析可能带产物的行。
// 期望:四种来源落盘后的行都能通过预筛;正文里出现同样字样的普通消息不通过
// (JSON 字符串里的引号带反斜杠),否则编辑 ACECode 自己的源码会让每一行都被解析。
TEST(SessionOutputs, PrefilterMatchesSerializedSourcesButNotQuotedText) {
    EXPECT_TRUE(acecode::jsonl_line_may_carry_outputs(acecode::serialize_message(write_result("C:/w/a.md"))));
    EXPECT_TRUE(acecode::jsonl_line_may_carry_outputs(acecode::serialize_message(bash_result({"C:/w/a.png"}))));

    ChatMessage spawn;
    spawn.role = "tool";
    spawn.metadata = {{"subagent_session_id", "child-1"}};
    EXPECT_TRUE(acecode::jsonl_line_may_carry_outputs(acecode::serialize_message(spawn)));
    EXPECT_EQ(acecode::session_output_child_session_id(spawn), "child-1");

    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = R"(the summary is {"verb":"Created"} and "output_files" in JSON)";
    EXPECT_FALSE(acecode::jsonl_line_may_carry_outputs(acecode::serialize_message(assistant)));
}

// 场景:工作目录内既写了文档也写了代码,还写了 .acecode 下的内部文件。
// 期望:只留文档;代码归「变更」;.acecode / .git 下的不算。
TEST(SessionOutputs, InsideWorkspaceKeepsOnlyDeliverablesOutsideHiddenState) {
    SessionOutputRoots roots;
    roots.folders = {"C:/repo"};
    const std::vector<SessionOutputCandidate> candidates = {
        {"C:/repo/docs/plan.md", SessionOutputSource::Write},
        {"C:/repo/src/main.cpp", SessionOutputSource::Write},
        {"C:/repo/.acecode/tmp/session-1/scratch.md", SessionOutputSource::Write},
        {"C:/repo/.git/description.txt", SessionOutputSource::Shell},
    };
    const auto entries = acecode::select_session_outputs(candidates, roots);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative, "docs/plan.md");
    EXPECT_EQ(entries[0].folder_index, 0);
}

// 场景:worktree 会话。工作目录是 <仓库>/.acecode/worktrees/<名字>,仓库根也在
// 会话文件夹里。
// 期望:worktree 里的文档算产物(相对路径按 worktree 算,不因路径里有 .acecode
// 被丢掉);仓库根的 .acecode/tmp 仍然不算。
// 回归:只按「路径里有 .acecode」过滤时,worktree 会话的产物列表永远是空的。
TEST(SessionOutputs, WorktreeFolderIsNotTreatedAsHiddenState) {
    SessionOutputRoots roots;
    roots.folders = {"C:/repo", "C:/repo/.acecode/worktrees/feat"};
    const std::vector<SessionOutputCandidate> candidates = {
        {"C:/repo/.acecode/worktrees/feat/docs/spec.md", SessionOutputSource::Write},
        {"C:/repo/.acecode/tmp/session-1/note.md", SessionOutputSource::Write},
    };
    const auto entries = acecode::select_session_outputs(candidates, roots);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].relative, "docs/spec.md");
    EXPECT_EQ(entries[0].folder_index, 1);
}

// 场景:写到工作目录外面(桌面、别的盘)和系统临时目录。
// 期望:工作目录外任何扩展名都算(用户明确要的导出位置);临时目录里的不算。
TEST(SessionOutputs, OutsideWorkspaceKeepsAnyExtensionExceptTemp) {
    SessionOutputRoots roots;
    roots.folders = {"C:/repo"};
    roots.temp_roots = {"C:/Users/u/AppData/Local/Temp"};
    const std::vector<SessionOutputCandidate> candidates = {
        {"D:/export/data.json", SessionOutputSource::Write},
        {"C:/Users/u/AppData/Local/Temp/x/report.md", SessionOutputSource::Write},
        // 名字只是以工作目录为前缀的兄弟目录,不在工作目录内。
        {"C:/repo-other/main.cpp", SessionOutputSource::Write},
    };
    const auto entries = acecode::select_session_outputs(candidates, roots);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].path, "C:/repo-other/main.cpp");
    EXPECT_TRUE(entries[0].relative.empty());
    EXPECT_EQ(entries[0].folder_index, -1);
    EXPECT_EQ(entries[1].path, "D:/export/data.json");
}

// 场景:同一个文件被写了多次,记录里的路径大小写、分隔符不一致(Windows)。
// 期望:只出现一次,取最近一次;结果按新 → 旧排列。
TEST(SessionOutputs, DedupesByPathNewestFirst) {
    SessionOutputRoots roots;
    roots.folders = {"C:/repo"};
    const std::vector<SessionOutputCandidate> candidates = {
        {"C:/repo/a.md", SessionOutputSource::Write},
        {"C:/repo/b.md", SessionOutputSource::Write},
        {"c:\\repo\\A.md", SessionOutputSource::Shell},
    };
    const auto entries = acecode::select_session_outputs(candidates, roots);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].relative, "A.md");
    EXPECT_EQ(entries[0].source, SessionOutputSource::Shell);
    EXPECT_EQ(entries[1].relative, "b.md");
}

// 场景:会话进行中反复查询产物(面板可见时每隔几秒一次)。
// 期望:第一次读到已有的写入;之后追加的工具结果下一次查询就能看到(只读
// 新追加的部分);文件从磁盘上删掉后不再返回。
TEST(SessionOutputsIndex, PicksUpAppendedResultsAndDropsDeletedFiles) {
    OutputsFixture fixture;
    const std::string first = fixture.touch("report.md", 60);
    acecode::SessionStorage::write_messages(fixture.jsonl("s1"), {write_result(first)});

    acecode::SessionOutputsIndex index;
    auto result = index.query(fixture.query("s1"));
    EXPECT_EQ(relatives(result), (std::vector<std::string>{"report.md"}));

    const std::string second = fixture.touch("charts/sales.png");
    ASSERT_TRUE(acecode::SessionStorage::append_message(fixture.jsonl("s1"), bash_result({second})));
    result = index.query(fixture.query("s1"));
    // sales.png 更新(report.md 的修改时间被拨到 60 秒前),排前面。
    EXPECT_EQ(relatives(result), (std::vector<std::string>{"charts/sales.png", "report.md"}));
    EXPECT_GT(result.files[0].size, 0u);
    EXPECT_GT(result.files[0].modified_ms, 0);

    std::filesystem::remove(acecode::path_from_utf8(first));
    result = index.query(fixture.query("s1"));
    EXPECT_EQ(relatives(result), (std::vector<std::string>{"charts/sales.png"}));
}

// 场景:压缩 / 回退把会话记录整个换成更短的内容(文件身份或长度变了)。
// 期望:从头重扫,旧记录里的候选不残留。
// 回归:只按「已读偏移」续读时,文件变短后 offset 越界,读盘抛异常。
TEST(SessionOutputsIndex, RescansWhenHistoryFileIsReplaced) {
    OutputsFixture fixture;
    const std::string old_file = fixture.touch("old.md");
    const std::string new_file = fixture.touch("new.md");
    acecode::SessionStorage::write_messages(fixture.jsonl("s1"),
        {write_result(old_file), write_result(fixture.touch("filler-with-a-long-name.md"))});

    acecode::SessionOutputsIndex index;
    EXPECT_EQ(index.query(fixture.query("s1")).files.size(), 2u);

    acecode::SessionStorage::write_messages(fixture.jsonl("s1"), {write_result(new_file)});
    EXPECT_EQ(relatives(index.query(fixture.query("s1"))), (std::vector<std::string>{"new.md"}));
}

// 场景:主会话派生了子代理,文件是子代理写的(记录在子会话自己的 JSONL 里)。
// 期望:主会话的产物列表包含子代理写的文件;子会话记录不存在时不报错。
TEST(SessionOutputsIndex, FollowsSubagentSessions) {
    OutputsFixture fixture;
    ChatMessage spawned;
    spawned.role = "tool";
    spawned.metadata = {{"tool_success", true}, {"subagent_session_id", "child-1"}};
    ChatMessage missing = spawned;
    missing.metadata["subagent_session_id"] = "child-missing";
    acecode::SessionStorage::write_messages(fixture.jsonl("root"), {spawned, missing});
    acecode::SessionStorage::write_messages(fixture.jsonl("child-1"),
        {write_result(fixture.touch("from-child.docx"))});

    acecode::SessionOutputsIndex index;
    EXPECT_EQ(relatives(index.query(fixture.query("root"))),
              (std::vector<std::string>{"from-child.docx"}));
}

// 场景:产物很多,面板只要最近的 N 个。
// 期望:按文件修改时间新 → 旧截断到 limit,has_more 为 true。
TEST(SessionOutputsIndex, OrdersByModifiedTimeAndReportsHasMore) {
    OutputsFixture fixture;
    std::vector<ChatMessage> messages;
    // 记录顺序与修改时间顺序相反:a 最先写但最近被改过。
    messages.push_back(write_result(fixture.touch("a.md", 0)));
    messages.push_back(write_result(fixture.touch("b.md", 120)));
    messages.push_back(write_result(fixture.touch("c.md", 60)));
    acecode::SessionStorage::write_messages(fixture.jsonl("s1"), messages);

    acecode::SessionOutputsIndex index;
    auto query = fixture.query("s1");
    query.limit = 2;
    const auto result = index.query(query);
    EXPECT_TRUE(result.has_more);
    EXPECT_EQ(relatives(result), (std::vector<std::string>{"a.md", "c.md"}));
}

// 场景:请求里的会话 id 带路径成分(../ 之类)。
// 期望:直接返回空,不拿它去拼文件名读别处的 .jsonl。
TEST(SessionOutputsIndex, RejectsSessionIdsWithPathComponents) {
    OutputsFixture fixture;
    acecode::SessionOutputsIndex index;
    auto query = fixture.query("../project/s1");
    EXPECT_TRUE(index.query(query).files.empty());
}
