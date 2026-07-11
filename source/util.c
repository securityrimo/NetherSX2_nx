/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

#ifdef DEBUG_LOG

static int s_nxlinkSock = -1;

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

void userAppInit(void) {
  initNxLink();
}

void userAppExit(void) {
  deinitNxLink();
}

#endif

// Log file state at file scope so debugFlush() (crash handler) can reach it.
#ifdef DEBUG_LOG
static Mutex log_mutex;
static FILE *log_file = NULL;
#endif

volatile int g_log_fsync_all = 0;

int debugPrintf(char *text, ...) {
#ifdef DEBUG_LOG
  // One log file kept open for the process lifetime (reopening per call was
  // thousands of SD round-trips). Every line is timestamped (ms since first
  // log), flushed AND fsync'd so a hard crash still leaves the latest output on
  // disk, and mirrored to stdout so `nxlink -s` shows it live. This is
  // crash-safe but slow: keep it off (config.h DEBUG_LOG) for release.
  static int tried_open = 0;
  static u64 t0 = 0, freq = 0;

  char buf[0x1000];
  va_list list;
  va_start(list, text);
  vsnprintf(buf, sizeof(buf), text, list);
  va_end(list);

  mutexLock(&log_mutex);
  if (!tried_open) {
    tried_open = 1;
    freq = armGetSystemTickFreq();
    t0 = armGetSystemTick();
    log_file = fopen(LOG_PATH, "w");
    if (log_file) {
      // ★ PERF: 64 KB full buffering so log writes BATCH instead of hitting the SD
      // card per line. Combined with the reduced flush cadence below, this removes
      // most of the per-line SD-write stall that was capping in-game FPS.
      static char logbuf[65536];
      setvbuf(log_file, logbuf, _IOFBF, sizeof(logbuf));
      fprintf(log_file, "==== NetherSX2_nx debug log (built %s %s) ====\n",
              __DATE__, __TIME__);
    }
  }
  const u64 ms = freq ? ((armGetSystemTick() - t0) * 1000ull / freq) : 0;
  if (log_file) {
    fprintf(log_file, "[%7llu] %s", (unsigned long long)ms, buf);
    // ★ PERF: don't fflush/fsync per line. fflush every 32 lines pushes the 64 KB
    // buffer to the FS; fsync (a real SD commit, ~ms) every 256 (16 in the VM
    // teardown phase, g_log_fsync_all). A hard crash loses only the tail: the
    // fixed exception handler writes a separately-fsync'd crash.log AND calls
    // debugFlush() (below), so the fault line is always durable.
    static unsigned sync_ctr = 0;
    const unsigned c = ++sync_ctr;
    if ((c & 0x1fu) == 0) fflush(log_file);
    if ((c & (g_log_fsync_all ? 0xfu : 0xffu)) == 0) { fflush(log_file); fsync(fileno(log_file)); }
  }
  // (Dropped the per-line nxlink stdout mirror: on real hardware there is no host
  // listening, so fputs+fflush(stdout) every line was pure wasted I/O. The full
  // log is in LOG_PATH.)
  mutexUnlock(&log_mutex);
#else
  (void)text;
#endif
  return 0;
}

void debugFlush(void) {
#ifdef DEBUG_LOG
  // Best-effort: skip if another thread holds the log lock (we must not block in
  // the crash handler). If we get it, push the buffered log to SD.
  if (log_file && mutexTryLock(&log_mutex)) {
    fflush(log_file);
    fsync(fileno(log_file));
    mutexUnlock(&log_mutex);
  }
#endif
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }
