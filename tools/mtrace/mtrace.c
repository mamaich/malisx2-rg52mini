/*
 * mtrace.so - LD_PRELOAD tracer of a kbase job-manager client (libmali or
 * malisx2) on the RG52 Mini: what the driver hands the kernel, decoded with
 * Mesa's pandecode (Bifrost v7).
 *
 * It follows /dev/mali0: every mmap of the kbase fd becomes a pandecode
 * mapping (SAME_VA cookies map at their GPU address; other regions map with
 * the GPU address as the file offset). Regions the driver unmaps that hold
 * code or descriptors are kept as private copies, so a later job can still
 * be decoded. Every JOB_SUBMIT is logged (atom number, core requirements,
 * dependencies, job chain), and the job chains of submits MTRACE_FROM to
 * MTRACE_FROM + MTRACE_COUNT - 1 are decoded into pandecode's dump file
 * (PANDECODE_DUMP_FILE, default /tmp/mtrace/pandecode).
 *
 *   MTRACE_DIR    log directory (default /tmp/mtrace)
 *   MTRACE_FROM   first submit to decode (default 300)
 *   MTRACE_AFTER  or: the first submit this many seconds after the first one
 *   MTRACE_COUNT  submits to decode (default 20)
 *   MTRACE_GPU_ID the GPU_ID register for pandecode (default 0x74021000, Mali-G52 r1p0)
 *   MTRACE_RAW    1: also every decoded job as hex words into the log, and
 *                 again MTRACE_REDUMP (4) submits later, once the GPU has run it
 *
 * Build: build.sh here (on the PC, with the malisx2 tree's Mesa and a cross build of it).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "genxml/decode.h"
#include "lib/wrap.h"

void disassemble_bifrost(FILE *fp, const void *code, size_t size, bool verbose);

/* kbase uapi, the parts used here (malisx2 src/kbase/uapi*.h) */
#define KB_IOCTL_TYPE 0x80
#define KB_JM_IOCTL_JOB_SUBMIT 0x40108002u
#define KB_IOCTL_MEM_ALLOC 0xc0208005u
#define KB_IOCTL_MEM_ALLOC_EX 0xc040803bu
#define KB_IOCTL_MEM_FREE 0x40088007u
#define KB_IOCTL_MEM_JIT_INIT 0x4018800eu
#define KB_IOCTL_MEM_COMMIT 0x40108014u
#define KB_IOCTL_MEM_IMPORT 0xc0188016u
#define KB_IOCTL_MEM_FLAGS_CHANGE 0x40188017u
#define KB_IOCTL_MEM_ALIAS 0xc0208015u
#define KB_MEM_COOKIE_BASE (64ull << 12)
#define KB_MEM_FIRST_FREE_ADDRESS ((64ull << 12) + KB_MEM_COOKIE_BASE)

struct kb_jm_job_submit {
   uint64_t addr;
   uint32_t nr_atoms;
   uint32_t stride;
};

struct kb_jm_dep {
   uint8_t atom_id;
   uint8_t dependency_type;
};

struct kb_jm_atom {
   uint64_t seq_nr;
   uint64_t jc;
   uint64_t udata[2];
   uint64_t extres_list;
   uint16_t nr_extres;
   uint8_t jit_id[2];
   struct kb_jm_dep pre_dep[2];
   uint8_t atom_number;
   uint8_t prio;
   uint8_t device_nr;
   uint8_t jobslot;
   uint32_t core_req;
   uint8_t renderpass_id;
   uint8_t padding[7];
};

#define KB_JM_REQ_SOFT_JOB (1u << 9)

