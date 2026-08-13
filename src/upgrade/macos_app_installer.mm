#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include "macos_app_installer.hpp"

#include "desktop/user_install_policy.hpp"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace acecode::upgrade {
namespace {

constexpr const char* kBundleIdentifier = "dev.acecode.desktop";
constexpr const char* kPreviousBundleName = ".ACECode.previous.app";
constexpr const char* kUpdateLockName = ".ACECode.update.lock";

void set_error(std::string* error, const std::string& message) {
    if (error) *error = message;
}

std::string cf_string_utf8(CFStringRef value) {
    if (!value) return {};
    const CFIndex length = CFStringGetLength(value);
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(
        length, kCFStringEncodingUTF8) + 1;
    if (capacity <= 1) return {};
    std::vector<char> buffer(static_cast<size_t>(capacity), '\0');
    if (!CFStringGetCString(value, buffer.data(), capacity,
                            kCFStringEncodingUTF8)) {
        return {};
    }
    return buffer.data();
}

std::string cf_error_text(CFErrorRef error) {
    if (!error) return "unknown Security.framework error";
    CFStringRef description = CFErrorCopyDescription(error);
    const std::string text = cf_string_utf8(description);
    if (description) CFRelease(description);
    return text.empty() ? "unknown Security.framework error" : text;
}

std::string ns_error_text(NSError* error) {
    if (!error) return "unknown filesystem error";
    const char* value = [[error localizedDescription] UTF8String];
    return value ? std::string(value) : std::string("unknown filesystem error");
}

NSURL* file_url(const fs::path& path, BOOL is_directory) {
    const std::string bytes = path.string();
    NSString* value = [[NSFileManager defaultManager]
        stringWithFileSystemRepresentation:bytes.c_str()
                                   length:bytes.size()];
    if (!value) return nil;
    return [NSURL fileURLWithPath:value isDirectory:is_directory];
}

fs::path filesystem_path(NSURL* url) {
    if (!url) return {};
    const char* value = [[url path] fileSystemRepresentation];
    return value ? fs::path(value) : fs::path{};
}

bool resource_flag(NSURL* url,
                   NSURLResourceKey key,
                   bool* value,
                   std::string* error) {
    NSError* native_error = nil;
    id resource_value = nil;
    if (![url getResourceValue:&resource_value forKey:key error:&native_error]) {
        set_error(error, ns_error_text(native_error));
        return false;
    }
    if (![resource_value isKindOfClass:[NSNumber class]]) {
        set_error(error, "unexpected macOS filesystem metadata");
        return false;
    }
    *value = [(NSNumber*)resource_value boolValue] == YES;
    return true;
}

bool validate_safe_install_path(const fs::path& installed_bundle,
                                std::string* error) {
    const char* home_bytes = [NSHomeDirectory() fileSystemRepresentation];
    if (!home_bytes) {
        set_error(error, "current user's home directory is unavailable");
        return false;
    }
    const auto user_paths =
        desktop::macos_user_install_paths(fs::path(home_bytes));
    const auto system_paths = desktop::macos_system_install_paths();
    const fs::path normalized_bundle = installed_bundle.lexically_normal();

    fs::path applications_path;
    desktop::MacosInstallLocation expected_location =
        desktop::MacosInstallLocation::unsupported;
    if (!user_paths.home.empty() &&
        normalized_bundle == user_paths.destination) {
        applications_path = user_paths.applications;
        expected_location =
            desktop::MacosInstallLocation::user_applications;
    } else if (normalized_bundle == system_paths.destination) {
        applications_path = system_paths.applications;
        expected_location =
            desktop::MacosInstallLocation::system_applications;
    } else {
        set_error(error,
                  "macOS self-update requires ~/Applications/ACECode.app or "
                  "/Applications/ACECode.app; "
                  "reinstall ACECode with the signed PKG first");
        return false;
    }

    NSURL* home_url = file_url(user_paths.home, YES);
    NSURL* applications_url = file_url(applications_path, YES);
    NSURL* installed_url = file_url(normalized_bundle, YES);
    if (!home_url || !applications_url || !installed_url) {
        set_error(error, "the macOS installation path cannot be represented safely");
        return false;
    }

    NSFileManager* file_manager = [NSFileManager defaultManager];
    BOOL is_directory = NO;
    if (![file_manager fileExistsAtPath:[applications_url path]
                            isDirectory:&is_directory] || is_directory == NO) {
        set_error(error, applications_path.string() +
                             " is missing or is not a directory");
        return false;
    }
    bool is_symlink = false;
    if (!resource_flag(applications_url, NSURLIsSymbolicLinkKey,
                       &is_symlink, error) || is_symlink) {
        if (is_symlink) {
            set_error(error, applications_path.string() +
                                 " must not be a symbolic link");
        }
        return false;
    }

    is_directory = NO;
    if (![file_manager fileExistsAtPath:[installed_url path]
                            isDirectory:&is_directory] || is_directory == NO) {
        set_error(error, "the installed ACECode.app bundle is missing");
        return false;
    }
    is_symlink = false;
    if (!resource_flag(installed_url, NSURLIsSymbolicLinkKey,
                       &is_symlink, error) || is_symlink) {
        if (is_symlink) {
            set_error(error, "the installed ACECode.app must not be a symbolic link");
        }
        return false;
    }

    NSURL* resolved_home = [home_url URLByResolvingSymlinksInPath];
    NSURL* resolved_applications = [applications_url URLByResolvingSymlinksInPath];
    NSURL* resolved_destination =
        [resolved_applications URLByAppendingPathComponent:@"ACECode.app"
                                               isDirectory:YES];
    const auto resolved_location =
        desktop::macos_self_update_install_location(
            filesystem_path(resolved_home),
            filesystem_path(resolved_applications),
            filesystem_path(resolved_destination));
    if (resolved_location != expected_location ||
        filesystem_path([installed_url URLByResolvingSymlinksInPath]) !=
            filesystem_path(resolved_destination)) {
        set_error(error,
                  "the resolved ACECode.app path is outside the supported installation locations");
        return false;
    }

    const fs::path resolved_applications_path =
        filesystem_path(resolved_applications);
    if (::access(resolved_applications_path.c_str(), W_OK | X_OK) != 0) {
        if (expected_location ==
            desktop::MacosInstallLocation::system_applications) {
            set_error(error,
                      "ACECode cannot modify /Applications. Install the update "
                      "manually with the signed ACECode PKG");
        } else {
            set_error(error,
                      "ACECode cannot modify ~/Applications: " +
                          std::string(std::strerror(errno)));
        }
        return false;
    }
    return true;
}

struct SigningIdentity {
    std::string identifier;
    std::string team_id;
};

bool create_static_code(const fs::path& bundle,
                        SecStaticCodeRef* code,
                        std::string* error) {
    NSURL* url = file_url(bundle, YES);
    if (!url) {
        set_error(error, "application bundle path cannot be represented safely");
        return false;
    }
    const OSStatus status = SecStaticCodeCreateWithPath(
        (__bridge CFURLRef)url, kSecCSDefaultFlags, code);
    if (status != errSecSuccess || !*code) {
        set_error(error, "cannot inspect application code signature (OSStatus " +
                         std::to_string(status) + ")");
        return false;
    }
    return true;
}

constexpr SecCSFlags kSignatureValidationFlags =
    kSecCSStrictValidate | kSecCSCheckAllArchitectures | kSecCSCheckNestedCode;

bool validate_static_code(SecStaticCodeRef code,
                          SecRequirementRef requirement,
                          std::string* error) {
    CFErrorRef native_error = nullptr;
    const OSStatus status = SecStaticCodeCheckValidityWithErrors(
        code, kSignatureValidationFlags, requirement, &native_error);
    if (status != errSecSuccess) {
        const std::string detail = cf_error_text(native_error);
        if (native_error) CFRelease(native_error);
        set_error(error, "application code signature is invalid: " + detail);
        return false;
    }
    if (native_error) CFRelease(native_error);
    return true;
}

bool read_signing_identity(SecStaticCodeRef code,
                           SigningIdentity& identity,
                           std::string* error) {
    CFDictionaryRef information = nullptr;
    const OSStatus status = SecCodeCopySigningInformation(
        code, kSecCSSigningInformation, &information);
    if (status != errSecSuccess || !information) {
        set_error(error, "cannot read application signing identity (OSStatus " +
                         std::to_string(status) + ")");
        return false;
    }

    const auto identifier_value =
        CFDictionaryGetValue(information, kSecCodeInfoIdentifier);
    const auto team_id_value =
        CFDictionaryGetValue(information, kSecCodeInfoTeamIdentifier);
    const CFStringRef identifier = identifier_value &&
            CFGetTypeID(identifier_value) == CFStringGetTypeID()
        ? static_cast<CFStringRef>(identifier_value)
        : nullptr;
    const CFStringRef team_id = team_id_value &&
            CFGetTypeID(team_id_value) == CFStringGetTypeID()
        ? static_cast<CFStringRef>(team_id_value)
        : nullptr;
    identity.identifier = cf_string_utf8(identifier);
    identity.team_id = cf_string_utf8(team_id);
    CFRelease(information);

    if (identity.identifier.empty() || identity.team_id.empty()) {
        set_error(error,
                  "ACECode must have a Developer ID signature with a non-empty Team ID");
        return false;
    }
    return true;
}

bool valid_team_id(const std::string& value) {
    if (value.size() != 10) return false;
    for (unsigned char c : value) {
        if (!std::isalnum(c)) return false;
    }
    return true;
}

bool copy_designated_requirement(SecStaticCodeRef code,
                                 SecRequirementRef* requirement,
                                 std::string* error) {
    const OSStatus status = SecCodeCopyDesignatedRequirement(
        code, kSecCSDefaultFlags, requirement);
    if (status != errSecSuccess || !*requirement) {
        set_error(error,
                  "cannot read the installed ACECode signing requirement "
                  "(OSStatus " + std::to_string(status) + ")");
        return false;
    }
    return true;
}

bool read_bundle_metadata(const fs::path& bundle,
                          std::string& identifier,
                          std::string& version,
                          std::string* error) {
    NSURL* info_url = file_url(bundle / "Contents" / "Info.plist", NO);
    NSDictionary* info = info_url
        ? [NSDictionary dictionaryWithContentsOfURL:info_url]
        : nil;
    if (![info isKindOfClass:[NSDictionary class]]) {
        set_error(error, "ACECode.app has an unreadable Info.plist");
        return false;
    }
    id identifier_value = [info objectForKey:@"CFBundleIdentifier"];
    id version_value = [info objectForKey:@"CFBundleShortVersionString"];
    if (![identifier_value isKindOfClass:[NSString class]] ||
        ![version_value isKindOfClass:[NSString class]]) {
        set_error(error, "ACECode.app is missing bundle identifier or version metadata");
        return false;
    }
    identifier = [(NSString*)identifier_value UTF8String] ?: "";
    version = [(NSString*)version_value UTF8String] ?: "";
    return true;
}

bool authenticate_candidate(const fs::path& reference_bundle,
                            const fs::path& candidate_bundle,
                            const std::string& expected_version,
                            std::string* error) {
    std::error_code ec;
    const fs::file_status candidate_status = fs::symlink_status(candidate_bundle, ec);
    if (ec || !fs::is_directory(candidate_status)) {
        set_error(error, "the staged ACECode.app is missing or is a symbolic link");
        return false;
    }

    SecStaticCodeRef reference_code = nullptr;
    if (!create_static_code(reference_bundle, &reference_code, error)) return false;
    if (!validate_static_code(reference_code, nullptr, error)) {
        CFRelease(reference_code);
        return false;
    }
    SigningIdentity reference_identity;
    if (!read_signing_identity(reference_code, reference_identity, error)) {
        CFRelease(reference_code);
        return false;
    }
    if (reference_identity.identifier != kBundleIdentifier) {
        CFRelease(reference_code);
        set_error(error, "installed application has an unexpected bundle identifier");
        return false;
    }
    if (!valid_team_id(reference_identity.team_id)) {
        CFRelease(reference_code);
        set_error(error, "installed ACECode has an invalid Developer Team ID");
        return false;
    }

    SecRequirementRef requirement = nullptr;
    if (!copy_designated_requirement(reference_code, &requirement, error)) {
        CFRelease(reference_code);
        return false;
    }
    if (!validate_static_code(reference_code, requirement, error)) {
        CFRelease(requirement);
        CFRelease(reference_code);
        return false;
    }
    CFRelease(reference_code);

    SecStaticCodeRef candidate_code = nullptr;
    if (!create_static_code(candidate_bundle, &candidate_code, error)) {
        CFRelease(requirement);
        return false;
    }
    if (!validate_static_code(candidate_code, requirement, error)) {
        CFRelease(candidate_code);
        CFRelease(requirement);
        return false;
    }
    SigningIdentity candidate_identity;
    if (!read_signing_identity(candidate_code, candidate_identity, error)) {
        CFRelease(candidate_code);
        CFRelease(requirement);
        return false;
    }
    CFRelease(candidate_code);
    CFRelease(requirement);

    if (candidate_identity.identifier != kBundleIdentifier ||
        candidate_identity.team_id != reference_identity.team_id) {
        set_error(error, "candidate ACECode.app is signed by an unexpected identity");
        return false;
    }

    std::string metadata_identifier;
    std::string metadata_version;
    if (!read_bundle_metadata(candidate_bundle, metadata_identifier,
                              metadata_version, error)) {
        return false;
    }
    if (metadata_identifier != kBundleIdentifier) {
        set_error(error, "candidate ACECode.app has an unexpected bundle identifier");
        return false;
    }
    if (metadata_version != expected_version) {
        set_error(error, "candidate ACECode.app version " + metadata_version +
                         " does not match manifest version " + expected_version);
        return false;
    }
    if (::access((candidate_bundle / "Contents" / "MacOS" / "ACECode").c_str(),
                 X_OK) != 0 ||
        ::access((candidate_bundle / "Contents" / "MacOS" / "acecode-daemon").c_str(),
                 X_OK) != 0) {
        set_error(error, "candidate ACECode.app contains a non-executable binary");
        return false;
    }
    return true;
}

class UpdateLock {
public:
    ~UpdateLock() {
        if (fd_ >= 0) {
            (void)::flock(fd_, LOCK_UN);
            (void)::close(fd_);
        }
    }

