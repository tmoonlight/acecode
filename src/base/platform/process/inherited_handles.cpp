#include "inherited_handles.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>

namespace acecode::platform {

InheritedHandleList::InheritedHandleList(std::initializer_list<void*> handles) {
    for (void* handle : handles) {
        if (!handle || handle == INVALID_HANDLE_VALUE) continue;
        if (std::find(handles_.begin(), handles_.end(), handle) == handles_.end()) {
            handles_.push_back(handle);
        }
    }
    if (handles_.empty()) {
        error_code_ = ERROR_INVALID_HANDLE;
        return;
    }

    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (size == 0) {
        error_code_ = ::GetLastError();
        return;
    }
    storage_.resize(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    if (!::InitializeProcThreadAttributeList(list, 1, 0, &size)) {
        error_code_ = ::GetLastError();
        return;
    }
    initialized_ = true;
    // 属性表只记下 handles_ 的地址,不复制内容:handles_ 此后不能再增删。
    if (!::UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                     handles_.data(), handles_.size() * sizeof(HANDLE),
                                     nullptr, nullptr)) {
        error_code_ = ::GetLastError();
        return;
    }
    ready_ = true;
}

InheritedHandleList::~InheritedHandleList() {
    if (initialized_) {
        ::DeleteProcThreadAttributeList(
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data()));
    }
}

_PROC_THREAD_ATTRIBUTE_LIST* InheritedHandleList::attribute_list() noexcept {
    return ready_ ? reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data())
                  : nullptr;
}

} // namespace acecode::platform

#endif
