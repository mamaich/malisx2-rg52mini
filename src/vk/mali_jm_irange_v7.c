/*
 * SPDX-FileCopyrightText: 2026 mamaich
 * SPDX-License-Identifier: MIT
 */

/*
 * Index ranges on the GPU for the Bifrost (v7) draws drawn as one Indexed
 * Vertex job. Such a draw needs the smallest and largest index it uses: the
 * job's vertex count and start (Invocation, the Draw descriptors' Offset
 * start, the Primitive's base vertex offset) and the size of its varying
 * buffers follow from them. Read on the CPU while the draw records
 * (mali_jm_cmd_draw_v7.c index_range), the indices come from memory that is
 * mostly CPU-uncached, a load at a time from DRAM: 45 % of the driver's time
 * on ARMSX2's GS thread in Jak and Daxter, 8 % of the whole thread.
 *
 * Arm's driver reads them on the GPU, with Compute jobs ahead of a pass's
 * Indexed Vertex jobs that fill those jobs in (seen in its job chains on
 * God of War), and so does panvk for indirect draws (Mesa
 * src/panfrost/libpan/draw_helper.cl, Collabora, MIT: the patching below
 * follows its panlib_patch_draw). Here, per group of draws of one batch:
 *
 *  - the CPU writes a record per draw (struct mali_v7_irange_draw: where
 *    its indices and its job are, its varying buffer strides) and splits
 *    its indices into chunks of MALI_V7_IRANGE_CHUNK (struct
 *    mali_v7_irange_chunk);
 *  - a scan job, one invocation per chunk, finds each chunk's smallest and
 *    largest index (restart indices left out) and merges them into the
 *    draw's record with atomic min and max;
 *  - a patch job behind it, one invocation per draw, takes the draw's
 *    varying buffers from the batch's varying heap (an atomic add on its
 *    offset) and writes the job's vertex count and start, the varying
 *    buffer descriptors and the position address, and turns the job into
 *    a Null job when the draw has no index to shade or the heap is full. It
 *    puts the record's min and max back for the next run;
 *  - each draw's Indexed Vertex job depends on the patch job (through its
 *    vertex shader's copy jobs when it has any).
 *
 * The varying heap is a growable GPU region per tiler heap slot
 * (mali_jm_heap.c), taken with the slot: the queue already waits for the
 * slot's previous user before a batch reuses it. Its offset is a word in
 * command memory set to zero by a Write Value job when the batch takes the
 * slot; the batches a pass splits into continue from it.
 */

#include "mali_cmd_state.h"

#if PAN_ARCH >= 9
#error "mali_jm_irange_v7.c is for the Bifrost (PAN_ARCH 7) draws"
#endif

#include <string.h>

#include "nir_builder.h"

#include "mali_arch.h"

/* The jobs' push constants (FAU). */
struct irange_push {
   uint64_t draws;   /* struct mali_v7_irange_draw[] */
   uint64_t chunks;  /* struct mali_v7_irange_chunk[] */
   uint64_t counts;  /* uint32_t draws, chunks: written when the group closes */
   uint64_t counter; /* uint32_t: the varying heap's offset */
   uint64_t heap;    /* the varying heap's start */
   uint32_t heap_size;
   uint32_t pad;
};

#define IRANGE_WG 8 /* invocations per workgroup: a Bifrost warp */

#define push_field(b, bits, name)                                              \
   nir_load_push_constant(b, 1, bits, nir_imm_int(b, offsetof(struct irange_push, name)))

static nir_def *
field_addr(nir_builder *b, nir_def *base, size_t off)
{
   return nir_iadd_imm(b, base, off);
}

static nir_def *
load32(nir_builder *b, nir_def *addr)
{
   return nir_load_global(b, 1, 32, addr, .align_mul = 4);
}

static void
store32(nir_builder *b, nir_def *addr, nir_def *v)
{
   nir_store_global(b, v, addr, .align_mul = 4);
}

