#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include "desktop/user_install_policy.hpp"

#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;

NSString* const kAcecodeBundleIdentifier = @"dev.acecode.desktop";

void show_alert(NSAlertStyle style, NSString* message, NSString* detail) {
    [NSApp activateIgnoringOtherApps:YES];
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setAlertStyle:style];
    [alert setMessageText:message];
    [alert setInformativeText:detail ?: @""];
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
    [alert release];
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
            *error = [NSError errorWithDomain:@"dev.acecode.user-applications"
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

NSURL* expected_source_url() {
    NSURL* target_url = [[NSBundle mainBundle] bundleURL];
    NSURL* image_root = [target_url URLByDeletingLastPathComponent];
    return [image_root URLByAppendingPathComponent:@"ACECode.app"
                                       isDirectory:YES];
}

bool dropped_source_is_expected(NSURL* requested_url, NSURL* expected_url) {
    if (!requested_url || !expected_url || ![requested_url isFileURL]) {
        return false;
    }

    NSURL* requested_resolved =
        [[requested_url URLByStandardizingPath] URLByResolvingSymlinksInPath];
    NSURL* expected_resolved =
        [[expected_url URLByStandardizingPath] URLByResolvingSymlinksInPath];

    id requested_identifier = nil;
    id expected_identifier = nil;
    NSError* requested_error = nil;
    NSError* expected_error = nil;
    const BOOL requested_has_identifier =
        [requested_resolved getResourceValue:&requested_identifier
                                      forKey:NSURLFileResourceIdentifierKey
                                       error:&requested_error];
    const BOOL expected_has_identifier =
        [expected_resolved getResourceValue:&expected_identifier
                                     forKey:NSURLFileResourceIdentifierKey
                                      error:&expected_error];
    if (requested_has_identifier && expected_has_identifier &&
        requested_identifier && expected_identifier &&
        [requested_identifier isEqual:expected_identifier]) {
        return true;
    }

    return filesystem_path(requested_resolved).lexically_normal() ==
           filesystem_path(expected_resolved).lexically_normal();
}

int install_application(NSURL* source_url) {
    NSFileManager* file_manager = [NSFileManager defaultManager];

    BOOL source_is_directory = NO;
    if (![file_manager fileExistsAtPath:[source_url path]
                            isDirectory:&source_is_directory] ||
        source_is_directory == NO) {
        show_error(@"ACECode.app is missing beside Applications. Reopen the original DMG.\n\nApplications 旁缺少 ACECode.app，请重新打开原始 DMG。");
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
        show_error(@"Quit ACECode, then use Applications again.\n\n请先退出 ACECode，然后重新使用 Applications 安装入口。");
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
                @"Applications could not create ~/Applications.\n\n无法创建 ~/Applications。", error));
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
            @"Applications could not copy ACECode into ~/Applications.\n\n无法将 ACECode 复制到 ~/Applications。", error));
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
            @"Applications could not finish replacing ACECode.\n\n无法完成 ACECode 的替换。", error));
        return 1;
    }

    if (!bundle_identifier_matches(destination_url)) {
        show_error(@"The installed application did not pass the final bundle check.\n\n安装后的应用未通过最终 Bundle 检查。");
        return 1;
    }

    if (![[NSWorkspace sharedWorkspace] openURL:destination_url]) {
        show_error(@"ACECode was installed, but macOS could not open it automatically. Open ~/Applications/ACECode.app manually.\n\nACECode 已安装，但 macOS 无法自动启动。请手动打开 ~/Applications/ACECode.app。");
        return 1;
    }
    return 0;
}

} // namespace

@interface ACECodeUserApplicationsDelegate : NSObject <NSApplicationDelegate> {
@private
    NSURL* requested_source_url_;
    BOOL launch_finished_;
    BOOL installation_started_;
    BOOL invalid_drop_;
    int exit_code_;
}

- (int)exitCode;
- (void)acceptOpenURLs:(NSArray<NSURL*>*)urls;
- (void)beginInstallation;

@end

@implementation ACECodeUserApplicationsDelegate

- (instancetype)init {
    self = [super init];
    if (self) {
        requested_source_url_ = nil;
        launch_finished_ = NO;
        installation_started_ = NO;
        invalid_drop_ = NO;
        exit_code_ = 1;
    }
    return self;
}

- (void)dealloc {
    [requested_source_url_ release];
    [super dealloc];
}

- (int)exitCode {
    return exit_code_;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    launch_finished_ = YES;
    if (requested_source_url_ || invalid_drop_) {
        [self beginInstallation];
        return;
    }

    show_alert(NSAlertStyleInformational,
               @"Drag ACECode to Applications / 将 ACECode 拖到 Applications",
               @"Drag the ACECode icon on the left onto this Applications icon. Installation to ~/Applications starts when you release it.\n\n请把左侧 ACECode 图标拖到这个 Applications 图标上，松手后会自动安装到 ~/Applications。");
    exit_code_ = 0;
    [NSApp stop:self];
}

- (BOOL)application:(NSApplication*)application
            openFile:(NSString*)filename {
    (void)application;
    if (!filename) return NO;
    [self acceptOpenURLs:@[
        [NSURL fileURLWithPath:filename isDirectory:YES]
    ]];
    return YES;
}

- (void)application:(NSApplication*)application
            openURLs:(NSArray<NSURL*>*)urls {
    (void)application;
    [self acceptOpenURLs:urls];
}

- (void)application:(NSApplication*)application
           openFiles:(NSArray<NSString*>*)filenames {
    NSMutableArray<NSURL*>* urls =
        [NSMutableArray arrayWithCapacity:[filenames count]];
    for (NSString* filename in filenames) {
        [urls addObject:[NSURL fileURLWithPath:filename isDirectory:YES]];
    }
    [self acceptOpenURLs:urls];
    [application replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

- (void)acceptOpenURLs:(NSArray<NSURL*>*)urls {
    if (installation_started_ || requested_source_url_ || invalid_drop_) {
        return;
    }

    if ([urls count] != 1) {
        invalid_drop_ = YES;
        if (launch_finished_) [self beginInstallation];
        return;
    }

    requested_source_url_ = [[urls objectAtIndex:0] retain];
    if (launch_finished_) [self beginInstallation];
}

- (void)beginInstallation {
    if (installation_started_) return;
    installation_started_ = YES;

    NSURL* source_url = expected_source_url();
    if (invalid_drop_ || !requested_source_url_ ||
        !dropped_source_is_expected(requested_source_url_, source_url)) {
        show_error(@"Drag the ACECode.app from beside Applications. Other applications are not accepted.\n\n请拖入 Applications 旁边的 ACECode.app，不能安装其他应用。");
        exit_code_ = 1;
    } else {
        exit_code_ = install_application(source_url);
    }

    [NSApp stop:self];
}

@end

int main() {
    int result = 1;
    @autoreleasepool {
        NSApplication* application = [NSApplication sharedApplication];
        ACECodeUserApplicationsDelegate* delegate =
            [[ACECodeUserApplicationsDelegate alloc] init];
        [application setDelegate:delegate];
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        [application run];
        result = [delegate exitCode];
        [application setDelegate:nil];
        [delegate release];
    }
    return result;
}
