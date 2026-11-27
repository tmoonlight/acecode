#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "desktop/page_annotation_session.hpp"
#include "desktop/session_annotation_store.hpp"

namespace acecode::desktop {
namespace {

using Json = nlohmann::json;

// 记录归属会话（Q33）的存储契约：标识与文档无关（D9）、编号按页签分组、
// 丢弃只作用于本页签、页签析构不影响记录。
class SessionAnnotationStoreTest : public testing::Test {
protected:
    struct Driver {
        explicit Driver(std::shared_ptr<SessionAnnotationStore> shared, std::string page)
            : store(std::move(shared)), page_id(std::move(page)), session(store) {}

        Json command(const std::string& action) const {
            Json request{{"action", action}, {"document_id", session.snapshot()["document_id"]}};
            if (action == "save") {
                request["target_ids"] = annotation_target_ids(session.snapshot()["targets"]);
                request["region"] = session.snapshot()["region"];
            }
            return request;
        }

        Json result(bool active = true, bool popup = false, Json target = nullptr) const {
            return {{"document_id", session.snapshot()["document_id"]},
                    {"active", active}, {"popup", popup}, {"target", target},
                    {"original", false}, {"differences", Json::array()},
                    {"targets", target.is_null() ? Json::array() : Json::array({target})},
                    {"region", nullptr}, {"selection_mode", nullptr},
                    {"viewport", {{"width", 800}, {"height", 600}, {"scroll_x", 0}, {"scroll_y", 0}}}};
        }

        Json target() const {
            return {{"id", "target-1"}, {"tag", "button"}, {"text", "Click"},
                    {"locator", "button#target"},
                    {"bounds", {{"x", 10}, {"y", 20}, {"width", 80}, {"height", 30}}}};
        }

        void start() {
            ASSERT_TRUE(session.prepare(page_id, command("start"), expression, error)) << error;
            ASSERT_TRUE(session.complete(session.sequence(), result(), error)) << error;
        }

        void select() {
            ASSERT_TRUE(session.prepare(page_id, command("read"), expression, error)) << error;
            ASSERT_TRUE(session.complete(session.sequence(), result(true, true, target()), error)) << error;
        }

        void save(const std::string& description) {
            auto request = command("save");
            request["description"] = description;
            request["target_id"] = "target-1";
            ASSERT_TRUE(session.prepare(page_id, request, expression, error)) << error;
            ASSERT_TRUE(session.complete(session.sequence(), result(), error)) << error;
        }

        void discard() {
            ASSERT_TRUE(session.prepare(page_id, command("discard"), expression, error)) << error;
            ASSERT_TRUE(session.complete(session.sequence(), result(), error)) << error;
        }

