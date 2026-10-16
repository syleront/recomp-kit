// platform_ui_mac.mm - the SDL host's macOS system dialogs. The runtime's
// comdlg32 and USER32 shims call the platform seam (platform/os.h); this file
// is the native presenter the seam installs, so a Save/Open in the game opens
// the real NSSavePanel/NSOpenPanel and a MessageBox is an NSAlert.
//
// The presenter runs on the guest's main thread, which is also the SDL host's
// run thread and therefore the Cocoa main thread: [panel runModal] pumps
// AppKit's own loop while the guest is blocked in the call, exactly as a modal
// dialog blocks the calling Windows thread. A call from any other thread is
// refused (returns -1) so the platform's deterministic fallback answers instead
// of showing AppKit off the main thread, which is undefined.
#import <AppKit/AppKit.h>

#include "platform_ui.h"

#include "platform/os.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// A UTF-8 C string as an NSString, or nil for an empty/NULL one.
NSString *ns(const char *s) {
    return s && *s ? [NSString stringWithUTF8String:s] : nil;
}

// The last path component of a Windows or POSIX path.
const char *basename_of(const char *path) {
    const char *base = path;
    for (const char *p = path; p && *p; ++p)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}

// Extensions named by a Win32 filter pattern ("*.pet;*.PET"), as an NSArray of
// bare extensions. "*.*"/"*" adds nothing (all files are allowed).
NSArray<NSString *> *allowed_types(const OsFileDialog *d) {
    if (!d->filters || d->filter_count <= 0)
        return nil;
    NSMutableOrderedSet<NSString *> *set = [NSMutableOrderedSet orderedSet];
    for (int i = 0; i < d->filter_count; ++i) {
        if (d->filter_index > 0 && d->filter_index != i + 1)
            continue;
        const char *patterns = d->filters[i].patterns;
        if (!patterns)
            continue;
        std::string list(patterns);
        size_t at = 0;
        while (at <= list.size()) {
            size_t sep = list.find(';', at);
            std::string token = list.substr(at, sep == std::string::npos ? sep : sep - at);
            at = sep == std::string::npos ? list.size() + 1 : sep + 1;
            if (token.rfind("*.", 0) == 0 && token.size() > 2)
                token = token.substr(2);
            else if (token == "*" || token == "*.*" || token.empty())
                continue;
            [set addObject:[NSString stringWithUTF8String:token.c_str()]];
        }
    }
    return set.count ? set.array : nil;
}

extern "C" int mac_file_dialog(OsFileDialog *d) {
    if (![NSThread isMainThread]) {
        fprintf(stderr, "[dialogs] file dialog off the main thread; deferring\n");
        return -1;
    }
    @autoreleasepool {
        NSSavePanel *panel = d->save ? [NSSavePanel savePanel] : [NSOpenPanel openPanel];
        if (!d->save) {
            NSOpenPanel *open = (NSOpenPanel *)panel;
            open.canChooseFiles = YES;
            open.canChooseDirectories = NO;
            open.allowsMultipleSelection = NO;
        } else {
            panel.canCreateDirectories = YES;
        }
        if (NSString *title = ns(d->title))
            panel.title = title;
        // A guest path is a Windows path; only a POSIX one can name a host
        // directory. The initial name is prefilled either way.
        if (d->initial_dir && d->initial_dir[0] == '/')
            panel.directoryURL =
                [NSURL fileURLWithPath:[NSString stringWithUTF8String:d->initial_dir]];
        if (d->file && *d->file)
            panel.nameFieldStringValue = [NSString stringWithUTF8String:basename_of(d->file)];
        // allowedFileTypes is deprecated on macOS 12+ but is the one call that
        // works across the deployment range without UniformTypeIdentifiers.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (NSArray<NSString *> *types = allowed_types(d))
            panel.allowedFileTypes = types;
#pragma clang diagnostic pop

        NSModalResponse response = [panel runModal];
        if (response != NSModalResponseOK || !panel.URL)
            return 0; // cancelled
        const char *path = panel.URL.path.UTF8String;
        if (!path)
            return 0;
        return os_file_dialog_set_result(d, path);
    }
}

// A Win32 icon code (OsMessageBox::icon) as an NSAlert style.
NSAlertStyle alert_style(int icon) {
    switch (icon) {
    case 2:
        return NSAlertStyleWarning;
    case 3:
        return NSAlertStyleCritical;
    default:
        return NSAlertStyleInformational;
    }
}

extern "C" int mac_message_box(const OsMessageBox *m) {
    if (![NSThread isMainThread]) {
        fprintf(stderr, "[dialogs] message box off the main thread; deferring\n");
        return -1;
    }
    @autoreleasepool {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.alertStyle = alert_style(m->icon);
        if (m->title && *m->title) {
            alert.messageText = [NSString stringWithUTF8String:m->title];
            alert.informativeText = ns(m->text) ?: @"";
        } else {
            alert.messageText = ns(m->text) ?: @"";
        }
        // NSAlert returns NSAlertFirstButtonReturn + the button's add order.
        // Add the default button first (it is the rightmost, as a default
        // button is on macOS) and keep the add order to map the answer back.
        int order[8];
        int added = 0;
        int count = m->count > 8 ? 8 : m->count;
        int def = m->default_index >= 0 && m->default_index < count ? m->default_index : 0;
        order[added++] = def;
        for (int i = 0; i < count; ++i)
            if (i != def)
                order[added++] = i;
        for (int i = 0; i < added; ++i)
            [alert addButtonWithTitle:ns(m->labels[order[i]]) ?: @""];
        NSModalResponse response = [alert runModal];
        int chosen = (int)(response - NSAlertFirstButtonReturn);
        if (chosen < 0 || chosen >= added)
            return def;
        return order[chosen];
    }
}

} // namespace

void platform_ui_install_dialogs() {
    os_set_file_dialog_presenter(mac_file_dialog);
    os_set_message_box_presenter(mac_message_box);
}