    bool acquire(const fs::path& path, std::string* error) {
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd_ < 0) {
            set_error(error, "cannot open the macOS update lock: " +
                             std::string(std::strerror(errno)));
            return false;
        }
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            set_error(error, errno == EWOULDBLOCK
                ? "another macOS update is already installing"
                : "cannot acquire the macOS update lock: " +
                    std::string(std::strerror(errno)));
            (void)::close(fd_);
            fd_ = -1;
            return false;
        }
        return true;
    }

private:
    int fd_ = -1;
};

bool remove_if_present(NSFileManager* file_manager,
                       NSURL* url,
                       std::string* error) {
    if (![file_manager fileExistsAtPath:[url path]]) return true;
    NSError* native_error = nil;
    if (![file_manager removeItemAtURL:url error:&native_error]) {
        set_error(error, "cannot remove " + filesystem_path(url).string() + ": " +
                         ns_error_text(native_error));
        return false;
    }
    return true;
}

bool move_item(NSFileManager* file_manager,
               NSURL* source,
               NSURL* destination,
               std::string* error) {
    NSError* native_error = nil;
    if (![file_manager moveItemAtURL:source toURL:destination error:&native_error]) {
        set_error(error, "cannot move " + filesystem_path(source).string() + " to " +
                         filesystem_path(destination).string() + ": " +
                         ns_error_text(native_error));
        return false;
    }
    return true;
}

} // namespace

