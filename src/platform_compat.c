/* Portable directory walking and exe-directory resolution.
 * See platform_compat.h for why these exist. */
#include "platform_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define EPOCH_CHDIR _chdir
#else
#include <dirent.h>
#include <unistd.h>
#define EPOCH_CHDIR chdir
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

/* ------------------------------------------------------------------ */
/* directory iteration                                                 */
/* ------------------------------------------------------------------ */

struct EpochDir {
#ifdef _WIN32
    HANDLE handle;
    WIN32_FIND_DATAA data;
    bool pending;   /* FindFirstFile already produced an unconsumed entry */
#else
    DIR* handle;
#endif
};

EpochDir* epoch_dir_open(const char* path) {
    if (!path || !*path) return NULL;

    EpochDir* dir = (EpochDir*)calloc(1, sizeof(EpochDir));
    if (!dir) return NULL;

#ifdef _WIN32
    /* FindFirstFile wants a wildcard, and hands back the first entry
     * immediately -- so it has to be held until the first _next() call. */
    char pattern[MAX_PATH];
    int n = snprintf(pattern, sizeof(pattern), "%s\\*", path);
    if (n < 0 || (size_t)n >= sizeof(pattern)) {
        free(dir);
        return NULL;
    }
    dir->handle = FindFirstFileA(pattern, &dir->data);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        free(dir);
        return NULL;
    }
    dir->pending = true;
#else
    dir->handle = opendir(path);
    if (!dir->handle) {
        free(dir);
        return NULL;
    }
#endif
    return dir;
}

const char* epoch_dir_next(EpochDir* dir) {
    if (!dir) return NULL;

    for (;;) {
        const char* name = NULL;
#ifdef _WIN32
        if (dir->pending) {
            dir->pending = false;
        } else if (!FindNextFileA(dir->handle, &dir->data)) {
            return NULL;
        }
        name = dir->data.cFileName;
#else
        struct dirent* ent = readdir(dir->handle);
        if (!ent) return NULL;
        name = ent->d_name;
#endif
        if (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;  /* skip . and .. */
        }
        return name;
    }
}

void epoch_dir_close(EpochDir* dir) {
    if (!dir) return;
#ifdef _WIN32
    if (dir->handle != INVALID_HANDLE_VALUE) FindClose(dir->handle);
#else
    if (dir->handle) closedir(dir->handle);
#endif
    free(dir);
}

bool epoch_is_dir(const char* path) {
    if (!path || !*path) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

/* ------------------------------------------------------------------ */
/* executable directory                                                */
/* ------------------------------------------------------------------ */

bool epoch_chdir_to_exe_dir(void) {
    char buf[4096];

#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        fprintf(stderr, "[PATH] GetModuleFileName failed\n");
        return false;
    }
    buf[n] = '\0';
#elif defined(__APPLE__)
    uint32_t cap = (uint32_t)sizeof(buf);
    if (_NSGetExecutablePath(buf, &cap) != 0) {
        fprintf(stderr, "[PATH] _NSGetExecutablePath needs %u bytes\n", (unsigned)cap);
        return false;
    }
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        fprintf(stderr, "[PATH] readlink(/proc/self/exe) failed\n");
        return false;
    }
    buf[n] = '\0';
#else
    (void)buf;
    fprintf(stderr, "[PATH] cannot locate the executable on this platform\n");
    return false;
#endif

    /* Strip the filename. Windows accepts forward slashes throughout, but
     * GetModuleFileName returns backslashes, so cut on either. */
    char* last = NULL;
    for (char* p = buf; *p; p++) {
        if (*p == '/' || *p == '\\') last = p;
    }
    if (!last) return false;
    *last = '\0';

    if (EPOCH_CHDIR(buf) != 0) {
        fprintf(stderr, "[PATH] could not chdir to '%s'\n", buf);
        return false;
    }
    return true;
}
