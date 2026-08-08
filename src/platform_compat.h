/**
 * @file platform_compat.h
 * @brief The few POSIX-isms this project needs, made portable.
 *
 * Two gaps stand between the project and a working Windows build:
 *
 *   1. Directory listing. The mod loader walks `mods/`, which meant
 *      <dirent.h> -- fine on Linux, macOS and MinGW, absent on MSVC.
 *
 *   2. Locating the executable. The runtime's gb_chdir_to_exe_dir() is
 *      implemented for Linux and macOS and returns false everywhere else,
 *      by its own admission. Without it every relative path (roms/,
 *      assets/, mods/, covers/) resolves against the *current* directory
 *      instead of the binary's, so launching from anywhere other than the
 *      build folder silently finds nothing. Double-clicking on Windows
 *      does exactly that.
 *
 * Both are small enough to solve here rather than wait on upstream.
 */
#ifndef EPOCH_PLATFORM_COMPAT_H
#define EPOCH_PLATFORM_COMPAT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque directory walk. Same shape on every platform. */
typedef struct EpochDir EpochDir;

/** Open a directory for iteration. NULL if it doesn't exist. */
EpochDir* epoch_dir_open(const char* path);

/**
 * Next entry name, or NULL at the end. Returns bare names, not paths, and
 * never yields "." or "..". The pointer is valid until the next call.
 */
const char* epoch_dir_next(EpochDir* dir);

void epoch_dir_close(EpochDir* dir);

/** True if @p path exists and is a directory. */
bool epoch_is_dir(const char* path);

/**
 * chdir to the directory holding this executable, so every relative path
 * in the project resolves the same way however it was launched.
 *
 * Supersedes the runtime's gb_chdir_to_exe_dir(): same contract, but
 * implemented for Windows too. Idempotent.
 */
bool epoch_chdir_to_exe_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* EPOCH_PLATFORM_COMPAT_H */
