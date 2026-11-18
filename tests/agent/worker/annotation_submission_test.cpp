#include <gtest/gtest.h>
#include "agent/worker/agent_task_queue.hpp"
#include <atomic>
#include <future>
#include <vector>

namespace {
using acecode::AnnotationSubmissionIdentity;
using acecode::AnnotationSubmissionStatus;
using acecode::agent::AgentTaskQueue;
using acecode::agent::WorkerTask;

WorkerTask annotation_task(std::string text = "annotation changes") {
    WorkerTask task;
    task.input.text = std::move(text);
    return task;
}

AnnotationSubmissionIdentity identity(std::string batch_id = "batch-1", char digest = 'a') {
    return {std::move(batch_id), std::string(64, digest)};
}
}

TEST(AnnotationSubmission, ConcurrentRetryAcceptsOneTaskAndReturnsTheSameReceipt) {
    std::atomic<bool> busy{true};
    AgentTaskQueue queue(busy);
    std::promise<void> start;
    const auto ready = start.get_future().share();
    std::vector<std::future<acecode::AnnotationSubmissionReceipt>> retries;
    for (int retry = 0; retry < 24; ++retry) {
        retries.push_back(std::async(std::launch::async, [queue_ptr = &queue, ready] {
            ready.wait();
            return queue_ptr->enqueue_annotation(annotation_task(), identity());
        }));
    }
    start.set_value();
    int first_acceptances = 0;
    for (auto& retry : retries) {
        const auto receipt = retry.get();
        EXPECT_TRUE(receipt.accepted());
        EXPECT_EQ(receipt.batch_id, "batch-1");
        if (receipt.status == AnnotationSubmissionStatus::Accepted) ++first_acceptances;
    }
    EXPECT_EQ(first_acceptances, 1);
    WorkerTask task;
    ASSERT_TRUE(queue.wait_pop(task));
    EXPECT_EQ(task.input.text, "annotation changes");
    EXPECT_EQ(task.input.metadata["annotation_batch_id"], "batch-1");
    queue.finish_task();
    EXPECT_FALSE(queue.has_user_work());
    EXPECT_EQ(queue.enqueue_annotation(annotation_task(), identity()).status,
              AnnotationSubmissionStatus::Duplicate);
    EXPECT_FALSE(queue.has_user_work());
    queue.request_shutdown();
}

TEST(AnnotationSubmission, ConflictDoesNotReplaceAcceptedPayloadOrQueueAnotherTurn) {
    std::atomic<bool> busy{false};
    AgentTaskQueue queue(busy);
    ASSERT_TRUE(queue.enqueue_annotation(annotation_task(), identity()).accepted());
    const auto conflict = queue.enqueue_annotation(annotation_task("changed"), identity("batch-1", 'b'));
    EXPECT_EQ(conflict.status, AnnotationSubmissionStatus::Conflict);
    EXPECT_TRUE(conflict.batch_id.empty());
    EXPECT_EQ(queue.enqueue_annotation(annotation_task(), identity()).status,
              AnnotationSubmissionStatus::Duplicate);
    queue.request_shutdown();
    const auto pending = queue.take_pending();
    ASSERT_EQ(pending.ordinary.size(), 1u);
    EXPECT_EQ(pending.ordinary.front().input.text, "annotation changes");
}

TEST(AnnotationSubmission, ConcurrentConflictingPayloadsHaveExactlyOneWinner) {
    std::atomic<bool> busy{false};
    AgentTaskQueue queue(busy);
    std::promise<void> start;
    const auto ready = start.get_future().share();
    std::vector<std::future<acecode::AnnotationSubmissionReceipt>> requests;
    for (int request = 0; request < 24; ++request) {
        requests.push_back(std::async(std::launch::async, [queue_ptr = &queue, ready, request] {
            ready.wait();
            const bool alternate = request % 2 != 0;
            return queue_ptr->enqueue_annotation(annotation_task(alternate ? "alternate" : "original"),
                identity("batch-race", alternate ? 'b' : 'a'));
        }));
    }
    start.set_value();
    int accepted = 0;
    int duplicates = 0;
    int conflicts = 0;
    for (auto& request : requests) {
        const auto receipt = request.get();
        if (receipt.status == AnnotationSubmissionStatus::Accepted) ++accepted;
        if (receipt.status == AnnotationSubmissionStatus::Duplicate) ++duplicates;
        if (receipt.status == AnnotationSubmissionStatus::Conflict) ++conflicts;
        EXPECT_EQ(receipt.batch_id, receipt.accepted() ? "batch-race" : "");
    }
    EXPECT_EQ(accepted, 1);
    EXPECT_EQ(duplicates, 11);
    EXPECT_EQ(conflicts, 12);
    queue.request_shutdown();
    const auto pending = queue.take_pending();
    EXPECT_EQ(pending.ordinary.size(), 1u);
}

