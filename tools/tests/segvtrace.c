/*
 * segvtrace: LD_PRELOAD library for the console (no gdb there): on SIGSEGV,
 * SIGBUS, SIGILL, SIGFPE or SIGABRT it prints to stderr (and to
 * $SEGVTRACE_LOG if set) the fault address, PC and LR from the signal
 * context, each as library + offset (dladdr), and a backtrace; then the
 * signal is re-raised with the default action. Offsets resolve to source
 * lines on the PC with llvm-addr2line against the unstripped build.
 *
 *   aarch64 build (on the PC): clang-19 --target=aarch64-linux-gnu --sysroot=<sysroot> \
 *     -O1 -g -fPIC -shared -o segvtrace.so segvtrace.c -ldl
 *   use: LD_PRELOAD=/home/ark/segvtrace.so program ...
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static int log_fd = -1;

static void
out(const char *s)
{
   size_t n = strlen(s);
   if (write(2, s, n) < 0) {
   }
   if (log_fd >= 0 && write(log_fd, s, n) < 0) {
   }
}

static void
where(const char *what, uintptr_t addr)
{
   char line[512];
   Dl_info info;
   if (addr && dladdr((void *)addr, &info) && info.dli_fname) {
      snprintf(line, sizeof(line), "segvtrace: %s 0x%lx = %s+0x%lx (%s+0x%lx)\n", what,
               (unsigned long)addr, info.dli_fname,
               (unsigned long)(addr - (uintptr_t)info.dli_fbase),
               info.dli_sname ? info.dli_sname : "?",
               info.dli_saddr ? (unsigned long)(addr - (uintptr_t)info.dli_saddr) : 0ul);
   } else {
      snprintf(line, sizeof(line), "segvtrace: %s 0x%lx\n", what, (unsigned long)addr);
   }
   out(line);
}

static void
handler(int sig, siginfo_t *si, void *ctx)
{
   char line[256];
   snprintf(line, sizeof(line), "segvtrace: signal %d (%s) code %d, fault address %p, thread %d\n",
            sig, strsignal(sig), si->si_code, si->si_addr, (int)gettid());
   out(line);
#if defined(__aarch64__)
   const ucontext_t *uc = ctx;
   where("pc", (uintptr_t)uc->uc_mcontext.pc);
   where("lr", (uintptr_t)uc->uc_mcontext.regs[30]);
   snprintf(line, sizeof(line), "segvtrace: sp 0x%lx x0 0x%lx x1 0x%lx x2 0x%lx x3 0x%lx\n",
            (unsigned long)uc->uc_mcontext.sp, (unsigned long)uc->uc_mcontext.regs[0],
            (unsigned long)uc->uc_mcontext.regs[1], (unsigned long)uc->uc_mcontext.regs[2],
            (unsigned long)uc->uc_mcontext.regs[3]);
   out(line);
#else
   (void)ctx;
#endif
   void *frames[64];
   int n = backtrace(frames, 64);
   for (int i = 0; i < n; i++) {
      char what[16];
      snprintf(what, sizeof(what), "#%d", i);
      where(what, (uintptr_t)frames[i]);
   }
   signal(sig, SIG_DFL);
   raise(sig);
}

__attribute__((constructor)) static void
init(void)
{
   const char *log = getenv("SEGVTRACE_LOG");
   if (log)
      log_fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
   /* backtrace() loads libgcc_s on first use, which must not happen in the
    * handler. */
   void *warm[2];
   backtrace(warm, 2);
   static char stack[64 * 1024];
   stack_t ss = {.ss_sp = stack, .ss_size = sizeof(stack)};
   sigaltstack(&ss, NULL);
   struct sigaction sa;
   memset(&sa, 0, sizeof(sa));
   sa.sa_sigaction = handler;
   sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
   int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
   for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
      sigaction(sigs[i], &sa, NULL);
}