#define DRAW_FIELD(name) offsetof(struct mali_v7_irange_draw, name)

/* One loop over n indices of 1 << log2 bytes from addr, min and max into
 * the variables (restart indices left out when restart). */
static void
scan_loop(nir_builder *b, nir_def *addr, nir_def *n, unsigned log2, nir_def *restart,
          nir_variable *vmin, nir_variable *vmax)
{
   const unsigned bits = 8u << log2;
   const uint32_t restart_value = bits == 32 ? UINT32_MAX : (1u << bits) - 1;
   nir_variable *i = nir_local_variable_create(b->impl, glsl_uint_type(), "i");
   nir_store_var(b, i, nir_imm_int(b, 0), 1);
   nir_push_loop(b);
   {
      nir_def *iv = nir_load_var(b, i);
      nir_break_if(b, nir_uge(b, iv, n));
      nir_def *a = nir_iadd(b, addr, nir_u2u64(b, nir_ishl_imm(b, iv, log2)));
      nir_def *v = nir_load_global(b, 1, bits, a, .align_mul = 1u << log2);
      v = nir_u2u32(b, v);
      nir_def *skip = nir_iand(b, restart, nir_ieq_imm(b, v, restart_value));
      nir_def *lo = nir_load_var(b, vmin), *hi = nir_load_var(b, vmax);
      nir_store_var(b, vmin, nir_bcsel(b, skip, lo, nir_umin(b, lo, v)), 1);
      nir_store_var(b, vmax, nir_bcsel(b, skip, hi, nir_umax(b, hi, v)), 1);
      nir_store_var(b, i, nir_iadd_imm(b, iv, 1), 1);
   }
   nir_pop_loop(b, NULL);
}

static nir_shader *
build_scan_nir(struct mali_device *dev)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, MALI_PER_ARCH(shader_nir_options)(dev, MESA_SHADER_COMPUTE), "%s",
      "mali_v7_irange_scan");
   b.shader->info.workgroup_size[0] = IRANGE_WG;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;

   nir_def *id = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *counts = push_field(&b, 64, counts);
   nir_def *nchunks = load32(&b, nir_iadd_imm(&b, counts, 4));
   nir_push_if(&b, nir_ult(&b, id, nchunks));
   {
      nir_def *ch = nir_iadd(&b, push_field(&b, 64, chunks),
                             nir_u2u64(&b, nir_imul_imm(&b, id, sizeof(struct mali_v7_irange_chunk))));
      nir_def *draw = load32(&b, field_addr(&b, ch, offsetof(struct mali_v7_irange_chunk, draw)));
      nir_def *first = load32(&b, field_addr(&b, ch, offsetof(struct mali_v7_irange_chunk, first)));
      nir_def *n = load32(&b, field_addr(&b, ch, offsetof(struct mali_v7_irange_chunk, count)));
      nir_def *rec = nir_iadd(&b, push_field(&b, 64, draws),
                              nir_u2u64(&b, nir_imul_imm(&b, draw, sizeof(struct mali_v7_irange_draw))));
      nir_def *indices = nir_load_global(&b, 1, 64, field_addr(&b, rec, DRAW_FIELD(indices)),
                                         .align_mul = 8);
      nir_def *flags = load32(&b, field_addr(&b, rec, DRAW_FIELD(flags)));
      nir_def *log2 = nir_iand_imm(&b, flags, MALI_V7_IRANGE_SIZE_MASK);
      nir_def *restart = nir_ine_imm(&b, nir_iand_imm(&b, flags, MALI_V7_IRANGE_RESTART), 0);
      nir_def *addr = nir_iadd(&b, indices, nir_u2u64(&b, nir_ishl(&b, first, log2)));

      nir_variable *vmin = nir_local_variable_create(b.impl, glsl_uint_type(), "min");
      nir_variable *vmax = nir_local_variable_create(b.impl, glsl_uint_type(), "max");
      nir_store_var(&b, vmin, nir_imm_int(&b, UINT32_MAX), 1);
      nir_store_var(&b, vmax, nir_imm_int(&b, 0), 1);
      /* One loop per index size: the size is the same for the whole draw. */
      nir_push_if(&b, nir_ieq_imm(&b, log2, 1));
      scan_loop(&b, addr, n, 1, restart, vmin, vmax);
      nir_push_else(&b, NULL);
      nir_push_if(&b, nir_ieq_imm(&b, log2, 2));
      scan_loop(&b, addr, n, 2, restart, vmin, vmax);
      nir_push_else(&b, NULL);
      scan_loop(&b, addr, n, 0, restart, vmin, vmax);
      nir_pop_if(&b, NULL);
      nir_pop_if(&b, NULL);

      nir_def *lo = nir_load_var(&b, vmin), *hi = nir_load_var(&b, vmax);
      /* Nothing but restart indices: min > max, nothing to merge. */
      nir_push_if(&b, nir_uge(&b, hi, lo));
      nir_global_atomic(&b, 32, field_addr(&b, rec, DRAW_FIELD(min)), lo,
                        .atomic_op = nir_atomic_op_umin);
      nir_global_atomic(&b, 32, field_addr(&b, rec, DRAW_FIELD(max)), hi,
                        .atomic_op = nir_atomic_op_umax);
      nir_pop_if(&b, NULL);
   }
   nir_pop_if(&b, NULL);
   return b.shader;
}

