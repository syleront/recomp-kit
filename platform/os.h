// os.h - the platform layer: what the runtime, the DirectX shims, the mod
// foundation and the shared host code need from the operating system. One
// POSIX implementation (macOS, Linux) and one Win32 implementation; nothing
// above this header includes a platform header, nothing here knows the guest.
//
// Every function is plain C with C linkage so C plugins and C++ hosts share
// it. Errors are reported the C way: -1 or NULL, errno where POSIX sets it.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Threads. A created thread is an opaque handle; identity is a 64-bit id that
// compares with ==, because the scheduler asks "is the caller this thread"
// far more often than it joins anything.
// ---------------------------------------------------------------------------
typedef struct OsThread OsThread;
typedef uint64_t OsThreadId;
// `stack_bytes` 0 means the platform default. NULL when the thread could not
// be created; nothing has started in that case.
OsThread *os_thread_create(void *(*fn)(void *), void *arg, size_t stack_bytes);
void os_thread_join(OsThread *t);   // waits, then frees the handle
void os_thread_detach(OsThread *t); // frees the handle; the thread runs on
void os_thread_exit(void);          // ends the calling thread; never returns
// Asks the scheduler to keep the calling thread on the fastest cores: the
// guest's threads draw every frame. A no-op where there is no such control.
void os_thread_prefer_performance(void);
OsThreadId os_thread_self(void);
OsThreadId os_thread_id_of(const OsThread *t);

// ---------------------------------------------------------------------------
// Virtual memory: a zero-filled, readable, writable, page-aligned region.
// ---------------------------------------------------------------------------
void *os_vm_reserve(size_t bytes); // NULL on failure
void os_vm_release(void *p, size_t bytes);

// ---------------------------------------------------------------------------
// Plugins.
// ---------------------------------------------------------------------------
void *os_dlopen(const char *path);
void *os_dlopen_noload(const char *path); // a handle only if already loaded
void *os_dlsym(void *handle, const char *name);
int os_dlclose(void *handle);          // 0 on success
const char *os_dlerror(void);          // text for the last failure
const char *os_plugin_extension(void); // ".dylib", ".so" or ".dll"

// ---------------------------------------------------------------------------
// Paths. UTF-8 everywhere; the Win32 implementation converts.
// ---------------------------------------------------------------------------
typedef struct OsStat {
    uint64_t size;
    int64_t atime, mtime, ctime; // seconds since the epoch
    int is_dir, is_regular, is_symlink, is_readonly;
} OsStat;
int os_stat(const char *path, OsStat *out);      // follows symlinks; 0 or -1
int os_lstat(const char *path, OsStat *out);     // does not follow
int os_mkdir(const char *path);                  // 0, or -1 (an existing directory is -1, as mkdir)
int os_rename(const char *from, const char *to); // replaces an existing destination file
int os_unlink(const char *path);
int os_rmdir(const char *path);       // the directory must be empty
int os_getcwd(char *buf, size_t cap); // 0 or -1
int os_chdir(const char *path);
// Calls `fn` for every entry except "." and "..", in directory order. A
// nonzero return from `fn` stops the walk. -1 when the directory cannot be
// opened, 0 otherwise.
typedef int (*OsListDirFn)(const char *name, void *user);
int os_listdir(const char *dir, OsListDirFn fn, void *user);
// The trailing XXXXXX of `template_path` is replaced in place; returns an open
// read-write descriptor for the new file, or -1.
int os_mkstemp(char *template_path);
// mkdtemp: replaces the trailing XXXXXX and creates the directory; 0 or -1.
int os_mkdtemp(char *template_path);
// A writable temporary directory without a trailing separator ("/tmp", %TEMP%).
const char *os_temp_dir(void);
// "/dev/null" or "NUL".
const char *os_null_device(void);
// Bytes a new file may still take on the volume holding `path` (an existing
// file or directory). 0 or -1.
int os_free_space(const char *path, uint64_t *bytes_out);
// Sets a file's modification (and access) time, in seconds since the epoch.
int os_set_mtime(const char *path, int64_t mtime);
// A string value from the Windows registry. `key` starts with "HKLM\" or
// "HKCU\" and is read from the 64- and then the 32-bit view. -1 elsewhere.
int os_registry_read(const char *key, const char *value, char *buf, size_t cap);
// Per-user data directory for `app` (not created): ~/Library/Application Support/<app>,
// %APPDATA%\<app>, $XDG_DATA_HOME/<app> or ~/.local/share/<app>. 0 or -1.
int os_user_data_dir(const char *app, char *buf, size_t cap);
// Runs argv[0] with argv (NULL-terminated), inheriting stdio; 0 and a pid, or -1.
int os_spawn(const char *const argv[], int64_t *pid_out);
// Waits for the child. exit_code receives the exit status, or 128 + signal on
// POSIX when it died by a signal (so an abort reads as 134 everywhere); 0 or -1.
int os_wait(int64_t pid, int *exit_code);