static int (*real_open)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static int (*real_ioctl)(int, unsigned long, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int (*real_munmap)(void *, size_t);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int mali_fd = -1;
static FILE *tlog;
static struct pandecode_context *pd;
static uint64_t submits;
static uint64_t from = 300, count = 20, gpu_id = 0x74021000;
static double after_s = -1, first_s;
static bool raw_dump;            /* MTRACE_RAW=1: raw_chain dumps */
static uint64_t redump_delay = 4; /* MTRACE_REDUMP */

struct region {
   void *cpu;
   uint64_t gpu;
   size_t size;
   bool copy; /* our own copy of an unmapped region */
};
static struct region *regions;
static size_t nregions, cap;
static uint64_t copied;

static void
resolve(void)
{
   if (real_ioctl)
      return;
   real_open = dlsym(RTLD_NEXT, "open");
   real_openat = dlsym(RTLD_NEXT, "openat");
   real_ioctl = dlsym(RTLD_NEXT, "ioctl");
   real_mmap = dlsym(RTLD_NEXT, "mmap");
   real_munmap = dlsym(RTLD_NEXT, "munmap");
}

static void
disasm(FILE *fp, const void *code, size_t size, uint64_t id, bool verbose)
{
   (void)id;
   disassemble_bifrost(fp, code, size, verbose);
}

/* An abort while a chain is decoded: the backtrace into the log, the dump
 * flushed, then the default action. */
static void
on_abort(int sig)
{
   if (tlog) {
      fprintf(tlog, "SIGABRT, backtrace:\n");
      fflush(tlog);
      void *bt[64];
      const int n = backtrace(bt, 64);
      backtrace_symbols_fd(bt, n, fileno(tlog));
   }
   if (pd && pd->dump_stream)
      fflush(pd->dump_stream);
   signal(sig, SIG_DFL);
   raise(sig);
}

__attribute__((constructor)) static void
init(void)
{
   resolve();
   const char *d = getenv("MTRACE_DIR");
   if (!d)
      d = "/tmp/mtrace";
   mkdir(d, 0777);
   char path[512];
   snprintf(path, sizeof(path), "%s/mtrace.%d.log", d, getpid());
   tlog = fopen(path, "w");
   if (getenv("MTRACE_FROM"))
      from = strtoull(getenv("MTRACE_FROM"), NULL, 0);
   if (getenv("MTRACE_COUNT"))
      count = strtoull(getenv("MTRACE_COUNT"), NULL, 0);
   if (getenv("MTRACE_GPU_ID"))
      gpu_id = strtoull(getenv("MTRACE_GPU_ID"), NULL, 0);
   raw_dump = getenv("MTRACE_RAW") && !strcmp(getenv("MTRACE_RAW"), "1");
   if (getenv("MTRACE_REDUMP"))
      redump_delay = strtoull(getenv("MTRACE_REDUMP"), NULL, 0);
   if (getenv("MTRACE_AFTER")) {
      after_s = strtod(getenv("MTRACE_AFTER"), NULL);
      from = UINT64_MAX;
   }
   if (!getenv("PANDECODE_DUMP_FILE")) {
      snprintf(path, sizeof(path), "%s/pandecode", d);
      setenv("PANDECODE_DUMP_FILE", path, 1);
   }
   pd = pandecode_create_context(false);
   pandecode_set_disassemble(pd, disasm);
   signal(SIGABRT, on_abort);
   if (tlog) {
      fprintf(tlog, "mtrace: pid %d, decode submits %" PRIu64 "..%" PRIu64 ", gpu id 0x%" PRIx64 "\n",
              getpid(), from, from + count - 1, gpu_id);
      fflush(tlog);
   }
}

static void
add_region(void *cpu, uint64_t gpu, size_t size, bool copy)
{
   if (nregions == cap) {
      cap = cap ? 2 * cap : 256;
      regions = realloc(regions, cap * sizeof(*regions));
   }
   regions[nregions++] = (struct region){cpu, gpu, size, copy};
   pandecode_inject_mmap(pd, gpu, cpu, (unsigned)size, NULL);
}

/* Drop kept copies a new mapping overlaps: the GPU range was reused. */
static void
drop_overlapping_copies(uint64_t gpu, size_t size)
{
   for (size_t i = 0; i < nregions;) {
      struct region *r = &regions[i];
      if (r->copy && r->gpu < gpu + size && gpu < r->gpu + r->size) {
         pandecode_inject_free(pd, r->gpu, (unsigned)r->size);
         free(r->cpu);
         copied -= r->size;
         regions[i] = regions[--nregions];
      } else {
         i++;
      }
   }
}

int
open(const char *path, int flags, ...)
{
   resolve();
   va_list ap;
   va_start(ap, flags);
   mode_t mode = (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
   va_end(ap);
   int fd = real_open(path, flags, mode);
   if (fd >= 0 && path && !strcmp(path, "/dev/mali0")) {
      mali_fd = fd;
      if (tlog)
         fprintf(tlog, "open /dev/mali0 -> %d\n", fd);
   }
   return fd;
}

int open64(const char *path, int flags, ...) __attribute__((alias("open")));

int
openat(int dirfd, const char *path, int flags, ...)
{
   resolve();
   va_list ap;
   va_start(ap, flags);
   mode_t mode = (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
   va_end(ap);
   int fd = real_openat(dirfd, path, flags, mode);
   if (fd >= 0 && path && !strcmp(path, "/dev/mali0")) {
      mali_fd = fd;
      if (tlog)
         fprintf(tlog, "openat /dev/mali0 -> %d\n", fd);
   }
   return fd;
}

int openat64(int dirfd, const char *path, int flags, ...) __attribute__((alias("openat")));

void *
mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
   resolve();
   void *p = real_mmap(addr, len, prot, flags, fd, off);
   if (p != MAP_FAILED && fd >= 0 && fd == mali_fd && (prot & PROT_READ)) {
      const uint64_t o = (uint64_t)off;
      uint64_t gpu = 0;
      if (o >= KB_MEM_COOKIE_BASE && o < KB_MEM_FIRST_FREE_ADDRESS)
         gpu = (uint64_t)(uintptr_t)p; /* SAME_VA */
      else if (o >= KB_MEM_FIRST_FREE_ADDRESS)
         gpu = o;
      if (gpu) {
         pthread_mutex_lock(&lock);
         drop_overlapping_copies(gpu, len);
         add_region(p, gpu, len, false);
         if (tlog)
            fprintf(tlog, "mmap off 0x%" PRIx64 " -> cpu %p gpu 0x%" PRIx64 "..0x%" PRIx64 " (%zu) prot %d\n",
                    o, p, gpu, gpu + len, len, prot);
         pthread_mutex_unlock(&lock);
      }
   }
   return p;
}

void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off)
   __attribute__((alias("mmap")));

int
munmap(void *addr, size_t len)
{
   resolve();
   const uintptr_t lo = (uintptr_t)addr, hi = lo + len;
   pthread_mutex_lock(&lock);
   for (size_t i = 0; i < nregions; i++) {
      struct region *r = &regions[i];
      const uintptr_t rlo = (uintptr_t)r->cpu, rhi = rlo + r->size;
      if (r->copy || rhi <= lo || hi <= rlo)
         continue;
      /* Keep what it held: shaders and descriptors outlive the CPU map (up
       * to 512 MiB of copies in all). Read through process_vm_readv, which
       * reports pages it cannot read instead of faulting (the driver may
       * have unmapped part of the region already, or the whole of it). */
      void *c = copied + r->size <= (512ull << 20) ? malloc(r->size) : NULL;
      ssize_t got = -1;
      if (c) {
         struct iovec local = {c, r->size}, remote = {r->cpu, r->size};
         got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
         if (got < (ssize_t)r->size) {
            const size_t ok = got < 0 ? 0 : (size_t)got;
            memset((uint8_t *)c + ok, 0, r->size - ok);
            if (tlog)
               fprintf(tlog, "munmap %p+%zu: region %p+%zu copied %zd bytes\n", addr, len, r->cpu,
                       r->size, got);
         }
      }
      if (c) {
         copied += r->size;
         r->cpu = c;
         r->copy = true;
         pandecode_inject_mmap(pd, r->gpu, c, (unsigned)r->size, NULL);
      } else {
         pandecode_inject_free(pd, r->gpu, (unsigned)r->size);
         regions[i--] = regions[--nregions];
      }
   }
   pthread_mutex_unlock(&lock);
   return real_munmap(addr, len);
}

static void
flag_names(uint64_t f, char *out, size_t n)
{
   static const struct { uint64_t bit; const char *name; } bits[] = {
      {1ull << 0, "CPU_RD"},        {1ull << 1, "CPU_WR"},        {1ull << 2, "GPU_RD"},
      {1ull << 3, "GPU_WR"},        {1ull << 4, "GPU_EX"},        {1ull << 6, "SAME_4GB"},
      {1ull << 8, "FIXED"},         {1ull << 9, "GROW_ON_GPF"},   {1ull << 10, "COH_SYSTEM"},
      {1ull << 11, "COH_LOCAL"},    {1ull << 12, "CACHED_CPU"},   {1ull << 13, "SAME_VA"},
      {1ull << 14, "NEED_MMAP"},    {1ull << 15, "COH_SYS_REQ"},  {1ull << 16, "PROTECTED"},
      {1ull << 17, "DONT_NEED"},    {1ull << 18, "IMPORT_SHARED"}, {1ull << 20, "TILER_ALIGN_TOP"},
      {1ull << 21, "UNCACHED_GPU"}, {1ull << 26, "SYNC_ON_MAP"},  {1ull << 28, "KERNEL_SYNC"},
   };
   size_t len = 0;
   out[0] = 0;
   for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]) && len + 20 < n; i++)
      if (f & bits[i].bit)
         len += snprintf(out + len, n - len, "%s%s", len ? "|" : "", bits[i].name);
   if ((f >> 22) & 0xf)
      snprintf(out + len, n - len, "%sgroup%u", len ? "|" : "", (unsigned)((f >> 22) & 0xf));
}

