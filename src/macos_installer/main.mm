#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include "desktop/user_install_policy.hpp"

#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;

NSString* const kAcecodeBundleIdentifier = @"dev.acecode.desktop";

void show_alert(NSAlertStyle style, NSString* message, NSString* detail) {
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setAlertStyle:style];
    [alert setMessageText:message];
    [alert setInformativeText:detail ?: @""];
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
}

void show_error(NSString* detail) {
    show_alert(NSAlertStyleCritical,
               @"ACECode could not be installed / 无法安装 ACECode",
               detail);
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
                   NSError** error) {
    id resource_value = nil;
    if (![url getResourceValue:&resource_value forKey:key error:error]) {
        return false;
    }
    if (![resource_value isKindOfClass:[NSNumber class]]) {
        if (error) {
            *error = [NSError errorWithDomain:@"dev.acecode.installer"
                                         code:1
                                     userInfo:@{
                                         NSLocalizedDescriptionKey:
                                             @"Unexpected filesystem metadata."
                                     }];
        }
        return false;
    }
    *value = [(NSNumber*)resource_value boolValue] == YES;
    return true;
}

NSString* error_detail(NSString* message, NSError* error) {
    if (!error) return message;
    return [NSString stringWithFormat:@"%@\n\n%@", message,
                                      [error localizedDescription]];
}

bool bundle_identifier_matches(NSURL* bundle_url) {
    NSBundle* bundle = [NSBundle bundleWithURL:bundle_url];
    return bundle && [[bundle bundleIdentifier]
        isEqualToString:kAcecodeBundleIdentifier];
}

} // namespace

