/* filehelper.c -- POSIX-backed implementation of the Java FileHelper bridge.
 *
 * libemucore.so calls these when it treats a path as a SAF content:// URI.
 * Since we hand the core only absolute SD paths, its native PCSX2 FileSystem
 * layer normally services them via plain POSIX and these rarely fire -- but
 * openURIAsFileDescriptor / statFile / findFiles are real implementations as a
 * safety net, and readPackageFile(ToString) serve the core's resources
 * (shaders, GameIndex.yaml, fonts) from RESOURCES_DIR. Results are handed back
 * as fake-JNI objects built via the jni_fake.h builders.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "filehelper.h"

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// Join dir + "/" + name into out (caller owns nothing; out is a fixed buffer).
// Avoids a double slash when dir already ends in '/'.
static void path_join(char *out, size_t cap, const char *dir, const char *name) {
  size_t dl = strlen(dir);
  if (dl && dir[dl - 1] == '/')
    snprintf(out, cap, "%s%s", dir, name);
  else
    snprintf(out, cap, "%s/%s", dir, name);
}

// st_mode -> StatResult/FindResult flags. The DIRECTORY bit is the one the core
// actually keys off; READ_ONLY is best-effort from the write-permission bits
// (Horizon's fs doesn't carry meaningful per-file owner perms, so this is a
// hint at most -- verify on-device if a read-only-detection path misbehaves).
static int mode_to_flags(const struct stat *st) {
  int flags = 0;
  if (S_ISDIR(st->st_mode))
    flags |= FH_ATTR_DIRECTORY;
  if (!(st->st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)))
    flags |= FH_ATTR_READ_ONLY;
  return flags;
}

// Build one FileHelper$FindResult from an already-stat'd entry.
static void *make_find_result(const char *full, const char *rel,
                              const struct stat *st) {
  void *o = jni_obj_new("FileHelper$FindResult");
  if (!o)
    return NULL;
  jni_obj_set_string(o, "name", full);
  jni_obj_set_string(o, "relativeName", rel);
  jni_obj_set_long(o, "size", (int64_t)st->st_size);
  jni_obj_set_long(o, "modifiedTime", (int64_t)st->st_mtime);
  jni_obj_set_int(o, "flags", mode_to_flags(st));
  return o;
}

// ---------------------------------------------------------------------------
// openURIAsFileDescriptor -- the file-open bridge
// ---------------------------------------------------------------------------

// Translate an Android SAF/ParcelFileDescriptor mode string to open(2) flags.
// SAF modes are combinations of r/w/t/a: "r" read, "w"/"rw" read-write+create,
// "rwt"/"wt" truncate, "wa"/"a" append. We over-permit (always allow create on
// any write) which matches what the core expects from openURIAsFileDescriptor.
static int mode_to_oflags(const char *mode) {
  int has_r = 0, has_w = 0, has_t = 0, has_a = 0;
  if (!mode)
    mode = "r";
  for (const char *p = mode; *p; p++) {
    switch (*p) {
      case 'r': has_r = 1; break;
      case 'w': has_w = 1; break;
      case 't': has_t = 1; break;
      case 'a': has_a = 1; break;
      default: break;
    }
  }
  if (!has_w && !has_a)
    return O_RDONLY;            // pure read
  int flags = O_RDWR | O_CREAT; // any write implies create
  if (has_a)
    flags |= O_APPEND;
  else if (has_t || !has_r)
    flags |= O_TRUNC;           // "w"/"rwt": start empty
  return flags;
}

int fh_open_fd(const char *path, const char *mode) {
  if (!path)
    return -1;
  int flags = mode_to_oflags(mode);
  int fd = open(path, flags, 0644);
  return fd;
}

// ---------------------------------------------------------------------------
// statFile
// ---------------------------------------------------------------------------

void *fh_stat(const char *path) {
  struct stat st;
  if (!path || stat(path, &st) != 0)
    return NULL; // missing -> Java null

  void *o = jni_obj_new("FileHelper$StatResult");
  if (!o)
    return NULL;
  jni_obj_set_long(o, "size", (int64_t)st.st_size);
  jni_obj_set_long(o, "modifiedTime", (int64_t)st.st_mtime);
  jni_obj_set_int(o, "flags", mode_to_flags(&st));
  return o;
}

// ---------------------------------------------------------------------------
// findFiles
// ---------------------------------------------------------------------------

// Recursive worker: append FindResults for `dir` into the growing list `items`.
// `root` is the original search base (for FH_FIND_RELATIVE_PATHS). Entries are
// collected into a dynamic array first because the count isn't known until the
// (possibly recursive) walk completes; the caller packs them into a fake array.
static void find_walk(const char *root, const char *dir, int flags,
                      void ***items, int *count, int *cap) {
  DIR *d = opendir(dir);
  if (!d) {

    return;
  }

  size_t rootlen = strlen(root);
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    const char *nm = de->d_name;
    if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
      continue; // skip "." and ".."
    if (nm[0] == '.' && !(flags & FH_FIND_HIDDEN_FILES))
      continue; // skip dotfiles unless hidden requested

    char full[1024];
    path_join(full, sizeof(full), dir, nm);

    struct stat st;
    if (stat(full, &st) != 0)
      continue; // dangling entry -- skip

    int is_dir = S_ISDIR(st.st_mode);
    int want = is_dir ? (flags & FH_FIND_FOLDERS) : (flags & FH_FIND_FILES);

    if (want) {
      // relativeName: full path minus the root prefix (+ '/') when relative
      // paths are requested, else just the leaf name.
      const char *rel = nm;
      if ((flags & FH_FIND_RELATIVE_PATHS) &&
          !strncmp(full, root, rootlen)) {
        rel = full + rootlen;
        while (*rel == '/')
          rel++;
      }

      void *fr = make_find_result(full, rel, &st);
      if (fr) {
        if (*count >= *cap) {
          int ncap = *cap ? *cap * 2 : 16;
          void **ni = realloc(*items, ncap * sizeof(void *));
          if (!ni) {
            closedir(d);
            return; // OOM: return what we have so far
          }
          *items = ni;
          *cap = ncap;
        }
        (*items)[(*count)++] = fr;
      }
    }

    if (is_dir && (flags & FH_FIND_RECURSIVE))
      find_walk(root, full, flags, items, count, cap);
  }

  closedir(d);
}

void *fh_find_files(const char *path, int flags) {
  // If neither FILES nor FOLDERS requested, the upstream default is both.
  if (!(flags & (FH_FIND_FILES | FH_FIND_FOLDERS)))
    flags |= FH_FIND_FILES | FH_FIND_FOLDERS;

  void **items = NULL;
  int count = 0, cap = 0;
  if (path)
    find_walk(path, path, flags, &items, &count, &cap);

  // Pack into a fake jobjectArray (empty array, never NULL).
  void *arr = jni_make_object_array(count);
  for (int i = 0; i < count; i++)
    jni_obj_array_set(arr, i, items[i]);
  free(items);


  return arr;
}

// ---------------------------------------------------------------------------
// readPackageFile(ToString) -- resources served from RESOURCES_DIR
// ---------------------------------------------------------------------------

// Read RESOURCES_DIR/<relpath> fully into a malloc'd buffer. On success returns
// the buffer and writes its length (NUL-terminator is appended but not counted)
// to *out_len; returns NULL on any failure.
static void *read_package_buf(const char *relpath, long *out_len) {
  if (!relpath)
    return NULL;

  char full[1024];
  path_join(full, sizeof(full), RESOURCES_DIR, relpath);

  FILE *f = fopen(full, "rb");
  if (!f) {

    return NULL;
  }

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long len = ftell(f);
  if (len < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);

  // +1 so callers building a String get a NUL-terminated buffer for free.
  char *buf = malloc((size_t)len + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }

  size_t got = fread(buf, 1, (size_t)len, f);
  fclose(f);
  if (got != (size_t)len) {

    free(buf);
    return NULL;
  }

  buf[len] = '\0';
  if (out_len)
    *out_len = len;
  return buf;
}

void *fh_read_package_bytes(const char *relpath) {
  long len = 0;
  char *buf = read_package_buf(relpath, &len);
  if (!buf)
    return NULL;
  void *arr = jni_make_byte_array(buf, (int)len);
  free(buf);
  return arr;
}

void *fh_read_package_string(const char *relpath) {
  long len = 0;
  char *buf = read_package_buf(relpath, &len);
  if (!buf)
    return NULL;
  void *s = jni_make_string(buf); // buf is NUL-terminated by read_package_buf
  free(buf);
  return s;
}

// ---------------------------------------------------------------------------
// getDisplayNameForURIPath / getRelativePathForURIPath -- path string math
// ---------------------------------------------------------------------------

void *fh_display_name(const char *path) {
  if (!path)
    return jni_make_string("");
  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  return jni_make_string(base);
}

void *fh_relative_path(const char *base, const char *full) {
  if (!full)
    return jni_make_string("");
  if (!base)
    return jni_make_string(full);

  size_t bl = strlen(base);
  // Tolerate a trailing slash on base so "/a/b/" and "/a/b" behave the same.
  if (bl && base[bl - 1] == '/')
    bl--;

  if (!strncmp(full, base, bl)) {
    const char *rel = full + bl;
    while (*rel == '/')
      rel++; // strip the leading separator(s)
    return jni_make_string(rel);
  }
  // Not under base -- hand back the full path unchanged.
  return jni_make_string(full);
}