/* MEM_ALLOC and MEM_ALLOC_EX: the request, then what the kernel gave. */
static int
on_alloc(int fd, unsigned long req, void *arg, bool ex)
{
   struct {
      uint64_t va_pages, commit_pages, extension, flags, fixed_address;
   } in;
   memcpy(&in, arg, ex ? sizeof(in) : 4 * sizeof(uint64_t));
   if (!ex)
      in.fixed_address = 0;
   const int ret = real_ioctl(fd, req, arg);
   struct { uint64_t flags, gpu_va; } out;
   memcpy(&out, arg, sizeof(out));
   if (tlog) {
      char names[256];
      flag_names(in.flags, names, sizeof(names));
      pthread_mutex_lock(&lock);
      fprintf(tlog, "alloc%s: va %" PRIu64 " pages, commit %" PRIu64 ", ext %" PRIu64
              ", flags 0x%" PRIx64 " (%s)%s -> ret %d, gpu 0x%" PRIx64 ", out flags 0x%" PRIx64 "\n",
              ex ? "_ex" : "", in.va_pages, in.commit_pages, in.extension, in.flags, names,
              in.fixed_address ? " fixed" : "", ret, ret ? 0 : out.gpu_va, ret ? 0 : out.flags);
      pthread_mutex_unlock(&lock);
   }
   return ret;
}

