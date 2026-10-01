#include "package_identity.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <appmodel.h>
#endif

namespace acecode {

bool has_windows_package_identity() noexcept {
#ifdef _WIN32
    UINT32 length = 0;
    const LONG result = GetCurrentPackageFullName(&length, nullptr);
    return result == ERROR_INSUFFICIENT_BUFFER || result == ERROR_SUCCESS;
#else
    return false;
#endif
}

} // namespace acecode
