#include <gtest/gtest.h>

#include "remote_control/rc_session_navigation.hpp"
#include "web/remote_control_session_event.hpp"

namespace {

using acecode::rc::RcSessionCommandKind;
using acecode::rc::RcSessionTarget;

RcSessionTarget target(std::string id, std::string updated, std::string title = {}) {
    RcSessionTarget value;
    value.session_id = std::move(id);
    value.updated_at = std::move(updated);
    value.title = std::move(title);
    value.workspace_label = "workspace";
    return value;
}

} // namespace

TEST(RcSessionNavigation, AliasesParseIdentically) {
    for (const auto* command : {"/session", "/sessions", "/resume", "/SeSsIoNs"}) {
        const auto parsed = acecode::rc::parse_rc_session_command(command);
        EXPECT_EQ(parsed.kind, RcSessionCommandKind::Recent);
    }
    EXPECT_EQ(acecode::rc::parse_rc_session_command("/sessions more").kind,
              RcSessionCommandKind::All);
    EXPECT_EQ(acecode::rc::parse_rc_session_command("/resume all").kind,
              RcSessionCommandKind::All);
}

TEST(RcSessionNavigation, SearchSelectAndInvalidCommands) {
    const auto search = acecode::rc::parse_rc_session_command("/session search important fix");
    EXPECT_EQ(search.kind, RcSessionCommandKind::Search);
    EXPECT_EQ(search.query, "important fix");

    const auto tab_search =
        acecode::rc::parse_rc_session_command("/session search\t关键词");
    EXPECT_EQ(tab_search.kind, RcSessionCommandKind::Search);
    EXPECT_EQ(tab_search.query, "关键词");

    const auto select = acecode::rc::parse_rc_session_command("/resume 12");
    EXPECT_EQ(select.kind, RcSessionCommandKind::Select);
    EXPECT_EQ(select.selection, 12u);

    EXPECT_EQ(acecode::rc::parse_rc_session_command("/sessions search").kind,
              RcSessionCommandKind::UsageError);
    EXPECT_EQ(acecode::rc::parse_rc_session_command("/sessions 0").kind,
              RcSessionCommandKind::UsageError);
    EXPECT_EQ(acecode::rc::parse_rc_session_command("/sessions banana").kind,
              RcSessionCommandKind::UsageError);
    EXPECT_EQ(acecode::rc::parse_rc_session_command("ordinary agent text").kind,
              RcSessionCommandKind::NotCommand);
}

TEST(RcSessionNavigation, ChunkingNeverSplitsUtf8Codepoints) {
    const std::string text = "你好吗世界";
    const auto chunks = acecode::rc::chunk_rc_session_output(text, 4);
    ASSERT_EQ(chunks.size(), 5u);
    EXPECT_EQ(chunks[0], "你");
    EXPECT_EQ(chunks[1], "好");
    EXPECT_EQ(chunks[2], "吗");
    EXPECT_EQ(chunks[3], "世");
    EXPECT_EQ(chunks[4], "界");
    std::string joined;
    for (const auto& chunk : chunks) joined += chunk;
    EXPECT_EQ(joined, text);
}

TEST(RcSessionNavigation, PersistedArchiveStateWinsOverActiveCatalogEntry) {
    auto persisted = std::vector<RcSessionTarget>{
        target("visible", "2026-08-05T10:00:00Z", "persisted title")};
    persisted.front().workspace_hash = "workspace-visible";
    auto archived = target("shared-id", "2026-08-05T09:00:00Z", "archived");
    archived.workspace_hash = "workspace-a";
    std::vector<RcSessionTarget> active{
        target("shared-id", "2026-08-05T12:00:00Z", "same scope hidden"),
        target("shared-id", "2026-08-05T11:30:00Z", "other scope visible"),
        target("visible", "2026-08-05T11:00:00Z", "active title"),
    };
    active[0].workspace_hash = "workspace-a";
    active[1].workspace_hash = "workspace-b";
    active[2].workspace_hash = "workspace-visible";
    for (auto& item : active) item.active = true;
    acecode::rc::merge_active_rc_session_targets(
        persisted, active, std::vector<RcSessionTarget>{archived});
    ASSERT_EQ(persisted.size(), 2u);
    EXPECT_EQ(persisted[0].session_id, "visible");
    EXPECT_EQ(persisted[0].title, "active title");
    EXPECT_TRUE(persisted[0].active);
    EXPECT_EQ(persisted[1].session_id, "shared-id");
    EXPECT_EQ(persisted[1].workspace_hash, "workspace-b");
    EXPECT_EQ(persisted[1].title, "other scope visible");
}