/*
 * Raw job dumps (MTRACE_RAW=1): every job of a decoded chain as hex words,
 * with a compute job's push uniforms, uniform buffer and attribute tables,
 * and an Indexed Vertex job's varying buffer tables; the same chains again
 * MTRACE_REDUMP (4) submits later, by when the GPU has run them, to see what
 * the driver's own compute jobs wrote into them.
 */
struct pending_chain {
   uint64_t jc, due;
};
static struct pending_chain pend[1024];
static unsigned npend;

static const uint8_t *
raw_cpu(uint64_t gpu, size_t size)
{
   for (size_t i = 0; i < nregions; i++) {
      const struct region *r = &regions[i];
      if (gpu >= r->gpu && gpu + size <= r->gpu + r->size)
         return (const uint8_t *)r->cpu + (gpu - r->gpu);
   }
   return NULL;
}

static void
raw_hex(const char *what, uint64_t gpu, size_t size)
{
   const uint8_t *p = gpu ? raw_cpu(gpu, size) : NULL;
   if (!p) {
      fprintf(tlog, "    %s 0x%" PRIx64 ": not mapped\n", what, gpu);
      return;
   }
   fprintf(tlog, "    %s 0x%" PRIx64 ":", what, gpu);
   for (size_t i = 0; i < size; i += 4) {
      if (i % 32 == 0)
         fprintf(tlog, "\n     +%03zx", i);
      uint32_t w;
      memcpy(&w, p + i, 4);
      fprintf(tlog, " %08x", w);
   }
   fprintf(tlog, "\n");
}

static uint64_t
raw_u64(const uint8_t *p, size_t off)
{
   uint64_t v;
   memcpy(&v, p + off, 8);
   return v;
}