int main() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp activateIgnoringOtherApps:YES];

        NSFileManager* file_manager = [NSFileManager defaultManager];
        NSURL* installer_url = [[NSBundle mainBundle] bundleURL];
        NSURL* image_root = [installer_url URLByDeletingLastPathComponent];
        NSURL* source_url = [image_root URLByAppendingPathComponent:@"ACECode.app"
                                                        isDirectory:YES];

        BOOL source_is_directory = NO;
        if (![file_manager fileExistsAtPath:[source_url path]
                                isDirectory:&source_is_directory] ||
            source_is_directory == NO) {
            show_error(@"ACECode.app is missing beside the installer. Reopen the original DMG.\n\n安装器旁缺少 ACECode.app，请重新打开原始 DMG。");
            return 1;
        }

        NSError* error = nil;
        bool source_is_symlink = false;
        if (!resource_flag(source_url, NSURLIsSymbolicLinkKey,
                           &source_is_symlink, &error) || source_is_symlink) {
            show_error(error_detail(
                @"The ACECode payload is not a safe application bundle.\n\nACECode 安装文件不是安全的应用包。", error));
            return 1;
        }
        if (!bundle_identifier_matches(source_url)) {
            show_error(@"The ACECode payload has an unexpected bundle identifier.\n\nACECode 安装文件的 Bundle ID 不正确。");
            return 1;
        }

        NSArray<NSRunningApplication*>* running =
            [NSRunningApplication runningApplicationsWithBundleIdentifier:
                kAcecodeBundleIdentifier];
        if ([running count] != 0) {
            show_error(@"Quit ACECode, then open this installer again.\n\n请先退出 ACECode，然后重新打开安装器。");
            return 1;
        }

        const char* home_bytes = [NSHomeDirectory() fileSystemRepresentation];
        if (!home_bytes) {
            show_error(@"The current user's home directory is unavailable.\n\n无法读取当前用户的主目录。");
            return 1;
        }
        const auto paths = acecode::desktop::macos_user_install_paths(
            fs::path(home_bytes));
        if (paths.home.empty()) {
            show_error(@"The current user's home directory is invalid.\n\n当前用户的主目录无效。");
            return 1;
        }

        NSURL* home_url = file_url(paths.home, YES);
        NSURL* applications_url = file_url(paths.applications, YES);
        NSURL* destination_url = file_url(paths.destination, YES);
        if (!home_url || !applications_url || !destination_url) {
            show_error(@"The user installation path could not be represented safely.\n\n无法安全表示用户安装路径。");
            return 1;
        }

        BOOL applications_is_directory = NO;
        const BOOL applications_exists =
            [file_manager fileExistsAtPath:[applications_url path]
                               isDirectory:&applications_is_directory];
        if (applications_exists) {
            bool applications_is_symlink = false;
            error = nil;
            if (!resource_flag(applications_url, NSURLIsSymbolicLinkKey,
                               &applications_is_symlink, &error) ||
                applications_is_symlink || applications_is_directory == NO) {
                show_error(error_detail(
                    @"~/Applications must be a real directory inside your home folder.\n\n~/Applications 必须是主目录中的真实文件夹，不能是符号链接。",
                    error));
                return 1;
            }
        } else {
            error = nil;
            if (![file_manager createDirectoryAtURL:applications_url
                         withIntermediateDirectories:YES
                                          attributes:nil
                                               error:&error]) {
                show_error(error_detail(
                    @"The installer could not create ~/Applications.\n\n安装器无法创建 ~/Applications。", error));
                return 1;
            }
        }

        NSURL* resolved_home = [home_url URLByResolvingSymlinksInPath];
        NSURL* resolved_applications =
            [applications_url URLByResolvingSymlinksInPath];
        NSURL* resolved_destination =
            [resolved_applications URLByAppendingPathComponent:@"ACECode.app"
                                                   isDirectory:YES];
        if (!acecode::desktop::macos_user_install_destination_is_safe(
                filesystem_path(resolved_home),
                filesystem_path(resolved_applications),
                filesystem_path(resolved_destination))) {
            show_error(@"The resolved installation path is outside ~/Applications.\n\n解析后的安装路径不在 ~/Applications 中，安装已停止。");
            return 1;
        }
        destination_url = resolved_destination;

        BOOL destination_is_directory = NO;
        const BOOL destination_exists =
            [file_manager fileExistsAtPath:[destination_url path]
                               isDirectory:&destination_is_directory];
        if (destination_exists) {
            bool destination_is_symlink = false;
            error = nil;
            if (!resource_flag(destination_url, NSURLIsSymbolicLinkKey,
                               &destination_is_symlink, &error) ||
                destination_is_symlink || destination_is_directory == NO) {
                show_error(error_detail(
                    @"The existing ~/Applications/ACECode.app is not a replaceable application bundle.\n\n现有的 ~/Applications/ACECode.app 不是可安全替换的应用包。",
                    error));
                return 1;
            }
        }

        NSString* temporary_name = [NSString stringWithFormat:
            @".ACECode-%@.installing.app", [[NSUUID UUID] UUIDString]];
        NSURL* temporary_url =
            [resolved_applications URLByAppendingPathComponent:temporary_name
                                                   isDirectory:YES];
        error = nil;
        if (![file_manager copyItemAtURL:source_url
                                   toURL:temporary_url
                                   error:&error]) {
            show_error(error_detail(
                @"The installer could not copy ACECode into ~/Applications.\n\n安装器无法将 ACECode 复制到 ~/Applications。", error));
            return 1;
        }

        bool installed = false;
        if (destination_exists) {
            NSURL* resulting_url = nil;
            error = nil;
            installed = [file_manager replaceItemAtURL:destination_url
                                         withItemAtURL:temporary_url
                                        backupItemName:nil
                                               options:NSFileManagerItemReplacementUsingNewMetadataOnly
                                      resultingItemURL:&resulting_url
                                                 error:&error] == YES;
        } else {
            error = nil;
            installed = [file_manager moveItemAtURL:temporary_url
                                              toURL:destination_url
                                              error:&error] == YES;
        }

        if (!installed) {
            NSError* cleanup_error = nil;
            [file_manager removeItemAtURL:temporary_url error:&cleanup_error];
            show_error(error_detail(
                @"The installer could not finish replacing ACECode.\n\n安装器无法完成 ACECode 的替换。", error));
            return 1;
        }

        if (!bundle_identifier_matches(destination_url)) {
            show_error(@"The installed application did not pass the final bundle check.\n\n安装后的应用未通过最终 Bundle 检查。");
            return 1;
        }

        show_alert(NSAlertStyleInformational,
                   @"ACECode installed / ACECode 已安装",
                   @"Installed for the current user at ~/Applications/ACECode.app. No administrator access was requested.\n\n已为当前用户安装到 ~/Applications/ACECode.app，全程未请求管理员权限。ACECode 即将启动。");

        if (![[NSWorkspace sharedWorkspace] openURL:destination_url]) {
            show_error(@"ACECode was installed, but macOS could not open it automatically. Open ~/Applications/ACECode.app manually.\n\nACECode 已安装，但 macOS 无法自动启动。请手动打开 ~/Applications/ACECode.app。");
            return 1;
        }
        return 0;
    }
}
