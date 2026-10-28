/*
 * SPDX-FileCopyrightText: 2026 ARMSX2 and bmdhacks
 * SPDX-License-Identifier: MIT
 */

/*
 * MALISX2_V7_FP16=1: fragment shader arithmetic in 16-bit floats where it
 * cannot reach anything that needs 32 bits. A hack the application did not
 * ask for (its shaders carry no RelaxedPrecision), for Bifrost, whose ALUs
 * do two 16-bit operations in one instruction (FMA.v2f16 and the like): the
 * backend packs 16-bit operations in pairs (bi_vectorize_filter).
 *
 * What keeps 32 bits: every value that reaches, through any chain of ALU
 * operations, a texture coordinate or other texture source, an integer or
 * bitwise operation (float to integer conversions, floatBitsToUint), a
 * store or any other intrinsic, or a colour output whose render target has
 * more than 10 bits a channel or 32-bit floats, and the depth, stencil and
 * sample mask outputs. The rest of the float arithmetic (the operations in
 * convertible()) is done in 16 bits: f2fmp on the way in, f2f32 on the way
 * out, and nir_opt_algebraic folds the f2fmp(f2f32(x@16)) pairs between two
 * converted operations. Colour outputs to targets of up to 10 bits a
 * channel (RGBA8, RGB10A2, RGB565) or 16-bit floats are stored as 16-bit,
 * so the blend unit takes F16 registers (mali_v7_register_format), and
 * filtered texture samples that only feed 16-bit operations return 16-bit
 * values (fold_tex16).
 *
 * Comparisons, transcendentals and other float operations stay 32-bit but
 * take inputs that may have been rounded to 16 bits on the way: the colour
 * maths of emulators works in 0..255 or 0..1, where FP16 keeps integers
 * exact up to 2048.
 */

#include "mali_shader.h"

#include <math.h>

#include "nir_builder.h"
#include "util/bitset.h"
#include "util/format/u_format.h"
#include "vk_format.h"
#include "vk_graphics_state.h"

/* Float operations done in 16 bits when nothing needs their result in 32. */
static bool
convertible(nir_op op)
{
   switch (op) {
   case nir_op_fadd:
   case nir_op_fsub:
   case nir_op_fmul:
   case nir_op_ffma:
   case nir_op_fneg:
   case nir_op_fabs:
   case nir_op_fsat:
   case nir_op_fmin:
   case nir_op_fmax:
   case nir_op_flrp:
   case nir_op_ffloor:
   case nir_op_fceil:
   case nir_op_ftrunc:
   case nir_op_fround_even:
   case nir_op_ffract:
      return true;
   default:
      return false;
   }
}

/* Operations that move data of any type: converted when every data source
 * is a 16-bit float already (second step). */
static bool
mover(nir_op op)
{
   switch (op) {
   case nir_op_mov:
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
   case nir_op_bcsel:
      return true;
   default:
      return false;
   }
}

/* Float operations that stay 32-bit but may take rounded inputs. */
static bool
float_input_ok(nir_op op)
{
   switch (op) {
   case nir_op_flt:
   case nir_op_fge:
   case nir_op_feq:
   case nir_op_fneu:
   case nir_op_fltu:
   case nir_op_fgeu:
   case nir_op_fequ:
   case nir_op_fneo:
   case nir_op_ford:
   case nir_op_funord:
   case nir_op_fisfinite:
   case nir_op_fisnormal:
   case nir_op_frcp:
   case nir_op_frsq:
   case nir_op_fsqrt:
   case nir_op_fexp2:
   case nir_op_flog2:
   case nir_op_fsin:
   case nir_op_fcos:
   case nir_op_fpow:
   case nir_op_fdiv:
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4:
   case nir_op_f2f16:
   case nir_op_f2fmp:
      return true;
   default:
      return false;
   }
}

/* A colour target FP16 holds without loss: normalized channels of up to
 * 10 bits, or floats of up to 16. */