static void
raw_chain(uint64_t jc, const char *tag)
{
   uint64_t job = jc;
   for (unsigned n = 0; job && n < 4096; n++) {
      const uint8_t *h = raw_cpu(job, 32);
      if (!h) {
         fprintf(tlog, "  RAW %s job 0x%" PRIx64 " not mapped\n", tag, job);
         return;
      }
      uint32_t w4, w5;
      memcpy(&w4, h + 16, 4);
      memcpy(&w5, h + 20, 4);
      const unsigned type = (w4 >> 1) & 0x7f;
      fprintf(tlog, "  RAW %s job 0x%" PRIx64 " type %u index %u deps %u %u%s\n", tag, job, type,
              w4 >> 16, w5 & 0xffff, w5 >> 16, (w4 & (1u << 8)) ? " barrier" : "");
      const size_t size = type == 4 ? 192 : type == 10 ? 384 : type == 2 ? 64 : 32;
      raw_hex("job", job, size);
      const uint8_t *j = raw_cpu(job, size);
      if (j && type == 4) {
         raw_hex("push uniforms", raw_u64(j, 64 + 48), 256);
         raw_hex("uniform buffers", raw_u64(j, 64 + 24), 128);
         raw_hex("attribute buffers", raw_u64(j, 64 + 64), 64);
         raw_hex("attributes", raw_u64(j, 64 + 72), 32);
      } else if (j && type == 10) {
         raw_hex("fragment varying buffers", raw_u64(j, 128 + 80), 96);
         raw_hex("vertex varying buffers", raw_u64(j, 256 + 80), 96);
         raw_hex("vertex push uniforms", raw_u64(j, 256 + 48), 64);
      }
      job = raw_u64(h, 24);
   }
}

static void
on_submit(const struct kb_jm_job_submit *s)
{
   pthread_mutex_lock(&lock);
   const uint64_t n = submits++;
   if (raw_dump && tlog) {
      for (unsigned i = 0; i < npend;) {
         if (pend[i].due <= n) {
            fprintf(tlog, "post-run dump of jc 0x%" PRIx64 " at submit %" PRIu64 "\n", pend[i].jc, n);
            raw_chain(pend[i].jc, "post");
            pend[i] = pend[--npend];
         } else {
            i++;
         }
      }
   }
   /* MTRACE_AFTER: the window starts at the first submit that many seconds
    * after the first one (drivers group atoms into submits differently). */
   if (after_s >= 0) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      const double now = ts.tv_sec + ts.tv_nsec * 1e-9;
      if (!n)
         first_s = now;
      else if (from == UINT64_MAX && now - first_s >= after_s)
         from = n;
   }
   const bool decode = n >= from && n < from + count;
   if (tlog)
      fprintf(tlog, "submit %" PRIu64 ": %u atoms, stride %u%s\n", n, s->nr_atoms, s->stride,
              decode ? " (decoded)" : "");
   for (uint32_t i = 0; i < s->nr_atoms; i++) {
      struct kb_jm_atom a;
      memset(&a, 0, sizeof(a));
      memcpy(&a, (const uint8_t *)(uintptr_t)s->addr + (size_t)i * s->stride,
             s->stride < sizeof(a) ? s->stride : sizeof(a));
      if (tlog)
         fprintf(tlog, "  atom %u req 0x%x deps %u/%u %u/%u jc 0x%" PRIx64 " slot %u prio %u\n",
                 a.atom_number, a.core_req, a.pre_dep[0].atom_id, a.pre_dep[0].dependency_type,
                 a.pre_dep[1].atom_id, a.pre_dep[1].dependency_type, a.jc, a.jobslot, a.prio);
      if (decode && a.jc && !(a.core_req & KB_JM_REQ_SOFT_JOB)) {
         if (tlog) {
            fprintf(tlog, "  decoding atom %u jc 0x%" PRIx64 "\n", a.atom_number, a.jc);
            fflush(tlog);
         }
         if (raw_dump && tlog) {
            raw_chain(a.jc, "submit");
            if (npend < 1024)
               pend[npend++] = (struct pending_chain){a.jc, n + redump_delay};
         }
         pandecode_jc(pd, a.jc, gpu_id);
         if (pd->dump_stream)
            fflush(pd->dump_stream);
      }
   }
   if (decode && n == from + count - 1) {
      pandecode_next_frame(pd);
      if (tlog)
         fprintf(tlog, "mtrace: decode window done\n");
   }
   if (tlog)
      fflush(tlog);
   pthread_mutex_unlock(&lock);
}