/* The descriptors the patch job writes, as offsets into an Indexed Vertex
 * job (the v7.xml layout). */
#define JOB_HEADER_W4    16 /* Is 64b, Type, Barrier, ..., Index */
#define JOB_INVOCATION   32 /* Invocations: the vertex count - 1 */
#define JOB_BASE_VERTEX  (40 + 4)  /* Primitive: Base vertex offset */
#define JOB_FRAG_START   (128 + 4) /* Fragment Draw: Offset start */
#define JOB_FRAG_POS     (128 + 16) /* Fragment Draw: Position */
#define JOB_VERT_START   (256 + 4) /* Vertex Draw: Offset start */

/* More vertices than this in one draw's range: not drawn (the varying
 * sizes stay in 32 bits, and no heap holds them). */
#define IRANGE_MAX_RANGE (1u << 22)

static nir_shader *
build_patch_nir(struct mali_device *dev)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, MALI_PER_ARCH(shader_nir_options)(dev, MESA_SHADER_COMPUTE), "%s",
      "mali_v7_irange_patch");
   b.shader->info.workgroup_size[0] = IRANGE_WG;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;

   nir_def *id = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *counts = push_field(&b, 64, counts);
   nir_def *ndraws = load32(&b, counts);
   nir_push_if(&b, nir_ult(&b, id, ndraws));
   {
      nir_def *rec = nir_iadd(&b, push_field(&b, 64, draws),
                              nir_u2u64(&b, nir_imul_imm(&b, id, sizeof(struct mali_v7_irange_draw))));
      nir_def *lo = load32(&b, field_addr(&b, rec, DRAW_FIELD(min)));
      nir_def *hi = load32(&b, field_addr(&b, rec, DRAW_FIELD(max)));
      /* Ready for the next run of the command buffer. */
      store32(&b, field_addr(&b, rec, DRAW_FIELD(min)), nir_imm_int(&b, UINT32_MAX));
      store32(&b, field_addr(&b, rec, DRAW_FIELD(max)), nir_imm_int(&b, 0));
      nir_def *job = nir_load_global(&b, 1, 64, field_addr(&b, rec, DRAW_FIELD(job)),
                                     .align_mul = 8);
      nir_def *w4_null = load32(&b, field_addr(&b, rec, DRAW_FIELD(w4_null)));
      nir_def *w4_idvs = load32(&b, field_addr(&b, rec, DRAW_FIELD(w4_idvs)));

      nir_def *range = nir_iadd_imm(&b, nir_isub(&b, hi, lo), 1);
      nir_def *ok = nir_iand(&b, nir_uge(&b, hi, lo),
                             nir_uge(&b, nir_imm_int(&b, IRANGE_MAX_RANGE), range));
      nir_def *size[MALI_V7_VARY_BUF_COUNT];
      nir_def *total = nir_imm_int(&b, 0);
      for (unsigned i = 0; i < MALI_V7_VARY_BUF_COUNT; i++) {
         nir_def *stride = load32(&b, field_addr(&b, rec, DRAW_FIELD(stride) + 4 * i));
         size[i] = nir_imul(&b, nir_bcsel(&b, ok, range, nir_imm_int(&b, 0)), stride);
         /* Each buffer 64-byte aligned, as command memory gave them. */
         total = nir_iadd(&b, total, nir_iand_imm(&b, nir_iadd_imm(&b, size[i], 63), ~63u));
      }
      nir_def *off = nir_imm_int(&b, 0);
      nir_push_if(&b, ok);
      nir_def *got = nir_global_atomic(&b, 32, push_field(&b, 64, counter), total,
                                       .atomic_op = nir_atomic_op_iadd);
      nir_pop_if(&b, NULL);
      off = nir_if_phi(&b, got, off);
      nir_def *end = nir_iadd(&b, off, total);
      ok = nir_iand(&b, ok, nir_iand(&b, nir_uge(&b, end, off),
                                     nir_uge(&b, push_field(&b, 32, heap_size), end)));

      nir_push_if(&b, nir_inot(&b, ok));
      {
         /* Nothing to shade, or no room: a Null job (panlib_patch_draw). */
         store32(&b, nir_iadd_imm(&b, job, JOB_HEADER_W4), w4_null);
      }
      nir_push_else(&b, NULL);
      {
         store32(&b, nir_iadd_imm(&b, job, JOB_HEADER_W4), w4_idvs);
         nir_def *table = nir_load_global(&b, 1, 64, field_addr(&b, rec, DRAW_FIELD(vary_table)),
                                          .align_mul = 8);
         nir_def *p = nir_iadd(&b, push_field(&b, 64, heap), nir_u2u64(&b, off));
         for (unsigned i = 0; i < MALI_V7_VARY_BUF_COUNT; i++) {
            nir_def *entry = nir_iadd_imm(&b, table, i * pan_size(ATTRIBUTE_BUFFER));
            nir_push_if(&b, nir_ine_imm(&b, size[i], 0));
            /* Attribute Buffer: Type 1D in bits 0-5, the 64-byte aligned
             * pointer above; Size in word 3. */
            nir_store_global(&b, nir_ior_imm(&b, p, MALI_ATTRIBUTE_TYPE_1D), entry,
                             .align_mul = 8);
            store32(&b, nir_iadd_imm(&b, entry, 12), size[i]);
            if (i == MALI_V7_VARY_BUF_POSITION)
               nir_store_global(&b, p, nir_iadd_imm(&b, job, JOB_FRAG_POS), .align_mul = 8);
            nir_pop_if(&b, NULL);
            p = nir_iadd(&b, p, nir_u2u64(&b, nir_iand_imm(&b, nir_iadd_imm(&b, size[i], 63),
                                                            ~63u)));
         }
         /* panvk's patching for one instance: Invocation (1, range, 1)
          * workgroups of one invocation, whose second word (shifts and
          * split) the CPU wrote and does not change with the range. */
         store32(&b, nir_iadd_imm(&b, job, JOB_INVOCATION), nir_iadd_imm(&b, range, -1));
         nir_def *vo = load32(&b, field_addr(&b, rec, DRAW_FIELD(vertex_offset)));
         nir_def *start = nir_iadd(&b, lo, vo);
         store32(&b, nir_iadd_imm(&b, job, JOB_BASE_VERTEX), nir_ineg(&b, lo));
         store32(&b, nir_iadd_imm(&b, job, JOB_FRAG_START), start);
         store32(&b, nir_iadd_imm(&b, job, JOB_VERT_START), start);
      }
      nir_pop_if(&b, NULL);
   }
   nir_pop_if(&b, NULL);
   return b.shader;
}