static bool
target_fp16_ok(VkFormat format)
{
   if (format == VK_FORMAT_UNDEFINED)
      return false;
   const struct util_format_description *d =
      util_format_description(vk_format_to_pipe_format(format));
   if (!d || d->layout != UTIL_FORMAT_LAYOUT_PLAIN || d->nr_channels == 0)
      return false;
   for (unsigned c = 0; c < d->nr_channels; c++) {
      const struct util_format_channel_description *ch = &d->channel[c];
      if (ch->type == UTIL_FORMAT_TYPE_VOID)
         continue;
      if (ch->type == UTIL_FORMAT_TYPE_FLOAT) {
         if (ch->size > 16)
            return false;
      } else if (!ch->normalized || ch->size > 10) {
         return false;
      }
   }
   return true;
}

/* Whether a store_output's colour can go out as 16-bit. */
static bool
output_fp16_ok(const nir_intrinsic_instr *intr, const struct vk_graphics_pipeline_state *state)
{
   if (nir_intrinsic_src_type(intr) != nir_type_float32 || intr->src[0].ssa->bit_size != 32)
      return false;
   if (!state || !state->rp)
      return false;
   const nir_io_semantics io = nir_intrinsic_io_semantics(intr);
   const struct vk_render_pass_state *rp = state->rp;
   if (io.location == FRAG_RESULT_COLOR) {
      if (!rp->color_attachment_count)
         return false;
      for (unsigned i = 0; i < rp->color_attachment_count; i++)
         if (rp->color_attachment_formats[i] != VK_FORMAT_UNDEFINED &&
             !target_fp16_ok(rp->color_attachment_formats[i]))
            return false;
      return true;
   }
   if (io.location < FRAG_RESULT_DATA0 || io.location > FRAG_RESULT_DATA7)
      return false;
   const unsigned rt = io.location - FRAG_RESULT_DATA0;
   return rt < rp->color_attachment_count &&
          target_fp16_ok(rp->color_attachment_formats[rt]);
}

struct fp16_ctx {
   BITSET_WORD *need32;
   const struct vk_graphics_pipeline_state *state;
};

static void
mark(struct fp16_ctx *ctx, nir_def *d)
{
   /* Only 32-bit values: booleans carry no precision, and 16-bit values
    * of the application stay as they are anyway. */
   if (d->bit_size == 32)
      BITSET_SET(ctx->need32, d->index);
}

static bool
needs32(const struct fp16_ctx *ctx, const nir_def *d)
{
   return BITSET_TEST(ctx->need32, d->index);
}

/* The uses that need 32 bits whatever happens to the rest. An if's
 * condition is a boolean and needs nothing. */
static void
mark_sinks(struct fp16_ctx *ctx, nir_function_impl *impl)
{
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         switch (instr->type) {
         case nir_instr_type_alu: {
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            if (convertible(alu->op) || mover(alu->op) || float_input_ok(alu->op))
               break;
            for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
               mark(ctx, alu->src[i].src.ssa);
            break;
         }
         case nir_instr_type_tex: {
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            for (unsigned i = 0; i < tex->num_srcs; i++)
               mark(ctx, tex->src[i].src.ssa);
            break;
         }
         case nir_instr_type_intrinsic: {
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            const unsigned n = nir_intrinsic_infos[intr->intrinsic].num_srcs;
            const bool colour = intr->intrinsic == nir_intrinsic_store_output &&
                                output_fp16_ok(intr, ctx->state);
            for (unsigned i = colour ? 1 : 0; i < n; i++)
               mark(ctx, intr->src[i].ssa);
            break;
         }
         default:
            break;
         }
      }
   }
}