bool preflight_macos_app_update(const fs::path& installed_bundle,
                                const fs::path& candidate_bundle,
                                const std::string& expected_version,
                                std::string* error) {
    @autoreleasepool {
        if (!validate_safe_install_path(installed_bundle, error)) return false;
        return authenticate_candidate(installed_bundle, candidate_bundle,
                                      expected_version, error);
    }
}

bool install_macos_app_update(const fs::path& installed_bundle,
                              const fs::path& candidate_bundle,
                              const std::string& expected_version,
                              fs::path* backup_bundle,
                              std::string* error) {
    @autoreleasepool {
        if (backup_bundle) backup_bundle->clear();
        if (!preflight_macos_app_update(installed_bundle, candidate_bundle,
                                        expected_version, error)) {
            return false;
        }

        const fs::path applications_dir = installed_bundle.parent_path();
        UpdateLock lock;
        if (!lock.acquire(applications_dir / kUpdateLockName, error)) return false;

        NSFileManager* file_manager = [NSFileManager defaultManager];
        NSURL* installed_url = file_url(installed_bundle, YES);
        NSURL* candidate_url = file_url(candidate_bundle, YES);
        const fs::path temporary_path = applications_dir /
            (".ACECode-" + std::string([[[NSUUID UUID] UUIDString] UTF8String]) +
             ".updating.app");
        const fs::path previous_path = applications_dir / kPreviousBundleName;
        NSURL* temporary_url = file_url(temporary_path, YES);
        NSURL* previous_url = file_url(previous_path, YES);
        if (!installed_url || !candidate_url || !temporary_url || !previous_url) {
            set_error(error, "macOS update paths cannot be represented safely");
            return false;
        }

        NSError* native_error = nil;
        if (![file_manager copyItemAtURL:candidate_url
                                   toURL:temporary_url
                                   error:&native_error]) {
            std::string ignored;
            remove_if_present(file_manager, temporary_url, &ignored);
            set_error(error, "cannot copy the staged ACECode.app beside the installation: " +
                             ns_error_text(native_error));
            return false;
        }
        if (!authenticate_candidate(installed_bundle, temporary_path,
                                    expected_version, error)) {
            std::string ignored;
            remove_if_present(file_manager, temporary_url, &ignored);
            return false;
        }

        if (!remove_if_present(file_manager, previous_url, error)) {
            std::string ignored;
            remove_if_present(file_manager, temporary_url, &ignored);
            return false;
        }
        if (!move_item(file_manager, installed_url, previous_url, error)) {
            std::string ignored;
            remove_if_present(file_manager, temporary_url, &ignored);
            return false;
        }

        std::string move_error;
        if (!move_item(file_manager, temporary_url, installed_url, &move_error)) {
            std::string rollback_error;
            const bool rolled_back = move_item(
                file_manager, previous_url, installed_url, &rollback_error);
            std::string ignored;
            remove_if_present(file_manager, temporary_url, &ignored);
            set_error(error, move_error + (rolled_back
                ? "; previous ACECode.app was restored"
                : "; rollback also failed: " + rollback_error));
            return false;
        }

        std::string final_error;
        if (!validate_safe_install_path(installed_bundle, &final_error) ||
            !authenticate_candidate(previous_path, installed_bundle,
                                    expected_version, &final_error)) {
            std::string cleanup_error;
            const fs::path failed_path = applications_dir /
                (".ACECode-" + std::string([[[NSUUID UUID] UUIDString] UTF8String]) +
                 ".failed.app");
            NSURL* failed_url = file_url(failed_path, YES);
            bool moved_failed = failed_url &&
                move_item(file_manager, installed_url, failed_url, &cleanup_error);
            if (!moved_failed) {
                std::string removal_error;
                if (remove_if_present(file_manager, installed_url, &removal_error)) {
                    moved_failed = true;
                } else if (cleanup_error.empty()) {
                    cleanup_error = std::move(removal_error);
                } else {
                    cleanup_error += "; cannot remove invalid replacement: " +
                                     removal_error;
                }
            }
            std::string rollback_error;
            const bool rolled_back = moved_failed &&
                move_item(file_manager, previous_url, installed_url, &rollback_error);
            if (moved_failed) {
                std::string ignored;
                remove_if_present(file_manager, failed_url, &ignored);
            }
            set_error(error, "installed ACECode.app failed final validation: " +
                             final_error + (rolled_back
                    ? "; previous ACECode.app was restored"
                    : "; rollback failed: " +
                        (rollback_error.empty() ? cleanup_error : rollback_error)));
            return false;
        }

        if (backup_bundle) *backup_bundle = previous_path;
        return true;
    }
}

} // namespace acecode::upgrade
