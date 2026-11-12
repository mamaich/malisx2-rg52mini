/*
 * mhwc <delay-s> <secs> - Mali hardware counters, whole GPU, through kbase's kinstr_prfcnt
 * interface (/dev/mali0; the RG52 Mini's g25p0 kbase has no vinstr reader). Waits delay-s, then
 * samples once a second for secs and prints the totals: "T <ns> <gpu cycles>" and
 * "C <block type> <block index> <counter index> <value>" for every counter that is not zero.
 * Block types: 0 front end (job manager), 1 tiler, 2 memory system (L2), 3 shader core; counter
 * names by index are in Mesa's src/panfrost/perf/database/hardwarelayout/Mali-G52.xml.
 * Works the same whichever Vulkan driver runs: the counters belong to the GPU, not to a context.
 *
 * The structures are those of the kernel's uapi/gpu/arm/bifrost/mali_kbase_hwcnt_reader.h.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define FLEX(t, s) ((uint16_t)((((t) & 0xf) << 12) | ((s) & 0xfff)))
#define NONE FLEX(0, 0)
#define ENUM_BLOCK FLEX(0, 0)
#define ENUM_SAMPLE_INFO FLEX(0, 2)
#define REQ_MODE FLEX(1, 0)
#define REQ_ENABLE FLEX(1, 1)
#define REQ_SCOPE FLEX(1, 2)
#define META_SAMPLE FLEX(2, 0)
#define META_CLOCK FLEX(2, 1)
#define META_BLOCK FLEX(2, 2)

struct hdr { uint16_t type, version; };

struct enum_item {
   struct hdr hdr;
   uint8_t pad[4];
   union {
      struct { uint8_t block_type, set, pad[2]; uint16_t num_instances, num_values; uint64_t mask[2]; } block;
      struct { uint32_t num_clock_domains, pad; } sample_info;
      uint8_t raw[24];
   } u;
};

struct req_item {
   struct hdr hdr;
   uint8_t pad[4];
   union {
      struct { uint8_t mode, pad[7]; uint64_t period_ns; } mode;
      struct { uint8_t block_type, set, pad[6]; uint64_t mask[2]; } enable;
      struct { uint8_t scope, pad[7]; } scope;
      uint8_t raw[24];
   } u;
};

struct meta {
   struct hdr hdr;
   uint8_t pad[4];
   union {
      struct { uint64_t ts_start, ts_end, seq, user_data; uint32_t flags, pad; } sample;
      struct { uint32_t num_domains, pad; uint64_t cycles[4]; } clock;
      struct { uint8_t block_type, block_idx, set, pad; uint32_t state, values_offset, pad2; } block;
   } u;
};

struct cmd { uint16_t cmd, pad[3]; uint64_t user_data; };
struct access { uint64_t seq, offset; };

#define KTYPE 0x80
struct version { uint16_t major, minor; };
#define IOCTL_VERSION_CHECK _IOWR(KTYPE, 0, struct version)
struct enum_info { uint32_t item_size, item_count; uint64_t list; };
#define IOCTL_ENUM _IOWR(KTYPE, 56, struct enum_info)
union setup { struct { uint32_t count, size; uint64_t ptr; } in; struct { uint32_t meta_size, mmap_size; } out; };
#define IOCTL_SETUP _IOWR(KTYPE, 57, union setup)
#define RTYPE 0xBF
#define IOCTL_CMD _IOW(RTYPE, 0x00, struct cmd)
#define IOCTL_GET _IOR(RTYPE, 0x01, struct access)
#define IOCTL_PUT _IOW(RTYPE, 0x10, struct access)

#define MAXB 64
static uint64_t total[4][MAXB][64];
static uint64_t total_ns, total_cycles;

/* A mark in the ftrace buffer, so the flips of the window can be counted. */
static void
mark(const char *what)
{
   int m = open("/sys/kernel/tracing/trace_marker", O_WRONLY | O_CLOEXEC);
   if (m >= 0) {
      write(m, what, strlen(what));
      close(m);
   }
}