/* A value needed in 32 bits needs its inputs in 32 bits too. */
static void
propagate(struct fp16_ctx *ctx, nir_function_impl *impl)
{
   bool progress;
   do {
      progress = false;
      nir_foreach_block_reverse(block, impl) {
         nir_foreach_instr_reverse(instr, block) {
            if (instr->type == nir_instr_type_alu) {
               nir_alu_instr *alu = nir_instr_as_alu(instr);
               if (!needs32(ctx, &alu->def))
                  continue;
               for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
                  nir_def *s = alu->src[i].src.ssa;
                  if (s->bit_size == 32 && !needs32(ctx, s)) {
                     mark(ctx, s);
                     progress = true;
                  }
               }
            } else if (instr->type == nir_instr_type_phi) {
               nir_phi_instr *phi = nir_instr_as_phi(instr);
               if (!needs32(ctx, &phi->def))
                  continue;
               nir_foreach_phi_src(src, phi) {
                  if (src->src.ssa->bit_size == 32 && !needs32(ctx, src->src.ssa)) {
                     mark(ctx, src->src.ssa);
                     progress = true;
                  }
               }
            }
         }
      }
   } while (progress);
}

/* A 16-bit float in a 32-bit value: f2f32 of a 16-bit value, or a constant
 * (which f2fmp folds). */
static bool
is_widened_fp16(const nir_def *d)
{
   const nir_instr *p = nir_def_instr(d);
   if (p->type == nir_instr_type_load_const)
      return true;
   if (p->type != nir_instr_type_alu)
      return false;
   const nir_alu_instr *alu = nir_instr_as_alu(p);
   return alu->op == nir_op_f2f32 && alu->src[0].src.ssa->bit_size == 16;
}

/* Whether the operation's constant sources fit FP16: normal values up to
 * 65504, or zero. A constant outside (16777216.0 for a depth, 1/16777215)
 * keeps the operation 32-bit. */
static bool
constants_fit(const nir_alu_instr *alu)
{
   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
      const nir_instr *p = nir_def_instr(alu->src[i].src.ssa);
      if (p->type != nir_instr_type_load_const)
         continue;
      const nir_load_const_instr *lc = nir_instr_as_load_const(p);
      if (lc->def.bit_size != 32)
         continue;
      for (unsigned c = 0; c < nir_ssa_alu_instr_src_components(alu, i); c++) {
         const double v = fabs(nir_const_value_as_float(lc->value[alu->src[i].swizzle[c]], 32));
         if (v != 0.0 && (v > 65504.0 || v < 1.0 / 16384.0))
            return false;
      }
   }
   return true;
}

/* The same operation on 16-bit sources, widened back for its users. The
 * sources before first_data_src (a select's condition) stay as they are. */
static void
convert(nir_builder *b, nir_alu_instr *alu, unsigned first_data_src)
{
   const unsigned n = nir_op_infos[alu->op].num_inputs;
   nir_def *srcs[NIR_MAX_VEC_COMPONENTS];
   b->cursor = nir_before_instr(&alu->instr);
   b->fp_math_ctrl = alu->fp_math_ctrl;
   for (unsigned i = 0; i < n; i++) {
      nir_def *s = nir_ssa_for_alu_src(b, alu, i);
      srcs[i] = i < first_data_src ? s : nir_f2fmp(b, s);
   }
   nir_def *r = nir_build_alu_src_arr(b, alu->op, srcs);
   nir_def_rewrite_uses(&alu->def, nir_f2f32(b, r));
   nir_instr_remove(&alu->instr);
}

/* Filtered samples of float textures whose every user is an f2fmp return
 * 16-bit values (TEXS/TEXC with an F16 register format), and the f2fmp
 * become moves. Like nir_opt_16bit_tex_image, but only for implicit-LOD,
 * biased, explicit-LOD and gradient samples of images, not shadow: Mesa's
 * pass also folds texel fetches, and a texel buffer fetch on v7 (an
 * attribute fetch, texelFetch of a samplerBuffer: Azahar's lighting and fog
 * tables) with a 16-bit result stopped Castlevania at its second frame. */
