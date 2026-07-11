/* crash.c -- libnx CPU exception handler.
 *
 * On an emulator (or anywhere Atmosphère's crash_reports aren't available) a
 * guest fault just closes the app with no report. Defining __libnx_exception_handler
 * (+ an exception stack) makes libnx route CPU exceptions here, so we can dump the
 * faulting PC / LR / fault-address / GPRs -- with the offset into libemucore.so
 * computed -- to /switch/nethersx2/crash.log before exiting.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "libc_shim.h" // JitRedirect / g_jit_redir for the dual-mapped-JIT exec redirect

extern so_module emu_mod; // libemucore.so (defined in main.c)

// dedicated stack for the exception handler (it runs after the crash, so it
// can't use the faulted thread's stack). fprintf/snprintf want some headroom.
__attribute__((aligned(0x1000))) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

// Full faulting context, built by our custom __libnx_exception_entry (exc_entry.s)
// and consumed here. g_exc_frame is the kernel's TLS exception frame; the asm
// propagates g_exc's (possibly modified) PC/GPRs back into it on the resume path.
ThreadExceptionDump g_exc;
void *g_exc_frame;

// append " (emucore+0xNNN)" if `v` falls inside the loaded core image
static int annotate(char *buf, size_t n, u64 v) {
  uintptr_t base = (uintptr_t)emu_mod.load_virtbase;
  uintptr_t end = base + emu_mod.load_size;
  if (v >= base && v < end)
    return snprintf(buf, n, " (emucore+0x%lx)", (unsigned long)(v - base));
  buf[0] = '\0';
  return 0;
}

// AArch64 store decoder for the JIT write-redirect. The recompiler emits code by
// STORING instruction words to the RX view (which isn't writable -> data abort);
// we re-issue the store to the RW alias `dst` (= fault_addr + delta) and advance
// PC past it. `ctx->far.x` is the faulting (RX) address = the store's target.
// Returns 1 if the faulting instruction was a store we handled, 0 otherwise.
static int jit_emulate_store(ThreadExceptionDump *ctx, u8 *dst) {
  const u32 insn = *(const volatile u32 *)(uintptr_t)ctx->pc.x; // emitter code (R+X)
  const unsigned Rt = insn & 0x1f;
  const unsigned Rn = (insn >> 5) & 0x1f;
  #define GPR(r)     ((r) == 31 ? 0ULL : ctx->cpu_gprs[r].x)          // Rt==31 -> XZR
  #define SETBASE(v) do { if (Rn == 31) ctx->sp.x = (u64)(v); else ctx->cpu_gprs[Rn].x = (u64)(v); } while (0)

  // Load/store register PAIR: bits[29:27]==0b101, L(bit22)==0 (store)
  if (((insn >> 27) & 0x7) == 0x5 && ((insn >> 22) & 1) == 0) {
    const unsigned opc = (insn >> 30) & 3, V = (insn >> 26) & 1, Rt2 = (insn >> 10) & 0x1f;
    const unsigned kind = (insn >> 23) & 3;               // 1=post,2=offset,3=pre
    const unsigned scale = V ? (2u + opc) : (opc == 2 ? 3u : 2u);
    const size_t esz = (size_t)1 << scale;
    if (V) { memcpy(dst, &ctx->fpu_gprs[Rt], esz); memcpy(dst + esz, &ctx->fpu_gprs[Rt2], esz); }
    else   { const u64 a = GPR(Rt), b = GPR(Rt2); memcpy(dst, &a, esz); memcpy(dst + esz, &b, esz); }
    if (kind == 3) SETBASE(ctx->far.x);                   // pre-index: Rn = fault addr
    else if (kind == 1) { int i7 = (int)((insn >> 15) & 0x7f); if (i7 & 0x40) i7 -= 0x80;
                          SETBASE((int64_t)ctx->far.x + (int64_t)i7 * (int64_t)esz); } // post
    ctx->pc.x += 4; return 1;
  }
  // Load/store SINGLE register: bits[29:27]==0b111
  if (((insn >> 27) & 0x7) == 0x7) {
    const unsigned size = (insn >> 30) & 3, V = (insn >> 26) & 1, opc = (insn >> 22) & 3;
    unsigned scale = size; int is_store;
    if (!V) is_store = (opc == 0);
    else { is_store = (opc == 0 || opc == 2); if (opc == 2) scale = 4; } // 128-bit SIMD
    if (!is_store) return 0;
    const size_t esz = (size_t)1 << scale;
    if (V) memcpy(dst, &ctx->fpu_gprs[Rt], esz);
    else { const u64 v = GPR(Rt); memcpy(dst, &v, esz); }
    if (((insn >> 24) & 3) == 0 && ((insn >> 21) & 1) == 0) { // unscaled/pre/post form
      const unsigned idx = (insn >> 10) & 3;                  // 0=unscaled,1=post,3=pre
      if (idx == 3) SETBASE(ctx->far.x);
      else if (idx == 1) { int i9 = (int)((insn >> 12) & 0x1ff); if (i9 & 0x100) i9 -= 0x200;
                           SETBASE((int64_t)ctx->far.x + i9); }
    }
    ctx->pc.x += 4; return 1;
  }
  return 0;
  #undef GPR
  #undef SETBASE
}

// Called by our custom __libnx_exception_entry (exc_entry.s) with ctx = &g_exc,
// the faulting context it captured. RETURNING resumes the faulted thread (the
// asm writes ctx's modified PC/GPRs back into the kernel frame and calls
// svcReturnFromException); the crash path never returns (svcExitProcess). Our
// entry replaces libnx's stock one, whose returnentry svcBreak()s on return --
// which is why the JIT fast path could never actually resume before.
void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  // FAST PATH -- JIT write-redirect. The core executes the RX code view DIRECTLY
  // (no per-dispatch faults); only its codegen STORES to RX fault here (RX isn't
  // writable). Decode the store, re-issue it to the RW alias, advance PC, resume.
  // Allocation-free; runs on the (relatively rare) codegen path, not every branch.
  {
    const unsigned ec = (ctx->esr >> 26) & 0x3f; // 0x24/0x25 = data abort
    if (ec == 0x24 || ec == 0x25) {
      const uintptr_t far = ctx->far.x;
      const int n = g_jit_redir_n;
      for (int i = 0; i < n; i++) {
        if (far >= g_jit_redir[i].lo && far < g_jit_redir[i].hi) {
          u8 *const dst = (u8 *)(uintptr_t)((intptr_t)far + g_jit_redir[i].delta);
          if (jit_emulate_store(ctx, dst))
            return;                                // re-issued to RW; asm resumes at PC+4
          break;                                   // in JIT range but not a decodable store
        }
      }
    }
  }

  // Real fault (not a JIT codegen store). Serialize: with the recompiler running
  // on multiple threads, a second thread faulting while this one logs would
  // corrupt the static buffer / libnx's shared exception stack and turn a clean
  // crash into an opaque svcBreak (why crash.log kept coming out stale). First
  // thread here logs + exits; any other faulting thread just parks until then.
  static volatile int crashing = 0;
  if (__atomic_exchange_n(&crashing, 1, __ATOMIC_SEQ_CST)) {
    for (;;) svcSleepThread(1000000000ULL);
  }

  static char out[4096];
  char ann[48];
  int p = 0;
  #define APP(...) do { if (p < (int)sizeof(out)) p += snprintf(out + p, sizeof(out) - p, __VA_ARGS__); } while (0)

  const unsigned ec2 = (ctx->esr >> 26) & 0x3f;
  APP("=== NetherSX2_nx CPU EXCEPTION ===\n");
  annotate(ann, sizeof(ann), ctx->far.x);
  APP("error_desc=0x%x  esr=0x%x  far=0x%lx%s\n",
      ctx->error_desc, ctx->esr, (unsigned long)ctx->far.x, ann);
  // On a data abort the PC is valid & mapped (only the data access faulted), so
  // it's safe to read the faulting instruction word -- decisive for telling an
  // undecodable codegen store (str into the JIT arena) from a bad guest access.
  if (ec2 == 0x24 || ec2 == 0x25)
    APP("faulting insn @PC = 0x%08x  (WnR=%d)\n",
        *(const volatile u32 *)(uintptr_t)ctx->pc.x, (int)((ctx->esr >> 6) & 1));
  for (int i = 0; i < g_jit_redir_n; i++)
    APP("JIT[%d] rx=[0x%lx,0x%lx) delta(rw-rx)=0x%lx\n", i,
        (unsigned long)g_jit_redir[i].lo, (unsigned long)g_jit_redir[i].hi,
        (unsigned long)g_jit_redir[i].delta);
  annotate(ann, sizeof(ann), ctx->pc.x);
  APP("PC=0x%lx%s\n", (unsigned long)ctx->pc.x, ann);
  annotate(ann, sizeof(ann), ctx->lr.x);
  APP("LR=0x%lx%s\n", (unsigned long)ctx->lr.x, ann);
  APP("SP=0x%lx  FP=0x%lx\n", (unsigned long)ctx->sp.x, (unsigned long)ctx->fp.x);
  for (int i = 0; i < 29; i++) {
    annotate(ann, sizeof(ann), ctx->cpu_gprs[i].x);
    APP("x%-2d=0x%lx%s\n", i, (unsigned long)ctx->cpu_gprs[i].x, ann);
  }
  #undef APP

  // Push the fault into the ALREADY-OPEN debug.log first and force it to SD. This
  // needs no fresh malloc/fopen (which can fail or deadlock mid-exception), so the
  // fault is durable even if the crash.log write below fails -- that blindness
  // (stale crash.log + no fault line in debug.log) is exactly what kept biting us.
  debugPrintf("%s", out);
  debugFlush();

  // Bonus: also drop a standalone crash.log for convenience.
  FILE *f = fopen(DATA_ROOT "/crash.log", "w");
  if (f) {
    fwrite(out, 1, (size_t)p, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
  }

  // We are on the exception stack, mid-fault. __libnx_exit() would run atexit
  // handlers / C++ destructors / thread teardown, any of which can fault again
  // here and turn a clean crash into an svcBreak (no crash.log). Exit straight
  // through the kernel instead -- the crash.log is already fsync'd above.
  svcExitProcess();
}
