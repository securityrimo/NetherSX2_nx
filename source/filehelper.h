/* filehelper.h -- POSIX-backed implementation of the Java FileHelper bridge.
 *
 * libemucore.so calls back into xyz.aethersx2.android.FileHelper for any path
 * it treats as a SAF content:// URI. Because we feed the core only absolute SD
 * paths, the native PCSX2 FileSystem layer handles them via plain POSIX and
 * FileHelper rarely fires -- but openURIAsFileDescriptor / statFile / findFiles
 * are implemented here as a safety net, plus readPackageFile(ToString) which
 * serve the core's resources (shaders, GameIndex.yaml, fonts) from RESOURCES_DIR.
 *
 * These return fake-JNI objects (see jni_fake.c); the helpers here build the
 * StatResult / FindResult / byte[] / String results the fake env hands back.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __FILEHELPER_H__
#define __FILEHELPER_H__

#include <stdint.h>

// PCSX2 FILESYSTEM_FILE_ATTRIBUTE_* bits used in StatResult/FindResult.flags
#define FH_ATTR_DIRECTORY  1
#define FH_ATTR_READ_ONLY  2
#define FH_ATTR_COMPRESSED 4

// PCSX2 FILESYSTEM_FIND_* bits passed to findFiles()
#define FH_FIND_RECURSIVE      1
#define FH_FIND_RELATIVE_PATHS 2
#define FH_FIND_HIDDEN_FILES   4
#define FH_FIND_FOLDERS        8
#define FH_FIND_FILES          16
#define FH_FIND_KEEP_ARRAY     32

// open(uri, mode) -> raw fd, or -1. mode is "r"/"rw"/"rwt"/"w" (Android SAF).
int fh_open_fd(const char *path, const char *mode);

// stat(path) -> a fake FileHelper$StatResult object (fields size:J, modifiedTime:J,
// flags:I), or NULL if the path does not exist.
void *fh_stat(const char *path);

// readdir(path, flags) -> a fake FileHelper$FindResult[] (jobjectArray), honoring
// the FH_FIND_* flags. Empty array if the dir is empty/unreadable.
void *fh_find_files(const char *path, int flags);

// read RESOURCES_DIR/<relpath> fully -> a fake byte[] (jbyteArray), or NULL.
void *fh_read_package_bytes(const char *relpath);
// read RESOURCES_DIR/<relpath> fully -> a fake String (jstring), or NULL.
void *fh_read_package_string(const char *relpath);

// string helpers used by the core for URI display
void *fh_display_name(const char *path);                  // -> jstring (basename)
void *fh_relative_path(const char *base, const char *full); // -> jstring

#endif
