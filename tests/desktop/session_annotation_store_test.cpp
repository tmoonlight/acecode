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
    // 删除不回退计数器，避免 id 复用。
    EXPECT_EQ(store->next_number("page-1"), 3u);
}

} // namespace
} // namespace acecode::desktop