// ---------------------------------------------------------------------------
// Descriptors. Binary mode always; created files are mode 0644.
// ---------------------------------------------------------------------------
enum {
    OS_O_RDONLY = 0,
    OS_O_WRONLY = 1,
    OS_O_RDWR = 2,
    OS_O_CREAT = 0x40,
    OS_O_EXCL = 0x80,
    OS_O_TRUNC = 0x200
};
enum { OS_SEEK_SET = 0, OS_SEEK_CUR = 1, OS_SEEK_END = 2 };
int os_fd_open(const char *path, int flags);
int64_t os_fd_read(int fd, void *buf, size_t n);        // bytes read, 0 at EOF, -1 on error
int64_t os_fd_write(int fd, const void *buf, size_t n); // bytes written, -1 on error
int64_t os_fd_seek(int fd, int64_t off, int whence);    // new offset or -1
int os_fd_close(int fd);
int os_fd_dup(int fd);
int os_fd_fsync(int fd);
int os_fd_truncate(int fd, int64_t length);
int os_fd_stat(int fd, OsStat *out);
// A FILE over an open descriptor, which then owns it.
void *os_fdopen(int fd, const char *mode);

// ---------------------------------------------------------------------------
// Process.
// ---------------------------------------------------------------------------
int os_exe_path(char *buf, size_t cap); // 0 or -1; NUL-terminated
// Report a fatal signal inside guest code. `what` is "SIGSEGV", "SIGBUS" or
// "an abort from the runtime". Returns 1 when handlers were installed and 0
// where the platform has no equivalent yet.
typedef void (*OsFaultFn)(const char *what);
int os_install_fault_handlers(OsFaultFn fn);
// The faulting address (si_addr / ExceptionAddress) of the signal that is
// being handled, or 0 when the platform did not report one. Valid only inside
// the handler; async-signal-safe.
uint64_t os_fault_address(void);
// The instruction pointer at the fault, taken from the signal context (the
// guest EIP is not written back by host code, so it can be stale when a shim
// faults). Valid only inside the handler; async-signal-safe.
uint64_t os_fault_pc(void);
// For use inside a fault handler: a raw write to the error stream and an
// immediate process exit, neither of which touches stdio or runs destructors.
void os_write_stderr_raw(const char *s, size_t n);
void os_exit_immediately(int code);

// ---------------------------------------------------------------------------
// Sampling profiler: where the calling thread's time went, measured in-process.
// ---------------------------------------------------------------------------
// Arms a statistical program-counter sampler for the calling thread. The
// platform interrupts the thread periodically and records where it was, so a
// profile can be taken where an external profiler cannot run (no window server,
// no ptrace, a 20-thread process `sample` gets 2 samples from). Off until
// called. Returns 1 when armed, 0 where the platform has no facility.
int os_sample_start(void);
// Stops the sampler and prints the ranked hot addresses, resolved to a symbol
// where the platform can, each as `samples self% symbol pc`. `out_file` is a
// `FILE *` (void here so the header needs no stdio).
void os_sample_dump(void *out_file);

// ---------------------------------------------------------------------------
// Time.
// ---------------------------------------------------------------------------
uint64_t os_monotonic_ns(void); // never goes backwards; arbitrary origin
// Broken-down local and UTC time for a Unix timestamp; 0 or -1.
int os_localtime(int64_t seconds, struct tm *out);
int os_gmtime(int64_t seconds, struct tm *out);
uint64_t os_wall_time_us(void); // microseconds since the Unix epoch
void os_sleep_us(uint64_t us);

// ---------------------------------------------------------------------------
// System dialogs. The runtime's comdlg32 and USER32 shims reach the platform's
// own file picker, alert and help through these. A host installs a native
// presenter; with none installed - a headless host, the unit tests - the
// deterministic fallback answers from the environment so a scripted run is
// reproducible:
//
//   RECOMP_FILE_DIALOG_RESULT   a path to choose, or "cancel" (the default)
//   RECOMP_MESSAGEBOX_RESULT    a button id ("6") or one of the ID* names
//                               (IDOK, IDCANCEL, IDABORT, IDRETRY, IDIGNORE,
//                               IDYES, IDNO, IDTRYAGAIN, IDCONTINUE)
// ---------------------------------------------------------------------------

