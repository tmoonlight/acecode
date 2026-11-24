#include "desktop/agent_browser_host.hpp"
#include "desktop/page_annotation_host_navigation.hpp"

#include <gtest/gtest.h>

namespace acecode::desktop {
namespace {

struct NavigationPage {
    std::string id = "page";
    bool closing = false;
    PageAnnotationSession annotations;
    bool webview = true;
    int store = 1;
    AgentBrowserState state;
};

struct NavigationHost : std::enable_shared_from_this<NavigationHost> {
    using Page = NavigationPage;
    std::vector<std::shared_ptr<Page>> pages;
    int updates = 0;
    auto find_page(const std::string& id) {
        const auto found = std::find_if(pages.begin(), pages.end(), [&](const auto& page) { return page->id == id; });
        return found == pages.end() ? nullptr : *found;
    }
    auto states() {
        std::vector<AgentBrowserState> result;
        for (const auto& page : pages) result.push_back(page->state);
        return result;
    }
    template <typename Update>
    void update_page(const std::shared_ptr<Page>& page, Update update) { ++updates; update(page->state); }
};

#ifdef _WIN32
void pump_annotation_timers(DWORD duration, const std::function<bool()>& finished = [] { return false; }) {
    const auto deadline = GetTickCount64() + duration;
    while (!finished() && GetTickCount64() < deadline) {
        const auto remaining = static_cast<DWORD>(deadline - GetTickCount64());
        MsgWaitForMultipleObjects(0, nullptr, FALSE, remaining, QS_TIMER);
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_TIMER, WM_TIMER, PM_REMOVE)) DispatchMessageW(&message);
    }
}

TEST(PageAnnotationNavigation, PollTimerCanBeStoppedOrDestroyedInsideItsOwnCallback) {
    int calls = 0;
    {
        AnnotationPoller stopped;
        stopped.start([&] { ++calls; });
        stopped.stop();
        stopped.stop();
        pump_annotation_timers(300);
        EXPECT_EQ(calls, 0);
    }
    auto poller = std::make_unique<AnnotationPoller>();
    poller->start([&] { ++calls; poller.reset(); });
    poller->start([&] { calls += 100; });
    pump_annotation_timers(2000, [&] { return calls > 0; });
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(poller);
    pump_annotation_timers(300);
    EXPECT_EQ(calls, 1);
}
#endif

TEST(PageAnnotationNavigation, StoreCompletionOnlyReloadsFrozenCurrentReservedPages) {
    auto host = std::make_shared<NavigationHost>();
    for (const auto& id : {"first", "switched", "closed", "superseded", "replaced"}) {
        const auto page = std::make_shared<NavigationPage>();
        page->id = id;
        page->state.page_id = id;
        host->pages.push_back(page);
    }
    const auto pages = annotation_store_pages(*host, 1);
    ASSERT_EQ(pages.size(), 5);
    ASSERT_TRUE(reserve_annotation_transition(pages, [](const auto&) { return true; }, [] { return true; }));
    const auto tickets = annotation_transition_tickets(pages);
    pages[1]->store = 2;
    pages[2]->closing = true;
    pages[3]->annotations.navigation.finish();
    ASSERT_TRUE(pages[3]->annotations.navigation.reserve());
    host->pages.back() = std::make_shared<NavigationPage>();
    host->pages.back()->id = "replaced";
    std::vector<std::string> reloaded;
    annotation_reload_reserved(*host, tickets, 1, true, [&](const auto& page) {
        reloaded.push_back(page->id);
        EXPECT_TRUE(page->annotations.navigation.armed());
        return false;
    });
    EXPECT_EQ(reloaded, std::vector<std::string>{"first"});
    EXPECT_FALSE(pages[0]->annotations.navigation.blocked());
    EXPECT_FALSE(pages[1]->annotations.navigation.blocked());
    EXPECT_TRUE(pages[3]->annotations.navigation.reserved());
}