TEST(RcSessionNavigation, RankingFilteringAndSnapshotAreStable) {
    std::vector<RcSessionTarget> items{
        target("old", "2026-01-01T00:00:00Z", "old title"),
        target("new", "2026-01-03T00:00:00Z", "new title"),
        target("middle", "2026-01-02T00:00:00Z", "needle title"),
    };
    items[0].summary = "needle in content";
    items[0].content_match_score = 9;
    acecode::rc::sort_rc_session_targets(items);
    ASSERT_EQ(items.front().session_id, "new");

    const auto found = acecode::rc::filter_rc_session_targets(items, "needle", 5);
    ASSERT_EQ(found.size(), 2u);
    EXPECT_EQ(found.front().session_id, "old");
    EXPECT_EQ(found.back().session_id, "middle");

    const auto selected = acecode::rc::select_rc_session_snapshot(found, 2);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->session_id, "middle");
    EXPECT_FALSE(acecode::rc::select_rc_session_snapshot(found, 0).has_value());
    EXPECT_FALSE(acecode::rc::select_rc_session_snapshot(found, 3).has_value());
}

TEST(RcSessionNavigation, ListingNumbersContinuouslyAndChunksWithoutLosingRows) {
    std::vector<RcSessionTarget> items;
    for (int i = 0; i < 14; ++i) {
        auto item = target("id-" + std::to_string(i), "2026-01-01T00:00:00Z",
                           "long session title " + std::to_string(i));
        items.push_back(std::move(item));
    }
    const auto listing = acecode::rc::format_rc_session_listing(items, "All sessions:");
    EXPECT_NE(listing.find("1. long session title 0"), std::string::npos);
    EXPECT_NE(listing.find("14. long session title 13"), std::string::npos);
    const auto chunks = acecode::rc::chunk_rc_session_output(listing, 80);
    ASSERT_GT(chunks.size(), 1u);
    std::string joined;
    for (const auto& chunk : chunks) joined += chunk;
    EXPECT_NE(joined.find("14. long session title 13"), std::string::npos);
}

TEST(RcSessionNavigation, SelectionWebSocketEventHasOnlyTargetDescriptorFields) {
    const auto event = acecode::web::remote_control_session_selected_event_json(
        "session-1", "workspace-1", "C:/work", false, "Fix regression",
        "2026-08-05T00:00:00Z");
    ASSERT_EQ(event.value("type", std::string{}), "remote_control_session_selected");
    ASSERT_TRUE(event.contains("payload"));
    const auto& payload = event["payload"];
    EXPECT_EQ(payload.value("session_id", std::string{}), "session-1");
    EXPECT_EQ(payload.value("workspace_hash", std::string{}), "workspace-1");
    EXPECT_EQ(payload.value("cwd", std::string{}), "C:/work");
    EXPECT_FALSE(payload.value("no_workspace", true));
    EXPECT_EQ(payload.value("title", std::string{}), "Fix regression");
    EXPECT_EQ(payload.value("updated_at", std::string{}), "2026-08-05T00:00:00Z");
    EXPECT_TRUE(payload.value("remote_control_bound", false));
    EXPECT_FALSE(payload.contains("token"));
    EXPECT_FALSE(payload.contains("binding_token"));
    EXPECT_FALSE(payload.contains("manifest_path"));
    EXPECT_FALSE(payload.contains("outbound_url"));
}