static int
take(int fd, uint8_t *map, int keep)
{
   struct cmd c = {.cmd = 3}; /* SAMPLE_SYNC */
   if (ioctl(fd, IOCTL_CMD, &c) < 0)
      return -1;
   struct access a;
   if (ioctl(fd, IOCTL_GET, &a) < 0)
      return -1;
   for (struct meta *m = (struct meta *)(map + a.offset); m->hdr.type != NONE; m++) {
      if (!keep)
         continue;
      if (m->hdr.type == META_SAMPLE)
         total_ns += m->u.sample.ts_end - m->u.sample.ts_start;
      else if (m->hdr.type == META_CLOCK && m->u.clock.num_domains)
         total_cycles += m->u.clock.cycles[0];
      else if (m->hdr.type == META_BLOCK && m->u.block.block_type < 4 && m->u.block.block_idx < MAXB) {
         const uint64_t *v = (const uint64_t *)(map + m->u.block.values_offset);
         for (int i = 0; i < 64; i++)
            total[m->u.block.block_type][m->u.block.block_idx][i] += v[i];
      }
   }
   return ioctl(fd, IOCTL_PUT, &a);
}

int
main(int argc, char **argv)
{
   if (argc < 3) {
      fprintf(stderr, "usage: mhwc <delay-s> <secs>\n");
      return 2;
   }
   const int delay = atoi(argv[1]), secs = atoi(argv[2]);
   int dev = open("/dev/mali0", O_RDWR | O_CLOEXEC);
   if (dev < 0) {
      perror("/dev/mali0");
      return 1;
   }
   struct version ver = {11, 36};
   ioctl(dev, IOCTL_VERSION_CHECK, &ver);

   struct enum_info ei = {0};
   if (ioctl(dev, IOCTL_ENUM, &ei) < 0) {
      perror("ENUM_INFO");
      return 1;
   }
   struct enum_item *items = calloc(ei.item_count, ei.item_size);
   ei.list = (uintptr_t)items;
   if (ioctl(dev, IOCTL_ENUM, &ei) < 0) {
      perror("ENUM_INFO list");
      return 1;
   }
   struct req_item req[16];
   unsigned n = 0;
   memset(req, 0, sizeof(req));
   req[n].hdr.type = REQ_MODE;
   req[n++].u.mode.mode = 0; /* manual */
   req[n].hdr.type = REQ_SCOPE;
   req[n++].u.scope.scope = 0; /* global */
   for (unsigned i = 0; i < ei.item_count; i++) {
      struct enum_item *it = (struct enum_item *)((uint8_t *)items + i * ei.item_size);
      if (it->hdr.type == ENUM_BLOCK && it->u.block.set == 0 && n < 15) {
         fprintf(stderr, "block type %u: %u instances, %u values\n", it->u.block.block_type,
                 it->u.block.num_instances, it->u.block.num_values);
         req[n].hdr.type = REQ_ENABLE;
         req[n].u.enable.block_type = it->u.block.block_type;
         req[n].u.enable.set = 0;
         req[n].u.enable.mask[0] = req[n].u.enable.mask[1] = ~0ull;
         n++;
      }
   }
   req[n++].hdr.type = NONE;
   union setup s = {.in = {n, sizeof(struct req_item), (uintptr_t)req}};
   int fd = ioctl(dev, IOCTL_SETUP, &s);
   if (fd < 0) {
      perror("SETUP");
      return 1;
   }
   uint8_t *map = mmap(NULL, s.out.mmap_size, PROT_READ, MAP_SHARED, fd, 0);
   if (map == MAP_FAILED) {
      perror("mmap");
      return 1;
   }
   sleep(delay);
   struct cmd c = {.cmd = 1}; /* START */
   if (ioctl(fd, IOCTL_CMD, &c) < 0) {
      perror("START");
      return 1;
   }
   take(fd, map, 0);
   mark("mhwc start");
   for (int i = 0; i < secs; i++) {
      sleep(1);
      if (take(fd, map, 1) < 0) {
         perror("sample");
         return 1;
      }
   }
   mark("mhwc end");
   c.cmd = 2; /* STOP */
   ioctl(fd, IOCTL_CMD, &c);
   printf("T %llu %llu\n", (unsigned long long)total_ns, (unsigned long long)total_cycles);
   for (int t = 0; t < 4; t++)
      for (int b = 0; b < MAXB; b++)
         for (int i = 0; i < 64; i++)
            if (total[t][b][i])
               printf("C %d %d %d %llu\n", t, b, i, (unsigned long long)total[t][b][i]);
   return 0;
}
