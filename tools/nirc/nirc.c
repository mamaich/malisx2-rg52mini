/*
 * nirc: rerun Mesa's panfrost compiler off the device on shaders dumped by
 * malisx2's MALISX2_NIR_DUMP (src/compiler/mali_compiler.c). Built by
 * build.sh against the native (x86-64) build of the same malisx2 + mesa
 * tree, so the compiler is the one under test.
 *
 *   nirc file.mxnir...
 *
 * One line per shader: wall time of pan_shader_compile(), the size and an
 * FNV-1a hash of the binary (to tell identical code), and the stats of the
 * shader (and of the IDVS varying shader), as the shader-db line has them. Compiler debug knobs (BIFROST_MESA_DEBUG, BI_RA_*) come from the
 * environment as on the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_serialize.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/model/pan_model.h"
#include "util/blob.h"
#include "util/os_file.h"
#include "util/u_dynarray.h"

struct mali_nir_dump {
   char magic[8];
   uint32_t inputs_size, model_size, layout_size, info_size, nir_size;
};

static double
now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void
print_stats(const char *what, const struct pan_stats *s)
{
   if (s->isa != PAN_STAT_BIFROST)
      return;
   const struct bifrost_stats *b = &s->bifrost;
   printf(" | %s %u instrs %u clauses %.1f cyc %u regs %u thr %u:%u sp:fi %u cost", what,
          b->instrs, b->clauses, b->cycles, b->registers_used, b->threads, b->spills,
          b->fills, b->spill_cost);
}

static int
compile_one(const char *path)
{
   size_t size = 0;
   char *data = os_read_file(path, &size);
   if (!data || size < sizeof(struct mali_nir_dump)) {
      fprintf(stderr, "%s: cannot read\n", path);
      return 1;
   }
   struct mali_nir_dump hdr;
   memcpy(&hdr, data, sizeof(hdr));
   if (memcmp(hdr.magic, "MXNIR01", 8) || hdr.inputs_size != sizeof(struct pan_compile_inputs) ||
       (hdr.model_size && hdr.model_size != sizeof(struct pan_model)) ||
       (hdr.layout_size && hdr.layout_size != sizeof(struct pan_varying_layout)) ||
       hdr.info_size != sizeof(struct pan_shader_info) ||
       sizeof(hdr) + hdr.inputs_size + hdr.model_size + hdr.layout_size + hdr.info_size +
             hdr.nir_size != size) {
      fprintf(stderr, "%s: not a dump of this build\n", path);
      free(data);
      return 1;
   }

   const char *p = data + sizeof(hdr);
   struct pan_compile_inputs inputs;
   struct pan_model model;
   struct pan_varying_layout layout;
   struct pan_shader_info info;
   memcpy(&inputs, p, sizeof(inputs));
   p += sizeof(inputs);
   if (hdr.model_size) {
      memcpy(&model, p, sizeof(model));
      p += sizeof(model);
      model.name = "dump";
      model.performance_counters = NULL;
      inputs.model = &model;
   } else {
      inputs.model = NULL;
   }
   if (hdr.layout_size) {
      memcpy(&layout, p, sizeof(layout));
      p += sizeof(layout);
      inputs.varying_layout = &layout;
   } else {
      inputs.varying_layout = NULL;
   }
   memcpy(&info, p, sizeof(info));
   p += sizeof(info);

   struct blob_reader blob;
   blob_reader_init(&blob, p, hdr.nir_size);
   /* The options depend on the stage, which is inside the blob:
    * deserialize with the vertex ones, then set the stage's. */
   nir_shader *nir = nir_deserialize(NULL, pan_get_nir_shader_compiler_options(inputs.arch,
                                                                               MESA_SHADER_VERTEX,
                                                                               false),
                                     &blob);
   if (!nir || blob.overrun) {
      fprintf(stderr, "%s: bad NIR blob\n", path);
      free(data);
      return 1;
   }
   nir->options = pan_get_nir_shader_compiler_options(inputs.arch, nir->info.stage, false);

   /* The IDVS split was decided before the dump (bi_should_idvs in the
    * driver's preprocessing), so BIFROST_MESA_DEBUG=noidvs does not reach
    * it here. NIRC_NOIDVS=1 compiles a split vertex shader whole, close to
    * what MALISX2_V7_IDVS=0 gives on the device: the outputs were lowered
    * with a load_shader_output_pan guard per bucket, which the compiler
    * inlines per half; for the whole shader every bucket is written. */
   if (getenv("NIRC_NOIDVS") && nir->info.stage == MESA_SHADER_VERTEX && info.vs.idvs) {
      info.vs.idvs = false;
      nir_inline_sysval(nir, nir_intrinsic_load_shader_output_pan, 0xffffffff);
      bool progress = true;
      while (progress) {
         progress = false;
         progress |= nir_opt_constant_folding(nir);
         progress |= nir_opt_dce(nir);
         progress |= nir_opt_dead_cf(nir);
         progress |= nir_opt_cse(nir);
      }
   }

   /* Experiment: NIRC_SPEC=<limit> if-converts the small ifs around UBO
    * loads with a computed offset (PICA's relative uniform reads in Azahar:
    * "if (index < 96) v = c[index]"), taking the loads as safe to run
    * unconditionally. Only to measure what flatter code would give; the
    * driver would have to make such a load safe itself (clamp the offset). */
   const char *spec = getenv("NIRC_SPEC");
   const bool guard = getenv("NIRC_GUARD") && atoi(getenv("NIRC_GUARD"));
   bool skip = false;
   if (spec && *spec && guard && nir->info.stage == MESA_SHADER_VERTEX) {
      /* As the driver (mali_shader.c other_indirect_loads): no other loads
       * with a computed offset that the flattening would take as safe. */
      nir_foreach_function_impl(impl, nir) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
               switch (intr->intrinsic) {
               case nir_intrinsic_load_deref:
                  skip = true;
                  break;
               case nir_intrinsic_load_ubo_vec4:
               case nir_intrinsic_load_interpolated_input:
               case nir_intrinsic_load_per_vertex_input:
               case nir_intrinsic_load_input_vertex:
                  skip |= !nir_src_is_const(intr->src[1]);
                  break;
               case nir_intrinsic_load_global_constant:
               case nir_intrinsic_load_input:
               case nir_intrinsic_load_constant:
                  skip |= !nir_src_is_const(intr->src[0]);
                  break;
               default:
                  break;
               }
            }
         }
      }
      if (skip)
         fprintf(stderr, "nirc: other indirect loads, not flattened\n");
   }
   if (spec && *spec && !skip && (!guard || nir->info.stage == MESA_SHADER_VERTEX)) {
      nir_opt_peephole_select_options o = {
         .limit = (unsigned)atoi(spec),
         .indirect_load_ok = true,
         .expensive_alu_ok = true,
      };
      bool progress = true;
      while (progress) {
         progress = false;
         /* NIRC_GUARD=1: a load in an if branch reads offset 0 unless the
          * if takes that branch: bcsel(cond, offset, 0), which is the same
          * value there, so a load moved out of the if stays in bounds. */
         nir_foreach_function_impl(impl, nir) {
            nir_builder b = nir_builder_create(impl);
            nir_foreach_block(block, impl) {
               nir_cf_node *parent = block->cf_node.parent;
               nir_if *nif = parent && parent->type == nir_cf_node_if ? nir_cf_node_as_if(parent) : NULL;
               const bool then_side = nif && block == nir_if_first_then_block(nif) &&
                                      block == nir_if_last_then_block(nif);
               const bool else_side = nif && block == nir_if_first_else_block(nif) &&
                                      block == nir_if_last_else_block(nif);
               nir_foreach_instr(instr, block) {
                  if (instr->type != nir_instr_type_intrinsic)
                     continue;
                  nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
                  if (intr->intrinsic != nir_intrinsic_load_ubo || nir_src_is_const(intr->src[1]))
                     continue;
                  if (!guard) {
                     nir_intrinsic_set_access(intr, nir_intrinsic_access(intr) | ACCESS_CAN_SPECULATE);
                     continue;
                  }
                  if (!then_side && !else_side)
                     continue;
                  nir_def *cond = nif->condition.ssa;
                  nir_instr *g = nir_def_instr(intr->src[1].ssa);
                  bool guarded = false;
                  if (g->type == nir_instr_type_alu && nir_instr_as_alu(g)->op == nir_op_bcsel &&
                      nir_instr_as_alu(g)->src[0].src.ssa == cond)
                     guarded = true;
                  if (!guarded) {
                     b.cursor = nir_before_instr(instr);
                     nir_def *off = intr->src[1].ssa;
                     nir_def *zero = nir_imm_intN_t(&b, 0, off->bit_size);
                     nir_def *safe = then_side ? nir_bcsel(&b, cond, off, zero) : nir_bcsel(&b, cond, zero, off);
                     nir_src_rewrite(&intr->src[1], safe);
                     progress = true;
                  }
                  nir_intrinsic_set_access(intr, nir_intrinsic_access(intr) | ACCESS_CAN_SPECULATE);
               }
            }
            nir_progress(true, impl, nir_metadata_control_flow);
         }
         progress |= nir_opt_peephole_select(nir, &o);
         progress |= nir_opt_copy_prop(nir);
         progress |= nir_opt_dce(nir);
         progress |= nir_opt_cse(nir);
         progress |= nir_opt_constant_folding(nir);
         progress |= nir_opt_dead_cf(nir);
      }
   }

   struct util_dynarray binary;
   util_dynarray_init(&binary, NULL);
   const double t0 = now_ms();
   pan_shader_compile(nir, &inputs, &binary, &info);
   const double t1 = now_ms();

   const char *base = strrchr(path, '/');
   uint64_t hash = 0xcbf29ce484222325ull;
   for (unsigned i = 0; i < binary.size; ++i)
      hash = (hash ^ ((const uint8_t *)binary.data)[i]) * 0x100000001b3ull;
   printf("%s %s %.0f ms %u B %016llx", base ? base + 1 : path,
          mesa_shader_stage_name(nir->info.stage), t1 - t0, binary.size,
          (unsigned long long)hash);
   print_stats("main", &info.stats);
   if (nir->info.stage == MESA_SHADER_VERTEX && info.vs.idvs && info.vs.secondary_enable)
      print_stats("var", &info.stats_idvs_varying);
   printf("\n");
   fflush(stdout);

   util_dynarray_fini(&binary);
   ralloc_free(nir);
   free(data);
   return 0;
}

int
main(int argc, char **argv)
{
   glsl_type_singleton_init_or_ref();
   int rc = 0;
   for (int i = 1; i < argc; i++)
      rc |= compile_one(argv[i]);
   glsl_type_singleton_decref();
   return rc;
}
