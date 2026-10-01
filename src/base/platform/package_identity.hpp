#pragma once

namespace acecode {

// True for a Windows process running with MSIX/AppX package identity.
// The package deployment service owns its installation directory.
bool has_windows_package_identity() noexcept;

} // namespace acecode