/* A Compute job running one of the two shaders, IRANGE_WG invocations per
 * workgroup, behind job dep; *job: its CPU view (for the Invocation the
 * group's close writes). */
static bool
add_job(struct mali_cmd_buffer *cmd, struct mali_jm_chain *c, const struct mali_shader *s,
        const struct irange_push *push, uint16_t dep, bool suppress_prefetch, void **job,
        uint16_t *index)
{
   const uint32_t base[3] = {0, 0, 0};
   const uint32_t groups[3] = {1, 1, 1};
   const uint32_t local[3] = {IRANGE_WG, 1, 1};
   const uint64_t fau =
      mali_compute_fau(cmd, s, NULL, push, sizeof(*push), base, groups) & BITFIELD64_MASK(56);
   const uint64_t tsd = mali_v7_copy_tsd(cmd, s, groups);
   if (vk_command_buffer_has_error(&cmd->vk) || !tsd || (s->fau.total_count && !fau))
      return false;

   struct mali_ptr j =
      mali_jm_cmd_add_job(cmd, c, MALI_JOB_TYPE_COMPUTE, pan_size(COMPUTE_JOB), false, dep);
   if (!j.cpu)
      return false;
   if (suppress_prefetch) {
      /* The job manager must not read ahead the jobs this one writes. */
      uint32_t *w4 = (uint32_t *)((uint8_t *)j.cpu + JOB_HEADER_W4);
      *w4 |= 1u << 11;
   }
   mali_v7_compute_invocation(pan_section_ptr(j.cpu, COMPUTE_JOB, INVOCATION), local, groups);
   pan_section_pack(j.cpu, COMPUTE_JOB, PARAMETERS, cfg) {
      cfg.job_task_split = mali_v7_compute_task_split(local);
   }
   pan_section_pack(j.cpu, COMPUTE_JOB, DRAW, cfg) {
      cfg.state = s->spd[MALI_SPD_MAIN];
      cfg.push_uniforms = fau;
      cfg.thread_storage = tsd;
   }
   *job = j.cpu;
   *index = c->index;
   return true;
}