// One entry of a Win32 filter string, already split by the runtime: a
// description and a semicolon-separated list of wildcard patterns.
typedef struct OsFileFilter {
    const char *description; // may be empty
    const char *patterns;    // "*.pet;*.PET", may be empty
    // The bare, lowercase, ';'-separated extensions this filter allows, derived
    // from `patterns` *and* from the extension list the description advertises
    // ("Picture Files (*.BMP; *.JPG)"). A Win32 filter only narrows the list of
    // files the picker shows - the user can still type any name - so a filter
    // whose description names a type the pattern omits (Petz 5's Editor filter
    // is exactly this: "(*.BMP; *.JPG)" but "*.bmp;") must keep that type
    // selectable. An empty string means the filter allows every file.
    const char *extensions;
} OsFileFilter;
// Derive the allowed extensions a Win32 filter advertises: every `*.ext` /
// `.ext` named by `patterns` or by a parenthesised wildcard list in
// `description`, lowercased and de-duplicated. Writes a NUL-terminated,
// ';'-separated list into `out` (`cap` bytes); an empty list means every file.
void os_file_filter_extensions(const char *patterns, const char *description, char *out, size_t cap);

typedef struct OsFileDialog {
    int save;                // 1: save panel, 0: open panel
    const char *title;       // NULL when the caller named none
    const char *initial_dir; // NULL when the caller named none
    const char *default_ext; // NULL when the caller named none
    char *file;              // in: the initial name; out: the chosen path
    size_t file_cap;         // bytes at `file`, including the NUL
    const OsFileFilter *filters;
    int filter_count;
    int filter_index;    // 1-based selected filter, or 0
    uint32_t flags;      // the caller's OFN_* flags
    int *file_offset;    // out: the name's index in `file`, or NULL
    int *file_extension; // out: the extension's index in `file`, or NULL
} OsFileDialog;
// 1: a file was chosen (`file` holds it), 0: cancelled, -1: no presenter and
// no deterministic answer to give, -2: chosen but it did not fit `file`
// (`file` holds a truncated, NUL-terminated prefix).
int os_file_dialog(OsFileDialog *d);
// Copy a chosen path into `d->file`, honouring `file_cap`, and fill the offset
// outputs. 1, or -2 when the path did not fit. Shared by the fallback and by a
// native presenter, so both compute the offsets the same way.
int os_file_dialog_set_result(OsFileDialog *d, const char *path);
typedef int (*OsFileDialogPresenter)(OsFileDialog *d);
// Install the native presenter. NULL restores the deterministic fallback.
void os_set_file_dialog_presenter(OsFileDialogPresenter fn);

// A message box reduced to its buttons by the runtime. `labels` and `ids` are
// parallel arrays of `count` entries; the chosen index is returned, or -1 when
// there is no presenter and no deterministic answer. `icon`: 0 none, 1 info,
// 2 warning, 3 error, 4 question.
typedef struct OsMessageBox {
    const char *title; // may be NULL
    const char *text;
    const char *const *labels;
    const uint32_t *ids;
    int count;
    int default_index;
    int icon;
} OsMessageBox;
int os_message_box(const OsMessageBox *m);
typedef int (*OsMessageBoxPresenter)(const OsMessageBox *m);
void os_set_message_box_presenter(OsMessageBoxPresenter fn);

// Help. Always non-fatal: log it, and open `url` in the platform browser only
// when one is given (a WinHelp file name is not a URL, so nothing opens).
// `what` tags the log line ("WinHelpA", "HtmlHelpA").
void os_help(const char *what, const char *file, const char *url);

// ---------------------------------------------------------------------------
// Strings.
// ---------------------------------------------------------------------------
int os_strcasecmp(const char *a, const char *b);

// ---------------------------------------------------------------------------
// Environment. Values are copied; 0 or -1.
// ---------------------------------------------------------------------------
int os_setenv(const char *name, const char *value);
int os_unsetenv(const char *name);
// The kit's switches: `recomp_env("PIN_CLOCK")` reads RECOMP_PIN_CLOCK.  The
// prefix names the kit, never a game; a switch is unset when NULL.
const char *recomp_env(const char *name);
// Switches from a file, for a platform with no shell to set them in (the iPad
// app reads Documents/switches.txt at start). One NAME=VALUE per line, spaces
// around either trimmed; blank lines, # comments and lines without '=' are
// skipped. Returns how many were set; 0 when there is no such file.
int recomp_env_apply_file(const char *path);

#ifdef __cplusplus
}
#endif