TEST(AnnotationSubmission, FailedAdmissionDoesNotConsumeIdentityAndSessionsAreIndependent) {
    std::atomic<bool> busy{false};
    AgentTaskQueue first(busy);
    EXPECT_EQ(first.enqueue_annotation(annotation_task(""), identity()).status,
              AnnotationSubmissionStatus::Invalid);
    EXPECT_EQ(first.enqueue_annotation(annotation_task(), identity()).status,
              AnnotationSubmissionStatus::Accepted);
    AgentTaskQueue second(busy);
    EXPECT_EQ(second.enqueue_annotation(annotation_task(), identity()).status,
              AnnotationSubmissionStatus::Accepted);
    first.request_shutdown();
    const auto rejected = first.enqueue_annotation(annotation_task(), identity("not-accepted"));
    EXPECT_FALSE(rejected.accepted());
    EXPECT_TRUE(rejected.batch_id.empty());
    EXPECT_EQ(second.enqueue_annotation(annotation_task(), identity("not-accepted")).status,
              AnnotationSubmissionStatus::Accepted);
    second.request_shutdown();
}

TEST(AnnotationSubmission, CapacityNeverEvictsReceiptsAndDoesNotBlockOrdinaryInput) {
    std::atomic<bool> busy{false};
    AgentTaskQueue queue(busy);
    for (std::size_t batch = 0; batch < AgentTaskQueue::kMaxAnnotationBatches; ++batch) {
        ASSERT_EQ(queue.enqueue_annotation(annotation_task(), identity(std::to_string(batch))).status,
                  AnnotationSubmissionStatus::Accepted);
    }
    EXPECT_EQ(queue.enqueue_annotation(annotation_task(), identity("overflow")).status,
              AnnotationSubmissionStatus::Capacity);
    auto missing = identity("overflow");
    missing.reconcile_only = true;
    EXPECT_EQ(queue.enqueue_annotation(annotation_task(), missing).status,
              AnnotationSubmissionStatus::NotAccepted);
    EXPECT_EQ(queue.enqueue_annotation(annotation_task(), identity("0")).status,
              AnnotationSubmissionStatus::Duplicate);
    EXPECT_TRUE(queue.enqueue(annotation_task("ordinary")));
    EXPECT_TRUE(queue.enqueue(annotation_task("ordinary")));
    queue.request_shutdown();
    const auto pending = queue.take_pending();
    EXPECT_EQ(pending.ordinary.size(), AgentTaskQueue::kMaxAnnotationBatches + 2);
}

TEST(AnnotationSubmission, ReconciliationCannotAdmitOrConsumeAnUnknownIdentity) {
    std::atomic<bool> busy{false};
    AgentTaskQueue queue(busy);
    auto lookup = identity();
    lookup.reconcile_only = true;
    const auto missing = queue.enqueue_annotation(annotation_task(), lookup);
    EXPECT_EQ(missing.status, AnnotationSubmissionStatus::NotAccepted);
    EXPECT_FALSE(missing.accepted());
    EXPECT_TRUE(missing.batch_id.empty());
    EXPECT_FALSE(queue.has_user_work());
    EXPECT_EQ(queue.enqueue_annotation(annotation_task(), identity()).status,
              AnnotationSubmissionStatus::Accepted);
    EXPECT_EQ(queue.enqueue_annotation(annotation_task(), lookup).status,
              AnnotationSubmissionStatus::Duplicate);
    lookup.payload_hash = std::string(64, 'b');
    EXPECT_EQ(queue.enqueue_annotation(annotation_task("different"), lookup).status,
              AnnotationSubmissionStatus::Conflict);
    queue.request_shutdown();
    EXPECT_EQ(queue.take_pending().ordinary.size(), 1u);
}

TEST(AnnotationSubmission, ConcurrentReconciliationAndAdmissionQueueExactlyOneTask) {
    std::atomic<bool> busy{false};
    AgentTaskQueue queue(busy);
    std::promise<void> start;
    const auto ready = start.get_future().share();
    std::vector<std::future<acecode::AnnotationSubmissionReceipt>> calls;
    for (int index = 0; index < 24; ++index) {
        calls.push_back(std::async(std::launch::async, [queue_ptr = &queue, ready, index] {
            ready.wait();
            auto request = identity();
            request.reconcile_only = index % 2 == 0;
            return queue_ptr->enqueue_annotation(annotation_task(), request);
        }));
    }
    start.set_value();
    int accepted = 0;
    for (auto& call : calls) {
        const auto receipt = call.get();
        if (receipt.status == AnnotationSubmissionStatus::Accepted) ++accepted;
        EXPECT_TRUE(receipt.accepted() || receipt.status == AnnotationSubmissionStatus::NotAccepted);
        EXPECT_EQ(receipt.batch_id, receipt.accepted() ? "batch-1" : "");
    }
    EXPECT_EQ(accepted, 1);
    queue.request_shutdown();
    EXPECT_EQ(queue.take_pending().ordinary.size(), 1u);
}