void
mali_v7_irange_close(struct mali_cmd_buffer *cmd)
{
   struct mali_v7_irange *g = &cmd->jm.irange;
   if (!g->scan_job)
      return;
   g->counts[0] = g->ndraws;
   g->counts[1] = g->nchunks;
   const uint32_t local[3] = {IRANGE_WG, 1, 1};
   const uint32_t scan_groups[3] = {MAX2(DIV_ROUND_UP(g->nchunks, IRANGE_WG), 1), 1, 1};
   const uint32_t patch_groups[3] = {MAX2(DIV_ROUND_UP(g->ndraws, IRANGE_WG), 1), 1, 1};
   mali_v7_compute_invocation(pan_section_ptr(g->scan_job, COMPUTE_JOB, INVOCATION), local,
                              scan_groups);
   mali_v7_compute_invocation(pan_section_ptr(g->patch_job, COMPUTE_JOB, INVOCATION), local,
                              patch_groups);
   memset(g, 0, sizeof(*g));
}

bool
mali_v7_irange_begin(struct mali_cmd_buffer *cmd, struct mali_jm_chain *c, uint32_t chunks,
                     uint16_t *patch_index)
{
   struct mali_v7_irange *g = &cmd->jm.irange;
   assert(chunks <= MALI_V7_IRANGE_MAX_CHUNKS);
   if (g->scan_job &&
       (g->ndraws == MALI_V7_IRANGE_MAX_DRAWS || g->nchunks + chunks > MALI_V7_IRANGE_MAX_CHUNKS))
      mali_v7_irange_close(cmd);
   if (g->scan_job) {
      *patch_index = g->patch_index;
      return true;
   }

   struct mali_device *dev = cmd->dev;
   struct mali_jm_batch *b = &cmd->jm.cur;
   uint64_t heap = 0, heap_size = 0;
   if (!mali_jm_vary_heap(cmd, b, &heap, &heap_size))
      return false;
   const struct mali_shader *scan =
      mali_v7_copy_shader(cmd, &dev->jm->v7_irange_scan, build_scan_nir, "mali-v7-irange-scan");
   const struct mali_shader *patch =
      mali_v7_copy_shader(cmd, &dev->jm->v7_irange_patch, build_patch_nir, "mali-v7-irange-patch");
   if (!scan || !patch)
      return false;

   /* The heap offset: zeroed once per heap slot the batch took (a pass
    * split into batches keeps counting). */
   uint16_t dep = 0;
   if (!b->vary_counter) {
      struct mali_ptr cnt = mali_cmd_alloc(cmd, 8, 8);
      if (!cnt.cpu ||
          !mali_jm_cmd_write_value(cmd, c, MALI_WRITE_VALUE_TYPE_IMMEDIATE_32, cnt.gpu, 0, false))
         return false;
      b->vary_counter = cnt.gpu;
      dep = c->index;
   }

   const size_t draws_size = MALI_V7_IRANGE_MAX_DRAWS * sizeof(struct mali_v7_irange_draw);
   const size_t chunks_size = MALI_V7_IRANGE_MAX_CHUNKS * sizeof(struct mali_v7_irange_chunk);
   struct mali_ptr m = mali_cmd_alloc(cmd, 64 + draws_size + chunks_size, 64);
   if (!m.cpu)
      return false;
   g->counts = m.cpu;
   g->draws = (struct mali_v7_irange_draw *)((uint8_t *)m.cpu + 64);
   g->chunks = (struct mali_v7_irange_chunk *)((uint8_t *)m.cpu + 64 + draws_size);
   const struct irange_push push = {
      .draws = m.gpu + 64,
      .chunks = m.gpu + 64 + draws_size,
      .counts = m.gpu,
      .counter = b->vary_counter,
      .heap = heap,
      .heap_size = (uint32_t)MIN2(heap_size, UINT32_MAX),
   };
   uint16_t scan_index;
   if (!add_job(cmd, c, scan, &push, dep, false, &g->scan_job, &scan_index) ||
       !add_job(cmd, c, patch, &push, scan_index, true, &g->patch_job, &g->patch_index)) {
      memset(g, 0, sizeof(*g));
      return false;
   }
   *patch_index = g->patch_index;
   return true;
}

void
mali_v7_irange_add(struct mali_cmd_buffer *cmd, const struct mali_v7_irange_draw *draw,
                   uint32_t count)
{
   struct mali_v7_irange *g = &cmd->jm.irange;
   assert(g->scan_job && g->ndraws < MALI_V7_IRANGE_MAX_DRAWS);
   const uint32_t d = g->ndraws++;
   struct mali_v7_irange_draw *rec = &g->draws[d];
   memcpy(rec, draw, sizeof(*draw));
   rec->min = UINT32_MAX;
   rec->max = 0;
   for (uint32_t first = 0; first < count; first += MALI_V7_IRANGE_CHUNK) {
      assert(g->nchunks < MALI_V7_IRANGE_MAX_CHUNKS);
      g->chunks[g->nchunks++] = (struct mali_v7_irange_chunk){
         .draw = d,
         .first = first,
         .count = MIN2(MALI_V7_IRANGE_CHUNK, count - first),
      };
   }
   /* A run that ends before the patch job (a fault) leaves min and max
    * changed: put them back for a re-submission. */
   static const uint32_t init[2] = {UINT32_MAX, 0};
   mali_jm_cmd_note_reset(cmd, &rec->min, init, sizeof(init));
}