static bool
fold_tex16(nir_function_impl *impl)
{
   bool progress = false;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_tex)
            continue;
         nir_tex_instr *tex = nir_instr_as_tex(instr);
         if ((tex->op != nir_texop_tex && tex->op != nir_texop_txb &&
              tex->op != nir_texop_txl && tex->op != nir_texop_txd) ||
             tex->sampler_dim == GLSL_SAMPLER_DIM_BUF || tex->is_shadow ||
             tex->dest_type != nir_type_float32 || tex->def.bit_size != 32)
            continue;
         bool all_f2fmp = !nir_def_is_unused(&tex->def);
         nir_foreach_use_including_if(use, &tex->def) {
            if (nir_src_is_if(use) || nir_src_use_instr(use)->type != nir_instr_type_alu ||
                nir_instr_as_alu(nir_src_use_instr(use))->op != nir_op_f2fmp) {
               all_f2fmp = false;
               break;
            }
         }
         if (!all_f2fmp)
            continue;
         nir_foreach_use(use, &tex->def) {
            nir_alu_instr *alu = nir_instr_as_alu(nir_src_use_instr(use));
            alu->op = nir_op_mov;
            alu->fp_math_ctrl = nir_op_valid_fp_math_ctrl(alu->op, alu->fp_math_ctrl);
         }
         tex->def.bit_size = 16;
         tex->dest_type = nir_type_float16;
         progress = true;
      }
   }
   return progress;
}

bool
mali_nir_fs_fp16(nir_shader *nir, const struct vk_graphics_pipeline_state *state)
{
   assert(nir->info.stage == MESA_SHADER_FRAGMENT);
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_index_ssa_defs(impl);

   struct fp16_ctx ctx = {
      .need32 = calloc(BITSET_WORDS(impl->ssa_alloc), sizeof(BITSET_WORD)),
      .state = state,
   };
   if (!ctx.need32)
      return false;

   mark_sinks(&ctx, impl);
   propagate(&ctx, impl);

   nir_builder b = nir_builder_create(impl);
   bool progress = false;

   /* Colour outputs to targets FP16 holds go out as 16-bit. */
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_store_output || !output_fp16_ok(intr, state))
            continue;
         b.cursor = nir_before_instr(instr);
         nir_src_rewrite(&intr->src[0], nir_f2fmp(&b, intr->src[0].ssa));
         nir_intrinsic_set_src_type(intr, nir_type_float16);
         progress = true;
      }
   }

   /* The arithmetic. Defs made here have indices past need32's end, so
    * only the original instructions are looked up. */
   const unsigned original_defs = impl->ssa_alloc;
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_alu)
            continue;
         nir_alu_instr *alu = nir_instr_as_alu(instr);
         if (!convertible(alu->op) || alu->def.bit_size != 32 ||
             alu->def.index >= original_defs || needs32(&ctx, &alu->def) ||
             (alu->fp_math_ctrl & nir_fp_no_contract) || !constants_fit(alu))
            continue;
         convert(&b, alu, 0);
         progress = true;
      }
   }

   /* Moves, vectors and selects of 16-bit floats, until none is left. */
   bool more;
   do {
      more = false;
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            if (!mover(alu->op) || alu->def.bit_size != 32 ||
                alu->def.index >= original_defs || needs32(&ctx, &alu->def))
               continue;
            const unsigned first = alu->op == nir_op_bcsel ? 1 : 0;
            bool all16 = true, any_alu = false;
            for (unsigned i = first; i < nir_op_infos[alu->op].num_inputs && all16; i++) {
               all16 = is_widened_fp16(alu->src[i].src.ssa);
               any_alu |= nir_def_instr(alu->src[i].src.ssa)->type == nir_instr_type_alu;
            }
            /* All-constant moves are left to constant folding. */
            if (!all16 || !any_alu || !constants_fit(alu))
               continue;
            convert(&b, alu, first);
            more = progress = true;
         }
      }
   } while (more);

   free(ctx.need32);
   if (!progress)
      return nir_no_progress(impl);

   nir_progress(true, impl, nir_metadata_control_flow);
   /* Texture samples whose every user now takes 16 bits return 16-bit
    * values (fold_tex16). nir_ssa_for_alu_src put a swizzling mov between
    * a sample and each f2fmp; copy propagation folds those into the f2fmp
    * sources first. */
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_dce);
   if (fold_tex16(impl))
      nir_progress(true, impl, nir_metadata_control_flow);
   NIR_PASS(_, nir, nir_opt_algebraic);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_opt_cse);
   return true;
}