TEST(PageAnnotationNavigation, PassivePollBoundsFlightsAndRejectsRetiredDocumentOrHost) {
    auto host = std::make_shared<NavigationHost>();
    const auto page = std::make_shared<NavigationPage>();
    page->state.page_id = page->id;
    page->state.content_state = kAgentBrowserContentStateLive;
    host->pages.push_back(page);
    std::string expression;
    std::string error;
    ASSERT_TRUE(page->annotations.prepare(page->id, {{"action", "start"}, {"document_id", ""}}, expression, error));
    page->annotations.fail(page->annotations.sequence());
    std::function<void(bool, nlohmann::json)> done;
    int evaluations = 0;
    const auto evaluate = [&](const auto&, const auto&, auto, const auto&, auto callback) {
        ++evaluations;
        done = callback;
    };
    const std::weak_ptr<NavigationHost> weak = host;
    const auto sequence = page->annotations.sequence();
    poll_annotations(weak, evaluate);
    poll_annotations(weak, evaluate);
    EXPECT_EQ(evaluations, 1);
    EXPECT_EQ(sequence, page->annotations.sequence());
    page->annotations.replace_document();
    const auto revision = page->annotations.revision();
    done(true, {{"error", "replaced_document"}});
    EXPECT_EQ(page->annotations.revision(), revision);
    EXPECT_EQ(host->updates, 0);
    ASSERT_TRUE(page->annotations.prepare(page->id, {{"action", "start"}, {"document_id", ""}}, expression, error));
    page->annotations.fail(page->annotations.sequence());
    poll_annotations(weak, evaluate);
    EXPECT_EQ(evaluations, 2);
    host.reset();
    done(true, {{"error", "replaced_document"}});
    EXPECT_EQ(page->annotations.revision(), revision);
}

TEST(PageAnnotationNavigation, PassivePollRetiresAStaleRuntimeDocument) {
    auto host = std::make_shared<NavigationHost>();
    const auto page = std::make_shared<NavigationPage>();
    page->state.page_id = page->id;
    page->state.content_state = kAgentBrowserContentStateLive;
    host->pages.push_back(page);
    std::string expression;
    std::string error;
    ASSERT_TRUE(page->annotations.prepare(page->id, {{"action", "start"}, {"document_id", ""}}, expression, error));
    const std::string document = page->annotations.document_id();
    page->annotations.fail(page->annotations.sequence());

    std::function<void(bool, nlohmann::json)> done;
    poll_annotations(std::weak_ptr<NavigationHost>(host),
        [&](const auto&, const auto&, auto, const auto&, auto callback) { done = callback; });
    ASSERT_TRUE(done);
    done(true, {{"error", "stale_document"}});

    EXPECT_TRUE(page->annotations.document_id().empty());
    EXPECT_FALSE(page->annotations.document_id() == document);
    EXPECT_EQ(host->updates, 1);
}

TEST(PageAnnotationNavigation, UiTickExpiresRequestsEvenWhenDocumentCannotBePolled) {
    const auto host = std::make_shared<NavigationHost>();
    const auto page = std::make_shared<NavigationPage>();
    page->state.page_id = page->id;
    host->pages.push_back(page);
    int completions = 0;
    const auto held = page->annotations.requests.add([&](nlohmann::json result) {
        ++completions;
        EXPECT_EQ(result["ok"], false);
        EXPECT_EQ(result["recoverable"], true);
    }, AnnotationRequests::Clock::now() - std::chrono::seconds(16));
    ASSERT_TRUE(page->annotations.navigation.reserve());
    poll_annotations(std::weak_ptr<NavigationHost>(host), [](const auto&, const auto&, auto, const auto&, auto) {
        ADD_FAILURE() << "Inactive document must not be evaluated";
    });
    EXPECT_EQ(completions, 1);
    EXPECT_TRUE(held->completed());
    held->finish({{"ok", true}});
    EXPECT_EQ(completions, 1);
}

TEST(PageAnnotationNavigation, RollbackNeverClearsASupersedingTransition) {
    const auto page = std::make_shared<NavigationPage>();
    std::string expression;
    std::string error;
    ASSERT_TRUE(page->annotations.prepare(page->id, {{"action", "start"}, {"document_id", ""}}, expression, error));
    EXPECT_FALSE(reserve_annotation_transition(std::vector{page}, [](const auto&) { return true; }, [&] {
        page->annotations.navigation.finish();
        EXPECT_TRUE(page->annotations.navigation.reserve());
        return true;
    }));
    EXPECT_TRUE(page->annotations.navigation.reserved());
}

