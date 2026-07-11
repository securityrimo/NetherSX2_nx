/* util.h -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

int debugPrintf(char *text, ...);

// force the debug log all the way to SD (used by the crash handler so the fault
// is durable even when opening a fresh crash.log fails mid-exception).
void debugFlush(void);

// when set, debugPrintf fsyncs EVERY line (not just every 128). main() flips it
// on right before runVMThread so the VM-execution phase -- where crashes bypass
// the exception handler (svcBreak) -- leaves an exact last line on disk. Boot up
// to that point stays batched/fast.
extern volatile int g_log_fsync_all;

void cpu_boost(int on);

int ret0(void);

static inline void* armGetTlsRw(void) {
  void* ret;
  __asm__ ("mrs %x[data], s3_3_c13_c0_2" : [data] "=r" (ret));
  return ret;
}

static inline void armSetTlsRw(void *addr) {
  __asm__  ("msr s3_3_c13_c0_2, %0" : : "r"(addr));
}

#endif
