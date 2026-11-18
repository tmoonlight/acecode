#pragma once

#include <string>

namespace acecode {

struct AnnotationSubmissionIdentity {
    std::string batch_id;
    std::string payload_hash;
};

enum class AnnotationSubmissionStatus {
    Accepted, Duplicate, Conflict, Capacity, Unavailable, Invalid
};

struct AnnotationSubmissionReceipt {
    AnnotationSubmissionStatus status = AnnotationSubmissionStatus::Unavailable;
    std::string batch_id;

    bool accepted() const {
        return status == AnnotationSubmissionStatus::Accepted ||
               status == AnnotationSubmissionStatus::Duplicate;
    }
};

} // namespace acecode