int
ioctl(int fd, unsigned long req, ...)
{
   resolve();
   va_list ap;
   va_start(ap, req);
   void *arg = va_arg(ap, void *);
   va_end(ap);
   if (fd == mali_fd && arg) {
      const unsigned r = (unsigned)req;
      if (r == KB_JM_IOCTL_JOB_SUBMIT)
         on_submit(arg);
      else if (r == KB_IOCTL_MEM_ALLOC || r == KB_IOCTL_MEM_ALLOC_EX)
         return on_alloc(fd, req, arg, r == KB_IOCTL_MEM_ALLOC_EX);
      else if (tlog && (r == KB_IOCTL_MEM_FREE || r == KB_IOCTL_MEM_IMPORT ||
                        r == KB_IOCTL_MEM_COMMIT || r == KB_IOCTL_MEM_FLAGS_CHANGE ||
                        r == KB_IOCTL_MEM_ALIAS || r == KB_IOCTL_MEM_JIT_INIT))
         fprintf(tlog, "ioctl 0x%x%s\n", r,
                 r == KB_IOCTL_MEM_FREE     ? " MEM_FREE" :
                 r == KB_IOCTL_MEM_IMPORT   ? " MEM_IMPORT" :
                 r == KB_IOCTL_MEM_COMMIT   ? " MEM_COMMIT" :
                 r == KB_IOCTL_MEM_FLAGS_CHANGE ? " MEM_FLAGS_CHANGE" :
                 r == KB_IOCTL_MEM_ALIAS    ? " MEM_ALIAS" : " MEM_JIT_INIT");
   }
   return real_ioctl(fd, req, arg);
}

/*
 * pandecode's lookup of the mapping holding a GPU address (build.sh renames
 * the real one in decode_common.c). Without the asserts the decoder
 * dereferences a NULL mapping for an address it does not know, which would
 * take the traced process down: such an address gets a zeroed 4 MiB
 * fallback instead (pointers read from it are 0, which the decoder
 * skips), and is logged.
 */
struct pandecode_mapped_memory *
pandecode_find_mapped_gpu_mem_containing_real(struct pandecode_context *ctx, uint64_t addr);

struct pandecode_mapped_memory *
pandecode_find_mapped_gpu_mem_containing(struct pandecode_context *ctx, uint64_t addr)
{
   struct pandecode_mapped_memory *mem = pandecode_find_mapped_gpu_mem_containing_real(ctx, addr);
   if (mem)
      return mem;
   static struct pandecode_mapped_memory fallback;
   static unsigned unknown;
   if (!fallback.addr)
      fallback.addr = calloc(1, 4u << 20);
   fallback.length = 4u << 20;
   fallback.gpu_va = addr & ~((1ull << 21) - 1);
   fallback.ro = true;
   if (tlog && unknown++ < 64)
      fprintf(tlog, "  unknown GPU memory 0x%" PRIx64 " (decoded as zeros)\n", addr);
   return fallback.addr ? &fallback : NULL;
}

/* pandecode's mprotect (build.sh renames it here): a no-op. */
int mtrace_noop_mprotect(void *addr, size_t len, int prot);

int
mtrace_noop_mprotect(void *addr, size_t len, int prot)
{
   (void)addr;
   (void)len;
   (void)prot;
   return 0;
}

/* The job-chain and CSF entry points of the other architectures, which
 * decode_common.c names but a v7 trace never reaches. */
#define JC_STUB(v)                                                                           void pandecode_jc_##v(struct pandecode_context *c, uint64_t jc, uint64_t id) { abort(); }    void pandecode_abort_on_fault_##v(struct pandecode_context *c, uint64_t jc) { abort(); }
JC_STUB(v4) JC_STUB(v5) JC_STUB(v6) JC_STUB(v9)
#define CS_STUB(v)                                                                           void pandecode_interpret_cs_##v(struct pandecode_context *c, uint64_t q, uint32_t s,                                      uint64_t id, uint32_t *r) { abort(); }                    void pandecode_cs_binary_##v(struct pandecode_context *c, uint64_t b, uint32_t s) { abort(); }    void pandecode_cs_trace_##v(struct pandecode_context *c, uint64_t t, uint32_t s,                                      uint64_t id) { abort(); }
CS_STUB(v10) CS_STUB(v11) CS_STUB(v12) CS_STUB(v13) CS_STUB(v14)