TEST(PageAnnotationNavigation, ReservationRequiresExplicitArmingAndBlocksWhileHeld) {
    AnnotationNavigationGuard guard;
    EXPECT_TRUE(guard.reserve());
    EXPECT_FALSE(guard.start());
    EXPECT_TRUE(guard.arm());
    EXPECT_TRUE(guard.start());
    EXPECT_FALSE(guard.start());
    EXPECT_TRUE(guard.start(true));
    EXPECT_FALSE(guard.reserve());
    guard.finish();
    EXPECT_TRUE(guard.reserve());
    EXPECT_TRUE(guard.blocked());
    guard.finish();
    EXPECT_FALSE(guard.blocked());
}

TEST(PageAnnotationNavigation, AllEntryMethodsShareTheDestructiveTransitionFence) {
    for (const auto& method : {"Page.navigate", "Page.reload", "Page.navigateToHistoryEntry", "Page.setDocumentContent"}) {
        EXPECT_TRUE(annotation_replaces_document(method));
    }
    for (const auto& method : {"Runtime.evaluate", "Page.captureScreenshot", "Page.getNavigationHistory"}) {
        EXPECT_FALSE(annotation_replaces_document(method));
    }
    EXPECT_TRUE(annotation_fragment_navigation("https://a.test/page#one", "https://a.test/page#two"));
    EXPECT_FALSE(annotation_fragment_navigation("https://a.test/page", "https://a.test/other#two"));
    EXPECT_FALSE(annotation_fragment_navigation("https://a.test/page", "https://a.test/page"));
}

TEST(PageAnnotationNavigation, DefaultStayAndReentrantChangesReleaseAllReservations) {
    const auto page = std::make_shared<NavigationPage>();
    std::string expression;
    std::string error;
    ASSERT_TRUE(page->annotations.prepare("page", {{"action", "start"}, {"document_id", ""}}, expression, error));
    const std::vector<std::shared_ptr<NavigationPage>> pages{page};
    const auto current = [](const auto& candidate) { return !candidate->closing; };
    int prompts = 0;
    EXPECT_FALSE(reserve_annotation_transition(pages, current, [&] {
        ++prompts;
        EXPECT_FALSE(page->annotations.navigation.start());
        return false;
    }));
    EXPECT_EQ(prompts, 1);
    EXPECT_FALSE(page->annotations.navigation.blocked());
    EXPECT_FALSE(reserve_annotation_transition(pages, current, [&] {
        page->annotations.replace_document();
        return true;
    }));
    EXPECT_FALSE(page->annotations.navigation.blocked());
    ASSERT_TRUE(page->annotations.prepare("page", {{"action", "start"}, {"document_id", ""}}, expression, error));
    EXPECT_FALSE(reserve_annotation_transition(pages, current, [&] {
        page->closing = true;
        return true;
    }));
    EXPECT_FALSE(page->annotations.navigation.blocked());
}

TEST(PageAnnotationNavigation, MultiplePagesAreReservedBeforeAnyConfirmationOrSideEffect) {
    const auto first = std::make_shared<NavigationPage>();
    const auto second = std::make_shared<NavigationPage>();
    const auto current = [](const auto&) { return true; };
    ASSERT_TRUE(second->annotations.navigation.reserve());
    int prompts = 0;
    EXPECT_FALSE(reserve_annotation_transition(std::vector{first, second}, current, [&] { ++prompts; return true; }));
    EXPECT_EQ(prompts, 0);
    EXPECT_FALSE(first->annotations.navigation.blocked());
    second->annotations.navigation.finish();
    EXPECT_TRUE(reserve_annotation_transition(std::vector{first, second}, current, [&] { ++prompts; return true; }));
    EXPECT_EQ(prompts, 0);
    const auto ticket = first->annotations.navigation.ticket();
    first->annotations.navigation.finish();
    ASSERT_TRUE(first->annotations.navigation.reserve());
    EXPECT_NE(ticket, first->annotations.navigation.ticket());
    EXPECT_TRUE(second->annotations.navigation.blocked());
}

}
}