        std::shared_ptr<SessionAnnotationStore> store;
        std::string page_id;
        PageAnnotationSession session;
        std::string expression;
        std::string error;
    };
};

TEST_F(SessionAnnotationStoreTest, RecordIdentityDoesNotEmbedDocumentGeneration) {
    auto store = std::make_shared<SessionAnnotationStore>();
    Driver driver(store, "page-1");
    driver.start();
    driver.select();
    driver.save("first");
    const auto document = driver.session.snapshot()["document_id"];
    driver.select();
    driver.save("second");

    const auto& records = store->records("page-1");
    ASSERT_EQ(records.size(), 2u);
    // 标识 = <page_id>:annotation:<n>，不含 document_id / epoch（D9）。
    EXPECT_EQ(records[0]["id"], "page-1:annotation:1");
    EXPECT_EQ(records[1]["id"], "page-1:annotation:2");
    EXPECT_EQ(records[0]["page_id"], "page-1");
    EXPECT_EQ(records[0]["document_id"], document);
    EXPECT_EQ(records[0]["id"].get<std::string>().find(":document:"), std::string::npos);

    // 文档替换后仍能按同一编号继续，且旧记录不被删除——旧实现会把它们清空。
    driver.session.replace_document();
    driver.start();
    driver.select();
    driver.save("third");
    const auto& after = store->records("page-1");
    ASSERT_EQ(after.size(), 3u);
    EXPECT_EQ(after[2]["id"], "page-1:annotation:3");
    EXPECT_NE(after[2]["document_id"], document);
    EXPECT_EQ(after[0]["id"], "page-1:annotation:1");
}

TEST_F(SessionAnnotationStoreTest, NumberingIsIndependentPerPage) {
    auto store = std::make_shared<SessionAnnotationStore>();
    Driver a(store, "page-a");
    Driver b(store, "page-b");
    a.start();
    a.select();
    a.save("a-first");
    a.select();
    a.save("a-second");
    b.start();
    b.select();
    b.save("b-first");

    EXPECT_EQ(store->records("page-a").size(), 2u);
    EXPECT_EQ(store->records("page-b").size(), 1u);
    EXPECT_EQ(store->next_number("page-a"), 3u);
    EXPECT_EQ(store->next_number("page-b"), 2u);
    // 每个页签的编号都从 1 开始，和旧实现（每页一个计数器）等价。
    EXPECT_EQ(store->records("page-b")[0]["id"], "page-b:annotation:1");
    EXPECT_EQ(store->records("page-b")[0]["number"], 1);
    // 快照只暴露本页签的记录，页签之间互不可见。
    EXPECT_EQ(a.session.snapshot()["records"].size(), 2u);
    EXPECT_EQ(b.session.snapshot()["records"].size(), 1u);
}

TEST_F(SessionAnnotationStoreTest, DiscardClearsOnlyTheDiscardingPage) {
    auto store = std::make_shared<SessionAnnotationStore>();
    Driver a(store, "page-a");
    Driver b(store, "page-b");
    a.start();
    a.select();
    a.save("a-first");
    b.start();
    b.select();
    b.save("b-first");

    a.discard();
    EXPECT_EQ(store->records("page-a").size(), 0u);
    EXPECT_EQ(store->next_number("page-a"), 1u);
    EXPECT_EQ(store->records("page-b").size(), 1u);
    EXPECT_EQ(store->next_number("page-b"), 2u);

    // 丢弃后编号确实从 1 重来，不会和旧记录撞 id。
    a.select();
    a.save("a-again");
    ASSERT_EQ(store->records("page-a").size(), 1u);
    EXPECT_EQ(store->records("page-a")[0]["id"], "page-a:annotation:1");
}

TEST_F(SessionAnnotationStoreTest, RecordsOutliveThePageThatWroteThem) {
    auto store = std::make_shared<SessionAnnotationStore>();
    {
        Driver driver(store, "page-1");
        driver.start();
        driver.select();
        driver.save("survives");
    }
    // 页签对象销毁（关页签）后记录仍在 store 里——这是 orphan 语义的前提。
    ASSERT_EQ(store->records("page-1").size(), 1u);
    EXPECT_EQ(store->records("page-1")[0]["description"], "survives");
    EXPECT_EQ(store->records("page-1")[0]["status"], "pending");
}

TEST_F(SessionAnnotationStoreTest, EraseDropsOneRecordAndBumpsRevision) {
    auto store = std::make_shared<SessionAnnotationStore>();
    Driver driver(store, "page-1");
    driver.start();
    driver.select();
    driver.save("keep");
    driver.select();
    driver.save("drop");

    const auto before = store->revision();
    EXPECT_FALSE(store->erase("page-1", "page-1:annotation:404"));
    EXPECT_EQ(store->revision(), before);
    EXPECT_TRUE(store->erase("page-1", "page-1:annotation:2"));
    EXPECT_GT(store->revision(), before);
    ASSERT_EQ(store->records("page-1").size(), 1u);
    EXPECT_EQ(store->records("page-1")[0]["description"], "keep");
    // 下一号从仍存活的记录重算；已发出/删除的最高号不再抬高新记录。
    EXPECT_EQ(store->next_number("page-1"), 2u);
    store->append("page-1", {{"document_id", driver.session.document_id()},
                             {"description", "replacement"}, {"targets", Json::array()},
                             {"differences", Json::array()}, {"region", nullptr},
                             {"status", "pending"}, {"valid", true}});
    EXPECT_EQ(store->records("page-1").back()["id"], "page-1:annotation:2");
}

TEST_F(SessionAnnotationStoreTest, SnapshotIsServedByTheStoreNotTheTab) {
    auto store = std::make_shared<SessionAnnotationStore>();
    Driver driver(store, "page-1");
    driver.start();
    ASSERT_TRUE(driver.session.snapshot()["records"].empty());

    // 直接改存储（不经过页签的任何命令），快照必须跟着变——存储是权威（#210）。
    store->append("page-1", {{"document_id", driver.session.document_id()},
                             {"description", "poked"}, {"targets", Json::array()},
                             {"differences", Json::array()}, {"region", nullptr},
                             {"status", "pending"}, {"valid", true}});
    const auto snapshot = driver.session.snapshot();
    ASSERT_EQ(snapshot["records"].size(), 1u);
    EXPECT_EQ(snapshot["records"][0]["description"], "poked");
    EXPECT_EQ(snapshot["pending_count"], 1);
}

TEST_F(SessionAnnotationStoreTest, AttachScreenshotStoresThumbnailAndPageShot) {
    auto store = std::make_shared<SessionAnnotationStore>();
    Driver driver(store, "page-1");
    driver.start();
    driver.select();
    driver.save("needs a picture");
    const auto id = store->records("page-1")[0]["id"].get<std::string>();

    std::string error;
    ASSERT_TRUE(driver.session.submission({
        {"action", "attach_screenshot"}, {"document_id", ""}, {"record_id", id},
        {"thumbnail", "data:image/png;base64,AAA"},
        {"page_shot", "data:image/png;base64,BBB"},
    }, error)) << error;
    const auto snapshot = driver.session.snapshot();
    EXPECT_EQ(snapshot["page_shot"], "data:image/png;base64,BBB");
    EXPECT_EQ(store->records("page-1")[0]["thumbnail"], "data:image/png;base64,AAA");

    // 只更新整屏图时不动记录上的缩略图（每页只留最新一张）。
    ASSERT_TRUE(driver.session.submission({
        {"action", "attach_screenshot"}, {"document_id", ""},
        {"page_shot", "data:image/png;base64,CCC"},
    }, error)) << error;
    EXPECT_EQ(driver.session.snapshot()["page_shot"], "data:image/png;base64,CCC");
    EXPECT_EQ(store->records("page-1")[0]["thumbnail"], "data:image/png;base64,AAA");

    // 非 image data URL、超过体积上限、未知记录都要拒绝。
    EXPECT_FALSE(driver.session.submission({
        {"action", "attach_screenshot"}, {"document_id", ""}, {"record_id", id},
        {"thumbnail", "https://evil.invalid/x.png"},
    }, error));
    EXPECT_FALSE(driver.session.submission({
        {"action", "attach_screenshot"}, {"document_id", ""}, {"record_id", "page-1:annotation:404"},
        {"thumbnail", "data:image/png;base64,AAA"},
    }, error));
}

TEST_F(SessionAnnotationStoreTest, AttachScreenshotAcceptsRealPngPayloadsButCapsTheCommand) {
    auto store = std::make_shared<SessionAnnotationStore>();
    Driver driver(store, "page-1");
    driver.start();
    driver.select();
    driver.save("needs a real picture");
    const auto id = store->records("page-1")[0]["id"].get<std::string>();

    // 真实网页的整屏 PNG 远大于普通提交上限；补图命令要能过。
    const std::string page_shot = "data:image/png;base64," + std::string(1024 * 1024, 'A');
    std::string error;
    ASSERT_TRUE(driver.session.submission({
        {"action", "attach_screenshot"}, {"document_id", ""}, {"record_id", id},
        {"thumbnail", "data:image/png;base64,AAA"}, {"page_shot", page_shot},
    }, error)) << error;
    EXPECT_EQ(driver.session.snapshot()["page_shot"].get<std::string>().size(), page_shot.size());

    // 上限仍然存在：超过 8 MiB 的整条命令被拒，且不改动已存状态。
    const auto before = driver.session.snapshot();
    EXPECT_FALSE(driver.session.submission({
        {"action", "attach_screenshot"}, {"document_id", ""},
        {"page_shot", "data:image/png;base64," + std::string(8 * 1024 * 1024, 'B')},
    }, error));
    EXPECT_EQ(error, "Invalid annotation submission");
    EXPECT_EQ(driver.session.snapshot(), before);
}

// 触发场景:一页上存了两条批注并补了整屏图,随后发送成功,前端按 id 把已发的
// 记录从原生存储里删掉(先删一条,再删最后一条)。
// 期望行为:还有待发记录时整屏图留着(发送时要上传它);最后一条删掉后整屏图
// 一并释放。
// 回归表现:整屏图(真实网页是几百 KB 到几 MB 的 data URL)在记录清空后仍留在
// 会话级存储里,之后这个页签的每次状态事件、每次批注轮询都整份带着它。
TEST_F(SessionAnnotationStoreTest, RemovingTheLastRecordReleasesThePageShot) {
    auto store = std::make_shared<SessionAnnotationStore>();
    Driver driver(store, "page-1");
    driver.start();
    driver.select();
    driver.save("first");
    driver.start();
    driver.select();
    driver.save("second");
    ASSERT_EQ(store->records("page-1").size(), 2u);
    const auto first = store->records("page-1")[0]["id"].get<std::string>();
    const auto second = store->records("page-1")[1]["id"].get<std::string>();

    std::string error;
    ASSERT_TRUE(driver.session.submission({
        {"action", "attach_screenshot"}, {"document_id", ""}, {"record_id", second},
        {"thumbnail", "data:image/png;base64,AAA"},
        {"page_shot", "data:image/png;base64,BBB"},
    }, error)) << error;

    ASSERT_TRUE(driver.session.submission({
        {"action", "remove"}, {"document_id", ""}, {"record_ids", Json::array({first})},
    }, error)) << error;
    EXPECT_EQ(driver.session.snapshot()["page_shot"], "data:image/png;base64,BBB");

    ASSERT_TRUE(driver.session.submission({
        {"action", "remove"}, {"document_id", ""}, {"record_ids", Json::array({second})},
    }, error)) << error;
    EXPECT_TRUE(store->records("page-1").empty());
    EXPECT_EQ(driver.session.snapshot()["page_shot"], "");
}

TEST_F(SessionAnnotationStoreTest, RemoveErasesRecordsWithoutDocumentCheck) {
    auto store = std::make_shared<SessionAnnotationStore>();
    Driver driver(store, "page-1");
    driver.start();
    driver.select();
    driver.save("keep");
    driver.select();
    driver.save("sent");
    const auto sent = store->records("page-1")[1]["id"].get<std::string>();

    std::string error;
    // 文档代传空也能删：发送成功后清理不该被「这一刻文档刚好换了」挡住。
    ASSERT_TRUE(driver.session.submission({
        {"action", "remove"}, {"document_id", ""}, {"record_ids", Json::array({sent})},
    }, error)) << error;
    ASSERT_EQ(store->records("page-1").size(), 1u);
    EXPECT_EQ(store->records("page-1")[0]["description"], "keep");

    EXPECT_FALSE(driver.session.submission({
        {"action", "remove"}, {"document_id", ""},
        {"record_ids", Json::array({"page-1:annotation:404"})},
    }, error));
}

TEST_F(SessionAnnotationStoreTest, RemovingAllSentRecordsRestartsNumberingAtOne) {
    auto store = std::make_shared<SessionAnnotationStore>();
    Driver driver(store, "page-1");
    driver.start();
    for (int index = 1; index <= 3; ++index) {
        driver.select();
        driver.save("sent-" + std::to_string(index));
    }
    ASSERT_EQ(driver.session.snapshot()["pending_count"], 3);
    EXPECT_EQ(store->next_number("page-1"), 4u);

    Json ids = Json::array();
    for (const auto& record : store->records("page-1")) ids.push_back(record["id"]);
    std::string error;
    ASSERT_TRUE(driver.session.submission({
        {"action", "remove"}, {"document_id", ""}, {"record_ids", ids},
    }, error)) << error;
    ASSERT_TRUE(store->records("page-1").empty());
    EXPECT_EQ(store->next_number("page-1"), 1u);

    driver.select();
    driver.save("new round");
    ASSERT_EQ(store->records("page-1").size(), 1u);
    EXPECT_EQ(store->records("page-1")[0]["id"], "page-1:annotation:1");
    EXPECT_EQ(store->records("page-1")[0]["number"], 1);
}

// 铅笔路径（#217）：改的是同一条记录，不新增、编号不变。
TEST_F(SessionAnnotationStoreTest, EditRecordUpdatesTheSameRecordInPlace) {
    auto store = std::make_shared<SessionAnnotationStore>();
    Driver driver(store, "page-1");
    driver.start();
    driver.select();
    driver.save("first");

    Json request{{"action", "edit_record"},
                 {"document_id", driver.session.document_id()},
                 {"record_id", "page-1:annotation:1"}};
    ASSERT_TRUE(driver.session.prepare("page-1", request, driver.expression, driver.error))
        << driver.error;
    // 回页面后 popup 必须打开、目标必须被重新选中，否则这次编辑没有落点。
    ASSERT_TRUE(driver.session.complete(driver.session.sequence(),
        driver.result(true, true, driver.target()), driver.error)) << driver.error;
    const auto popup = driver.session.snapshot();
    EXPECT_EQ(popup["draft_description"], "first");
    EXPECT_EQ(popup["editing_record_id"], "page-1:annotation:1");

    auto save = driver.command("save");
    save["description"] = "edited";
    save["target_id"] = "target-1";
    ASSERT_TRUE(driver.session.prepare("page-1", save, driver.expression, driver.error)) << driver.error;
    ASSERT_TRUE(driver.session.complete(driver.session.sequence(),
        driver.result(true, false), driver.error)) << driver.error;

    const auto& records = store->records("page-1");
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["id"], "page-1:annotation:1");
    EXPECT_EQ(records[0]["number"], 1);
    EXPECT_EQ(records[0]["description"], "edited");
    EXPECT_EQ(driver.session.snapshot()["editing_record_id"], "");
}

// 目标已失效的记录不能回页面改：调用方要退化成「只改文字」。
TEST_F(SessionAnnotationStoreTest, EditRecordRejectsStaleRecords) {
    auto store = std::make_shared<SessionAnnotationStore>();
    Driver driver(store, "page-1");
    driver.start();
    driver.select();
    driver.save("first");
    driver.session.replace_document();
    // 刷新后重新进入批注模式：文档代是新的，旧记录属于上一代。
    driver.start();

    Json request{{"action", "edit_record"},
                 {"document_id", driver.session.document_id()},
                 {"record_id", "page-1:annotation:1"}};
    EXPECT_FALSE(driver.session.prepare("page-1", request, driver.expression, driver.error));
    EXPECT_EQ(driver.error, "Annotation target is invalid");
}

} // namespace
} // namespace acecode::desktop
