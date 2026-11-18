#include "desktop/page_annotation_session.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "web/annotation_submission_request.hpp"

#include <atomic>
#include <iostream>
#include <string>

int main() {
    using nlohmann::json;
    acecode::desktop::PageAnnotationSession session;
    const std::atomic<bool> busy{true};
    acecode::agent::AgentTaskQueue queue(busy);
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            const auto request = json::parse(line);
            const auto operation = request.at("operation").get<std::string>();
            json response;
            std::string expression;
            std::string error;
            if (operation == "prepare") {
                response["ok"] = session.prepare(request.value("page_id", "page-integration"),
                    request.at("command"), expression, error);
                response["expression"] = expression;
                response["sequence"] = session.sequence();
            } else if (operation == "complete") {
                response["ok"] = session.complete(request.at("sequence").get<std::uint64_t>(),
                    request.at("result"), error);
            } else if (operation == "submission") {
                response["ok"] = session.submission(request.at("command"), expression, error);
                response["expression"] = expression;
            } else if (operation == "replace") {
                session.replace_document();
                response["ok"] = true;
            } else if (operation == "snapshot") {
                response["ok"] = true;
            } else if (operation == "receive") {
                const auto& payload = request.at("payload");
                acecode::AnnotationSubmissionIdentity identity;
                if (!acecode::web::parse_annotation_submission_identity(payload, true, identity, error)) {
                    response["ok"] = false;
                } else {
                    const auto mode = request.value("reconciliation", std::string{});
                    if (!acecode::web::parse_annotation_reconciliation(
                            mode.empty() ? nullptr : mode.c_str(), identity, error)) {
                        std::cout << json{{"ok", false}, {"status", 400}, {"error", error}}.dump() << std::endl;
                        continue;
                    }
                    acecode::agent::WorkerTask task;
                    task.input.text = payload.at("text").get<std::string>();
                    task.input.metadata = payload;
                    if (identity.batch_id.empty()) {
                        response = {{"ok", queue.enqueue(std::move(task))}, {"queued", true}};
                    } else {
                        const auto receipt = queue.enqueue_annotation(std::move(task), identity);
                        response = {{"ok", receipt.accepted()}, {"queued", receipt.accepted()},
                            {"annotation_batch_id", receipt.batch_id},
                            {"duplicate", receipt.status == acecode::AnnotationSubmissionStatus::Duplicate}};
                        if (!receipt.accepted()) {
                            const bool not_accepted = receipt.status == acecode::AnnotationSubmissionStatus::NotAccepted;
                            const bool conflict = receipt.status == acecode::AnnotationSubmissionStatus::Conflict;
                            response["status"] = not_accepted ? 404 : conflict ? 409 : 503;
                            error = not_accepted ? "annotation_batch_not_accepted" :
                                conflict ? "annotation_batch_conflict" : "annotation_batch_unavailable";
                        }
                    }
                }
            } else if (operation == "drain") {
                queue.request_shutdown();
                auto pending = queue.take_pending();
                response = {{"ok", true}, {"priority_count", pending.priority.size()},
                    {"ordinary", json::array()}};
                for (const auto& task : pending.ordinary) response["ordinary"].push_back(task.input.metadata);
            } else {
                response = {{"ok", false}};
                error = "Unknown test operation";
            }
            response["error"] = error;
            response["annotations"] = session.snapshot();
            std::cout << response.dump() << std::endl;
        } catch (const std::exception& error) {
            std::cout << json{{"ok", false}, {"error", error.what()}}.dump() << std::endl;
        }
    }
    queue.request_shutdown();
}
