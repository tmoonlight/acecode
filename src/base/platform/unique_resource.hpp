#pragma once

#include <utility>

namespace acecode::platform {

// Traits supplies handle_type, invalid(), valid(handle), and close(handle).
// Only get() borrows; release() explicitly transfers ownership.
template <class Traits>
class UniqueResource {
public:
    using handle_type = typename Traits::handle_type;
    UniqueResource() noexcept = default;
    explicit UniqueResource(handle_type handle) noexcept : handle_(handle) {}
    ~UniqueResource() { reset(); }
    UniqueResource(const UniqueResource&) = delete;
    UniqueResource& operator=(const UniqueResource&) = delete;
    UniqueResource(UniqueResource&& other) noexcept : handle_(other.release()) {}
    UniqueResource& operator=(UniqueResource&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    handle_type get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return Traits::valid(handle_); }
    handle_type release() noexcept { return std::exchange(handle_, Traits::invalid()); }
    void reset(handle_type handle = Traits::invalid()) noexcept {
        if (handle_ == handle) return;
        const auto previous = std::exchange(handle_, handle);
        if (Traits::valid(previous)) Traits::close(previous);
    }
    // For native APIs that fill an owned handle. Releases any previous resource first.
    handle_type* put() noexcept { reset(); return &handle_; }

private:
    handle_type handle_ = Traits::invalid();
};

} // namespace acecode::platform
