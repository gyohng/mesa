/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "radv_shader.h"
#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "nir/nir_xfb_info.h"
#include "spirv/nir_spirv.h"
#include "util/memstream.h"
#include "util/mesa-sha1.h"
#include "util/u_atomic.h"
#include "radv_debug.h"
#include "radv_meta.h"
#include "radv_private.h"
#include "radv_shader_args.h"

#include "util/u_debug.h"
#include "ac_binary.h"
#include "ac_nir.h"
#if defined(USE_LIBELF)
#include "ac_rtld.h"
#endif
#include "aco_interface.h"
#include "sid.h"
#include "vk_format.h"

#include "aco_shader_info.h"
#include "radv_aco_shader_info.h"
#ifdef LLVM_AVAILABLE
#include "ac_llvm_util.h"
#endif

static void
get_nir_options_for_stage(struct radv_physical_device *device, gl_shader_stage stage)
{
   bool split_fma = (stage <= MESA_SHADER_GEOMETRY || stage == MESA_SHADER_MESH) &&
                    device->instance->debug_flags & RADV_DEBUG_SPLIT_FMA;
   device->nir_options[stage] = (nir_shader_compiler_options){
      .vertex_id_zero_based = true,
      .lower_scmp = true,
      .lower_flrp16 = true,
      .lower_flrp32 = true,
      .lower_flrp64 = true,
      .lower_device_index_to_zero = true,
      .lower_fdiv = true,
      .lower_fmod = true,
      .lower_ineg = true,
      .lower_bitfield_insert_to_bitfield_select = true,
      .lower_bitfield_extract = true,
      .lower_pack_snorm_4x8 = true,
      .lower_pack_unorm_4x8 = true,
      .lower_pack_half_2x16 = true,
      .lower_pack_64_2x32 = true,
      .lower_pack_64_4x16 = true,
      .lower_pack_32_2x16 = true,
      .lower_unpack_snorm_2x16 = true,
      .lower_unpack_snorm_4x8 = true,
      .lower_unpack_unorm_2x16 = true,
      .lower_unpack_unorm_4x8 = true,
      .lower_unpack_half_2x16 = true,
      .lower_ffma16 = split_fma || device->rad_info.gfx_level < GFX9,
      .lower_ffma32 = split_fma || device->rad_info.gfx_level < GFX10_3,
      .lower_ffma64 = split_fma,
      .lower_fpow = true,
      .lower_mul_2x32_64 = true,
      .lower_rotate = true,
      .lower_iadd_sat = device->rad_info.gfx_level <= GFX8,
      .lower_hadd = true,
      .lower_mul_32x16 = true,
      .lower_uclz = true,
      .has_fsub = true,
      .has_isub = true,
      .has_sdot_4x8 = device->rad_info.has_accelerated_dot_product,
      .has_sudot_4x8 =
         device->rad_info.has_accelerated_dot_product && device->rad_info.gfx_level >= GFX11,
      .has_udot_4x8 = device->rad_info.has_accelerated_dot_product,
      .has_dot_2x16 =
         device->rad_info.has_accelerated_dot_product && device->rad_info.gfx_level < GFX11,
      .has_find_msb_rev = true,
      .has_pack_half_2x16_rtz = true,
      .use_scoped_barrier = true,
#ifdef LLVM_AVAILABLE
      .has_fmulz = !device->use_llvm || LLVM_VERSION_MAJOR >= 12,
#else
      .has_fmulz = true,
#endif
      .max_unroll_iterations = 32,
      .max_unroll_iterations_aggressive = 128,
      .use_interpolated_input_intrinsics = true,
      .vectorize_vec2_16bit = true,
      /* nir_lower_int64() isn't actually called for the LLVM backend,
       * but this helps the loop unrolling heuristics. */
      .lower_int64_options = nir_lower_imul64 | nir_lower_imul_high64 | nir_lower_imul_2x32_64 |
                             nir_lower_divmod64 | nir_lower_minmax64 | nir_lower_iabs64 |
                             nir_lower_iadd_sat64,
      .lower_doubles_options = nir_lower_drcp | nir_lower_dsqrt | nir_lower_drsq | nir_lower_ddiv,
      .divergence_analysis_options = nir_divergence_view_index_uniform,
   };
}

void
radv_get_nir_options(struct radv_physical_device *device)
{
   for (gl_shader_stage stage = MESA_SHADER_VERTEX; stage < MESA_VULKAN_SHADER_STAGES; stage++)
      get_nir_options_for_stage(device, stage);
}

static uint8_t
vectorize_vec2_16bit(const nir_instr *instr, const void *_)
{
   if (instr->type != nir_instr_type_alu)
      return 0;

   const nir_alu_instr *alu = nir_instr_as_alu(instr);
   const unsigned bit_size = alu->dest.dest.ssa.bit_size;
   if (bit_size == 16)
      return 2;
   else
      return 1;
}

static bool
is_meta_shader(nir_shader *nir)
{
   return nir && nir->info.internal;
}

bool
radv_can_dump_shader(struct radv_device *device, nir_shader *nir, bool meta_shader)
{
   if (!(device->instance->debug_flags & RADV_DEBUG_DUMP_SHADERS))
      return false;

   if ((is_meta_shader(nir) || meta_shader) &&
       !(device->instance->debug_flags & RADV_DEBUG_DUMP_META_SHADERS))
      return false;

   return true;
}

bool
radv_can_dump_shader_stats(struct radv_device *device, nir_shader *nir)
{
   /* Only dump non-meta shader stats. */
   return device->instance->debug_flags & RADV_DEBUG_DUMP_SHADER_STATS && !is_meta_shader(nir);
}

void
radv_optimize_nir(struct nir_shader *shader, bool optimize_conservatively)
{
   bool progress;

   do {
      progress = false;

      NIR_PASS(progress, shader, nir_split_array_vars, nir_var_function_temp);
      NIR_PASS(progress, shader, nir_shrink_vec_array_vars, nir_var_function_temp);

      if (!shader->info.var_copies_lowered) {
         /* Only run this pass if nir_lower_var_copies was not called
          * yet. That would lower away any copy_deref instructions and we
          * don't want to introduce any more.
          */
         NIR_PASS(progress, shader, nir_opt_find_array_copies);
      }

      NIR_PASS(progress, shader, nir_opt_copy_prop_vars);
      NIR_PASS(progress, shader, nir_opt_dead_write_vars);
      NIR_PASS(_, shader, nir_lower_vars_to_ssa);

      NIR_PASS(_, shader, nir_lower_alu_width, vectorize_vec2_16bit, NULL);
      NIR_PASS(_, shader, nir_lower_phis_to_scalar, true);

      NIR_PASS(progress, shader, nir_copy_prop);
      NIR_PASS(progress, shader, nir_opt_remove_phis);
      NIR_PASS(progress, shader, nir_opt_dce);
      if (nir_opt_trivial_continues(shader)) {
         progress = true;
         NIR_PASS(progress, shader, nir_copy_prop);
         NIR_PASS(progress, shader, nir_opt_remove_phis);
         NIR_PASS(progress, shader, nir_opt_dce);
      }
      NIR_PASS(progress, shader, nir_opt_if,
               nir_opt_if_aggressive_last_continue | nir_opt_if_optimize_phi_true_false);
      NIR_PASS(progress, shader, nir_opt_dead_cf);
      NIR_PASS(progress, shader, nir_opt_cse);
      NIR_PASS(progress, shader, nir_opt_peephole_select, 8, true, true);
      NIR_PASS(progress, shader, nir_opt_constant_folding);
      NIR_PASS(progress, shader, nir_opt_algebraic);

      NIR_PASS(progress, shader, nir_opt_undef);

      if (shader->options->max_unroll_iterations) {
         NIR_PASS(progress, shader, nir_opt_loop_unroll);
      }
   } while (progress && !optimize_conservatively);

   NIR_PASS(progress, shader, nir_opt_shrink_vectors);
   NIR_PASS(progress, shader, nir_remove_dead_variables,
            nir_var_function_temp | nir_var_shader_in | nir_var_shader_out, NULL);

   if (shader->info.stage == MESA_SHADER_FRAGMENT &&
       (shader->info.fs.uses_discard || shader->info.fs.uses_demote)) {
      NIR_PASS(progress, shader, nir_opt_conditional_discard);
      NIR_PASS(progress, shader, nir_opt_move_discards_to_top);
   }

   NIR_PASS(progress, shader, nir_opt_move, nir_move_load_ubo);
}

void
radv_optimize_nir_algebraic(nir_shader *nir, bool opt_offsets)
{
   bool more_algebraic = true;
   while (more_algebraic) {
      more_algebraic = false;
      NIR_PASS(_, nir, nir_copy_prop);
      NIR_PASS(_, nir, nir_opt_dce);
      NIR_PASS(_, nir, nir_opt_constant_folding);
      NIR_PASS(_, nir, nir_opt_cse);
      NIR_PASS(more_algebraic, nir, nir_opt_algebraic);
   }

   if (opt_offsets) {
      static const nir_opt_offsets_options offset_options = {
         .uniform_max = 0,
         .buffer_max = ~0,
         .shared_max = ~0,
      };
      NIR_PASS(_, nir, nir_opt_offsets, &offset_options);
   }

   /* Do late algebraic optimization to turn add(a,
    * neg(b)) back into subs, then the mandatory cleanup
    * after algebraic.  Note that it may produce fnegs,
    * and if so then we need to keep running to squash
    * fneg(fneg(a)).
    */
   bool more_late_algebraic = true;
   while (more_late_algebraic) {
      more_late_algebraic = false;
      NIR_PASS(more_late_algebraic, nir, nir_opt_algebraic_late);
      NIR_PASS(_, nir, nir_opt_constant_folding);
      NIR_PASS(_, nir, nir_copy_prop);
      NIR_PASS(_, nir, nir_opt_dce);
      NIR_PASS(_, nir, nir_opt_cse);
   }
}

static void
shared_var_info(const struct glsl_type *type, unsigned *size, unsigned *align)
{
   assert(glsl_type_is_vector_or_scalar(type));

   uint32_t comp_size = glsl_type_is_boolean(type) ? 4 : glsl_get_bit_size(type) / 8;
   unsigned length = glsl_get_vector_elements(type);
   *size = comp_size * length, *align = comp_size;
}

struct radv_shader_debug_data {
   struct radv_device *device;
   const struct vk_object_base *object;
};

static void
radv_spirv_nir_debug(void *private_data, enum nir_spirv_debug_level level, size_t spirv_offset,
                     const char *message)
{
   struct radv_shader_debug_data *debug_data = private_data;
   struct radv_instance *instance = debug_data->device->instance;

   static const VkDebugReportFlagsEXT vk_flags[] = {
      [NIR_SPIRV_DEBUG_LEVEL_INFO] = VK_DEBUG_REPORT_INFORMATION_BIT_EXT,
      [NIR_SPIRV_DEBUG_LEVEL_WARNING] = VK_DEBUG_REPORT_WARNING_BIT_EXT,
      [NIR_SPIRV_DEBUG_LEVEL_ERROR] = VK_DEBUG_REPORT_ERROR_BIT_EXT,
   };
   char buffer[256];

   snprintf(buffer, sizeof(buffer), "SPIR-V offset %lu: %s", (unsigned long)spirv_offset, message);

   vk_debug_report(&instance->vk, vk_flags[level], debug_data->object, 0, 0, "radv", buffer);
}

static void
radv_compiler_debug(void *private_data, enum aco_compiler_debug_level level, const char *message)
{
   struct radv_shader_debug_data *debug_data = private_data;
   struct radv_instance *instance = debug_data->device->instance;

   static const VkDebugReportFlagsEXT vk_flags[] = {
      [ACO_COMPILER_DEBUG_LEVEL_PERFWARN] = VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT,
      [ACO_COMPILER_DEBUG_LEVEL_ERROR] = VK_DEBUG_REPORT_ERROR_BIT_EXT,
   };

   /* VK_DEBUG_REPORT_DEBUG_BIT_EXT specifies diagnostic information
    * from the implementation and layers.
    */
   vk_debug_report(&instance->vk, vk_flags[level] | VK_DEBUG_REPORT_DEBUG_BIT_EXT,
                   NULL, 0, 0, "radv", message);
}

static bool
lower_intrinsics(nir_shader *nir, const struct radv_pipeline_key *key)
{
   nir_function_impl *entry = nir_shader_get_entrypoint(nir);
   bool progress = false;
   nir_builder b;

   nir_builder_init(&b, entry);

   nir_foreach_block (block, entry) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         b.cursor = nir_before_instr(&intrin->instr);

         nir_ssa_def *def = NULL;
         switch (intrin->intrinsic) {
         case nir_intrinsic_is_sparse_texels_resident:
            def = nir_ieq_imm(&b, intrin->src[0].ssa, 0);
            break;
         case nir_intrinsic_sparse_residency_code_and:
            def = nir_ior(&b, intrin->src[0].ssa, intrin->src[1].ssa);
            break;
         case nir_intrinsic_load_view_index:
            if (key->has_multiview_view_index)
               continue;
            def = nir_imm_zero(&b, 1, 32);
            break;
         default:
            continue;
         }

         nir_ssa_def_rewrite_uses(&intrin->dest.ssa, def);

         nir_instr_remove(instr);
         progress = true;
      }
   }

   if (progress)
      nir_metadata_preserve(entry, nir_metadata_block_index | nir_metadata_dominance);
   else
      nir_metadata_preserve(entry, nir_metadata_all);

   return progress;
}

static bool
radv_lower_primitive_shading_rate(nir_shader *nir, enum amd_gfx_level gfx_level)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   bool progress = false;

   nir_builder b;
   nir_builder_init(&b, impl);

   /* Iterate in reverse order since there should be only one deref store to PRIMITIVE_SHADING_RATE
    * after lower_io_to_temporaries for vertex shaders.
    */
   nir_foreach_block_reverse(block, impl) {
      nir_foreach_instr_reverse(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_store_deref)
            continue;

         nir_variable *var = nir_intrinsic_get_var(intr, 0);
         if (var->data.mode != nir_var_shader_out ||
             var->data.location != VARYING_SLOT_PRIMITIVE_SHADING_RATE)
            continue;

         b.cursor = nir_before_instr(instr);

         nir_ssa_def *val = nir_ssa_for_src(&b, intr->src[1], 1);

         /* x_rate = (shadingRate & (Horizontal2Pixels | Horizontal4Pixels)) ? 0x1 : 0x0; */
         nir_ssa_def *x_rate = nir_iand_imm(&b, val, 12);
         x_rate = nir_b2i32(&b, nir_ine_imm(&b, x_rate, 0));

         /* y_rate = (shadingRate & (Vertical2Pixels | Vertical4Pixels)) ? 0x1 : 0x0; */
         nir_ssa_def *y_rate = nir_iand_imm(&b, val, 3);
         y_rate = nir_b2i32(&b, nir_ine_imm(&b, y_rate, 0));

         nir_ssa_def *out = NULL;

         /* MS:
          * Primitive shading rate is a per-primitive output, it is
          * part of the second channel of the primitive export.
          * Bits [28:31] = VRS rate
          * This will be added to the other bits of that channel in the backend.
          *
          * VS, TES, GS:
          * Primitive shading rate is a per-vertex output pos export.
          * Bits [2:5] = VRS rate
          * HW shading rate = (xRate << 2) | (yRate << 4)
          *
          * GFX11: 4-bit VRS_SHADING_RATE enum
          * GFX10: X = low 2 bits, Y = high 2 bits
          */
         unsigned x_rate_shift = 2;
         unsigned y_rate_shift = 4;

         if (gfx_level >= GFX11) {
            x_rate_shift = 4;
            y_rate_shift = 2;
         }
         if (nir->info.stage == MESA_SHADER_MESH) {
            x_rate_shift += 26;
            y_rate_shift += 26;
         }

         out = nir_ior(&b, nir_ishl_imm(&b, x_rate, x_rate_shift), nir_ishl_imm(&b, y_rate, y_rate_shift));

         nir_instr_rewrite_src(&intr->instr, &intr->src[1], nir_src_for_ssa(out));

         progress = true;
         if (nir->info.stage == MESA_SHADER_VERTEX)
            break;
      }
      if (nir->info.stage == MESA_SHADER_VERTEX && progress)
         break;
   }

   if (progress)
      nir_metadata_preserve(impl, nir_metadata_block_index | nir_metadata_dominance);
   else
      nir_metadata_preserve(impl, nir_metadata_all);

   return progress;
}

bool
radv_lower_fs_intrinsics(nir_shader *nir, const struct radv_pipeline_stage *fs_stage,
                         const struct radv_pipeline_key *key)
{
   const struct radv_shader_info *info = &fs_stage->info;
   const struct radv_shader_args *args = &fs_stage->args;
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   bool progress = false;

   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         b.cursor = nir_after_instr(&intrin->instr);

         switch (intrin->intrinsic) {
         case nir_intrinsic_load_sample_mask_in: {
            nir_ssa_def *sample_coverage =
               nir_load_vector_arg_amd(&b, 1, .base = args->ac.sample_coverage.arg_index);

            nir_ssa_def *def = NULL;
            if (info->ps.uses_sample_shading || key->ps.sample_shading_enable) {
               /* gl_SampleMaskIn[0] = (SampleCoverage & (1 << gl_SampleID)). */
               nir_ssa_def *sample_id = nir_load_sample_id(&b);
               def = nir_iand(&b, sample_coverage, nir_ishl(&b, nir_imm_int(&b, 1u), sample_id));
            } else {
               def = sample_coverage;
            }

            nir_ssa_def_rewrite_uses(&intrin->dest.ssa, def);

            nir_instr_remove(instr);
            progress = true;
            break;
         }
         case nir_intrinsic_load_frag_coord: {
            if (!key->adjust_frag_coord_z)
               continue;

            if (!(nir_ssa_def_components_read(&intrin->dest.ssa) & (1 << 2)))
               continue;

            nir_ssa_def *frag_z = nir_channel(&b, &intrin->dest.ssa, 2);

            /* adjusted_frag_z = fddx_fine(frag_z) * 0.0625 + frag_z */
            nir_ssa_def *adjusted_frag_z = nir_fddx_fine(&b, frag_z);
            adjusted_frag_z = nir_ffma_imm1(&b, adjusted_frag_z, 0.0625f, frag_z);

            /* VRS Rate X = Ancillary[2:3] */
            nir_ssa_def *ancillary =
               nir_load_vector_arg_amd(&b, 1, .base = args->ac.ancillary.arg_index);
            nir_ssa_def *x_rate = nir_ubfe_imm(&b, ancillary, 2, 2);

            /* xRate = xRate == 0x1 ? adjusted_frag_z : frag_z. */
            nir_ssa_def *cond = nir_ieq_imm(&b, x_rate, 1);
            frag_z = nir_bcsel(&b, cond, adjusted_frag_z, frag_z);

            nir_ssa_def *new_dest = nir_vector_insert_imm(&b, &intrin->dest.ssa, frag_z, 2);
            nir_ssa_def_rewrite_uses_after(&intrin->dest.ssa, new_dest, new_dest->parent_instr);

            progress = true;
            break;
         }
         case nir_intrinsic_load_barycentric_at_sample: {
            nir_ssa_def *num_samples = nir_load_rasterization_samples_amd(&b);
            nir_ssa_def *new_dest;

            if (key->dynamic_rasterization_samples) {
               nir_ssa_def *res1, *res2;

               nir_push_if(&b, nir_ieq_imm(&b, num_samples, 1));
               {
                  res1 = nir_load_barycentric_pixel(&b, 32,
                                                    .interp_mode = nir_intrinsic_interp_mode(intrin));
               }
               nir_push_else(&b, NULL);
               {
                  nir_ssa_def *sample_pos =
                     nir_load_sample_positions_amd(&b, 32, intrin->src[0].ssa, num_samples);

                  /* sample_pos -= 0.5 */
                  sample_pos = nir_fsub(&b, sample_pos, nir_imm_float(&b, 0.5f));

                  res2 = nir_load_barycentric_at_offset(&b, 32, sample_pos,
                                                        .interp_mode = nir_intrinsic_interp_mode(intrin));
               }
               nir_pop_if(&b, NULL);

               new_dest = nir_if_phi(&b, res1, res2);
            } else {
               if (!key->ps.num_samples) {
                  new_dest =
                     nir_load_barycentric_pixel(&b, 32,
                                                .interp_mode = nir_intrinsic_interp_mode(intrin));
               } else {
                  nir_ssa_def *sample_pos =
                     nir_load_sample_positions_amd(&b, 32, intrin->src[0].ssa, num_samples);

                  /* sample_pos -= 0.5 */
                  sample_pos = nir_fsub(&b, sample_pos, nir_imm_float(&b, 0.5f));

                  new_dest =
                     nir_load_barycentric_at_offset(&b, 32, sample_pos,
                                                    .interp_mode = nir_intrinsic_interp_mode(intrin));
               }
            }

            nir_ssa_def_rewrite_uses(&intrin->dest.ssa, new_dest);
            nir_instr_remove(instr);

            progress = true;
            break;
         }
         default:
            break;
         }
      }
   }

   if (progress)
      nir_metadata_preserve(impl, 0);
   else
      nir_metadata_preserve(impl, nir_metadata_all);

   return progress;
}

/* Emulates NV_mesh_shader first_task using first_vertex. */
static bool
radv_lower_ms_workgroup_id(nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   bool progress = false;
   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_load_workgroup_id)
            continue;

         progress = true;
         b.cursor = nir_after_instr(instr);
         nir_ssa_def *x = nir_channel(&b, &intrin->dest.ssa, 0);
         nir_ssa_def *x_full = nir_iadd(&b, x, nir_load_first_vertex(&b));
         nir_ssa_def *v = nir_vector_insert_imm(&b, &intrin->dest.ssa, x_full, 0);
         nir_ssa_def_rewrite_uses_after(&intrin->dest.ssa, v, v->parent_instr);
      }
   }

   nir_metadata preserved =
      progress ? (nir_metadata_block_index | nir_metadata_dominance) : nir_metadata_all;
   nir_metadata_preserve(impl, preserved);
   return progress;
}

static bool
is_sincos(const nir_instr *instr, const void *_)
{
   return instr->type == nir_instr_type_alu &&
          (nir_instr_as_alu(instr)->op == nir_op_fsin || nir_instr_as_alu(instr)->op == nir_op_fcos);
}

static nir_ssa_def *
lower_sincos(struct nir_builder *b, nir_instr *instr, void *_)
{
   nir_alu_instr *sincos = nir_instr_as_alu(instr);
   nir_ssa_def *src = nir_fmul_imm(b, nir_ssa_for_alu_src(b, sincos, 0), 0.15915493667125702);
   return sincos->op == nir_op_fsin ? nir_fsin_amd(b, src) : nir_fcos_amd(b, src);
}

static bool
is_not_xfb_output(nir_variable *var, void *data)
{
   if (var->data.mode != nir_var_shader_out)
      return true;

   return !var->data.explicit_xfb_buffer &&
          !var->data.explicit_xfb_stride;
}

nir_shader *
radv_shader_spirv_to_nir(struct radv_device *device, const struct radv_pipeline_stage *stage,
                         const struct radv_pipeline_key *key, bool is_internal)
{
   unsigned subgroup_size = 64, ballot_bit_size = 64;
   if (key->cs.compute_subgroup_size) {
      /* Only compute shaders currently support requiring a
       * specific subgroup size.
       */
      assert(stage->stage >= MESA_SHADER_COMPUTE);
      subgroup_size = key->cs.compute_subgroup_size;
      ballot_bit_size = key->cs.compute_subgroup_size;
   }

   nir_shader *nir;

   if (stage->internal_nir) {
      /* Some things such as our meta clear/blit code will give us a NIR
       * shader directly.  In that case, we just ignore the SPIR-V entirely
       * and just use the NIR shader.  We don't want to alter meta and RT
       * shaders IR directly, so clone it first. */
      nir = nir_shader_clone(NULL, stage->internal_nir);
      nir_validate_shader(nir, "in internal shader");

      assert(exec_list_length(&nir->functions) == 1);
   } else {
      uint32_t *spirv = (uint32_t *)stage->spirv.data;
      assert(stage->spirv.size % 4 == 0);

      bool dump_meta = device->instance->debug_flags & RADV_DEBUG_DUMP_META_SHADERS;
      if ((device->instance->debug_flags & RADV_DEBUG_DUMP_SPIRV) &&
          (!is_internal || dump_meta))
         radv_print_spirv(stage->spirv.data, stage->spirv.size, stderr);

      uint32_t num_spec_entries = 0;
      struct nir_spirv_specialization *spec_entries =
         vk_spec_info_to_nir_spirv(stage->spec_info, &num_spec_entries);
      struct radv_shader_debug_data spirv_debug_data = {
         .device = device,
         .object = stage->spirv.object,
      };
      const struct spirv_to_nir_options spirv_options = {
         .caps =
            {
               .amd_fragment_mask = true,
               .amd_gcn_shader = true,
               .amd_image_gather_bias_lod = true,
               .amd_image_read_write_lod = true,
               .amd_shader_ballot = true,
               .amd_shader_explicit_vertex_parameter = true,
               .amd_trinary_minmax = true,
               .demote_to_helper_invocation = true,
               .derivative_group = true,
               .descriptor_array_dynamic_indexing = true,
               .descriptor_array_non_uniform_indexing = true,
               .descriptor_indexing = true,
               .device_group = true,
               .draw_parameters = true,
               .float_controls = true,
               .float16 = device->physical_device->rad_info.has_packed_math_16bit,
               .float32_atomic_add = true,
               .float32_atomic_min_max = true,
               .float64 = true,
               .float64_atomic_min_max = true,
               .geometry_streams = true,
               .groups = true,
               .image_atomic_int64 = true,
               .image_ms_array = true,
               .image_read_without_format = true,
               .image_write_without_format = true,
               .int8 = true,
               .int16 = true,
               .int64 = true,
               .int64_atomics = true,
               .integer_functions2 = true,
               .mesh_shading = true,
               .mesh_shading_nv = true,
               .min_lod = true,
               .multiview = true,
               .physical_storage_buffer_address = true,
               .post_depth_coverage = true,
               .ray_cull_mask = true,
               .ray_query = true,
               .ray_tracing = true,
               .ray_traversal_primitive_culling = true,
               .runtime_descriptor_array = true,
               .shader_clock = true,
               .shader_viewport_index_layer = true,
               .sparse_residency = true,
               .stencil_export = true,
               .storage_8bit = true,
               .storage_16bit = true,
               .storage_image_ms = true,
               .subgroup_arithmetic = true,
               .subgroup_ballot = true,
               .subgroup_basic = true,
               .subgroup_quad = true,
               .subgroup_shuffle = true,
               .subgroup_uniform_control_flow = true,
               .subgroup_vote = true,
               .tessellation = true,
               .transform_feedback = true,
               .variable_pointers = true,
               .vk_memory_model = true,
               .vk_memory_model_device_scope = true,
               .fragment_shading_rate = device->physical_device->rad_info.gfx_level >= GFX10_3,
               .workgroup_memory_explicit_layout = true,
            },
         .ubo_addr_format = nir_address_format_vec2_index_32bit_offset,
         .ssbo_addr_format = nir_address_format_vec2_index_32bit_offset,
         .phys_ssbo_addr_format = nir_address_format_64bit_global,
         .push_const_addr_format = nir_address_format_logical,
         .shared_addr_format = nir_address_format_32bit_offset,
         .constant_addr_format = nir_address_format_64bit_global,
         .use_deref_buffer_array_length = true,
         .debug =
            {
               .func = radv_spirv_nir_debug,
               .private_data = &spirv_debug_data,
            },
         .force_tex_non_uniform = key->tex_non_uniform,
      };
      nir = spirv_to_nir(spirv, stage->spirv.size / 4, spec_entries, num_spec_entries, stage->stage,
                         stage->entrypoint, &spirv_options,
                         &device->physical_device->nir_options[stage->stage]);
      nir->info.internal |= is_internal;
      assert(nir->info.stage == stage->stage);

      /* Work around applications that declare shader_call_data variables inside ray generation
       * shaders. */
      if (nir->info.stage == MESA_SHADER_RAYGEN)
         NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_call_data, NULL);

      nir_validate_shader(nir, "after spirv_to_nir");

      free(spec_entries);

      const struct nir_lower_sysvals_to_varyings_options sysvals_to_varyings = {
         .point_coord = true,
      };
      NIR_PASS_V(nir, nir_lower_sysvals_to_varyings, &sysvals_to_varyings);

      /* We have to lower away local constant initializers right before we
       * inline functions.  That way they get properly initialized at the top
       * of the function and not at the top of its caller.
       */
      NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
      NIR_PASS(_, nir, nir_lower_returns);
      bool progress = false;
      NIR_PASS(progress, nir, nir_inline_functions);
      if (progress) {
         NIR_PASS(_, nir, nir_opt_copy_prop_vars);
         NIR_PASS(_, nir, nir_copy_prop);
      }
      NIR_PASS(_, nir, nir_opt_deref);

      /* Pick off the single entrypoint that we want */
      foreach_list_typed_safe(nir_function, func, node, &nir->functions)
      {
         if (func->is_entrypoint)
            func->name = ralloc_strdup(func, "main");
         else
            exec_node_remove(&func->node);
      }
      assert(exec_list_length(&nir->functions) == 1);

      /* Make sure we lower constant initializers on output variables so that
       * nir_remove_dead_variables below sees the corresponding stores
       */
      NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_shader_out);

      /* Now that we've deleted all but the main function, we can go ahead and
       * lower the rest of the constant initializers.
       */
      NIR_PASS(_, nir, nir_lower_variable_initializers, ~0);

      /* Split member structs.  We do this before lower_io_to_temporaries so that
       * it doesn't lower system values to temporaries by accident.
       */
      NIR_PASS(_, nir, nir_split_var_copies);
      NIR_PASS(_, nir, nir_split_per_member_structs);

      if (nir->info.stage == MESA_SHADER_FRAGMENT)
         NIR_PASS(_, nir, nir_lower_io_to_vector, nir_var_shader_out);
      if (nir->info.stage == MESA_SHADER_FRAGMENT)
         NIR_PASS(_, nir, nir_lower_input_attachments,
                  &(nir_input_attachment_options){
                     .use_fragcoord_sysval = true,
                     .use_layer_id_sysval = false,
                  });

      nir_remove_dead_variables_options dead_vars_opts = {
         .can_remove_var = is_not_xfb_output,
      };
      NIR_PASS(_, nir, nir_remove_dead_variables,
               nir_var_shader_in | nir_var_shader_out | nir_var_system_value | nir_var_mem_shared,
               &dead_vars_opts);

      /* Variables can make nir_propagate_invariant more conservative
       * than it needs to be.
       */
      NIR_PASS(_, nir, nir_lower_global_vars_to_local);
      NIR_PASS(_, nir, nir_lower_vars_to_ssa);

      NIR_PASS(_, nir, nir_propagate_invariant, key->invariant_geom);

      NIR_PASS(_, nir, nir_lower_clip_cull_distance_arrays);

      if (nir->info.stage == MESA_SHADER_VERTEX ||
          nir->info.stage == MESA_SHADER_TESS_EVAL ||
          nir->info.stage == MESA_SHADER_GEOMETRY)
         NIR_PASS_V(nir, nir_shader_gather_xfb_info);

      NIR_PASS(_, nir, nir_lower_discard_or_demote, key->ps.lower_discard_to_demote);

      nir_lower_doubles_options lower_doubles = nir->options->lower_doubles_options;

      if (device->physical_device->rad_info.gfx_level == GFX6) {
         /* GFX6 doesn't support v_floor_f64 and the precision
          * of v_fract_f64 which is used to implement 64-bit
          * floor is less than what Vulkan requires.
          */
         lower_doubles |= nir_lower_dfloor;
      }

      NIR_PASS(_, nir, nir_lower_doubles, NULL, lower_doubles);

      NIR_PASS(_, nir, nir_shader_lower_instructions, &is_sincos, &lower_sincos, NULL);
   }

   NIR_PASS(_, nir, nir_lower_system_values);
   nir_lower_compute_system_values_options csv_options = {
      /* Mesh shaders run as NGG which can implement local_invocation_index from
       * the wave ID in merged_wave_info, but they don't have local_invocation_ids.
       */
      .lower_cs_local_id_to_index = nir->info.stage == MESA_SHADER_MESH,
      .lower_local_invocation_index = nir->info.stage == MESA_SHADER_COMPUTE &&
                                      ((nir->info.workgroup_size[0] == 1) +
                                       (nir->info.workgroup_size[1] == 1) +
                                       (nir->info.workgroup_size[2] == 1)) == 2,
   };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &csv_options);

   if (nir->info.stage == MESA_SHADER_MESH) {
      /* NV_mesh_shader: include first_task (aka. first_vertex) in workgroup ID. */
      NIR_PASS(_, nir, radv_lower_ms_workgroup_id);

      /* Mesh shaders only have a 1D "vertex index" which we use
       * as "workgroup index" to emulate the 3D workgroup ID.
       */
      nir_lower_compute_system_values_options o = {
         .lower_workgroup_id_to_index = true,
      };
      NIR_PASS(_, nir, nir_lower_compute_system_values, &o);
   }

   /* Vulkan uses the separate-shader linking model */
   nir->info.separate_shader = true;

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   if (nir->info.ray_queries > 0) {
      /* Lower shared variables early to prevent the over allocation of shared memory in
       * radv_nir_lower_ray_queries.  */
      if (nir->info.stage == MESA_SHADER_COMPUTE) {
         if (!nir->info.shared_memory_explicit_layout)
            NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared, shared_var_info);

         NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared,
                  nir_address_format_32bit_offset);
      }

      NIR_PASS(_, nir, nir_opt_ray_queries);
      NIR_PASS(_, nir, nir_opt_ray_query_ranges);
      NIR_PASS(_, nir, radv_nir_lower_ray_queries, device);
   }

   nir_lower_tex_options tex_options = {
      .lower_txp = ~0,
      .lower_txf_offset = true,
      .lower_tg4_offsets = true,
      .lower_txs_cube_array = true,
      .lower_to_fragment_fetch_amd = device->physical_device->use_fmask,
      .lower_lod_zero_width = true,
      .lower_invalid_implicit_lod = true,
      .lower_array_layer_round_even = true,
   };

   NIR_PASS(_, nir, nir_lower_tex, &tex_options);

   static const nir_lower_image_options image_options = {
      .lower_cube_size = true,
   };

   NIR_PASS(_, nir, nir_lower_image, &image_options);

   NIR_PASS(_, nir, nir_lower_vars_to_ssa);

   if (nir->info.stage == MESA_SHADER_VERTEX || nir->info.stage == MESA_SHADER_GEOMETRY ||
       nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS_V(nir, nir_lower_io_to_temporaries, nir_shader_get_entrypoint(nir), true, true);
   } else if (nir->info.stage == MESA_SHADER_TESS_EVAL) {
      NIR_PASS_V(nir, nir_lower_io_to_temporaries, nir_shader_get_entrypoint(nir), true, false);
   }

   NIR_PASS(_, nir, nir_split_var_copies);

   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_function_temp, NULL);
   bool gfx7minus = device->physical_device->rad_info.gfx_level <= GFX7;
   NIR_PASS(_, nir, nir_lower_subgroups,
            &(struct nir_lower_subgroups_options){
               .subgroup_size = subgroup_size,
               .ballot_bit_size = ballot_bit_size,
               .ballot_components = 1,
               .lower_to_scalar = 1,
               .lower_subgroup_masks = 1,
               .lower_relative_shuffle = 1,
               .lower_shuffle_to_32bit = 1,
               .lower_vote_eq = 1,
               .lower_quad_broadcast_dynamic = 1,
               .lower_quad_broadcast_dynamic_to_const = gfx7minus,
               .lower_shuffle_to_swizzle_amd = 1,
            });

   NIR_PASS(_, nir, nir_lower_load_const_to_scalar);
   NIR_PASS(_, nir, nir_opt_shrink_stores, !device->instance->disable_shrink_image_store);

   if (!key->optimisations_disabled)
      radv_optimize_nir(nir, false);

   /* We call nir_lower_var_copies() after the first radv_optimize_nir()
    * to remove any copies introduced by nir_opt_find_array_copies().
    */
   NIR_PASS(_, nir, nir_lower_var_copies);

   unsigned lower_flrp = (nir->options->lower_flrp16 ? 16 : 0) |
                         (nir->options->lower_flrp32 ? 32 : 0) |
                         (nir->options->lower_flrp64 ? 64 : 0);
   if (lower_flrp != 0) {
      bool progress = false;
      NIR_PASS(progress, nir, nir_lower_flrp, lower_flrp, false /* always precise */);
      if (progress)
         NIR_PASS(_, nir, nir_opt_constant_folding);
   }

   const nir_opt_access_options opt_access_options = {
      .is_vulkan = true,
   };
   NIR_PASS(_, nir, nir_opt_access, &opt_access_options);

   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const, nir_address_format_32bit_offset);

   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_vec2_index_32bit_offset);

   NIR_PASS(_, nir, lower_intrinsics, key);

   /* Lower deref operations for compute shared memory. */
   if (nir->info.stage == MESA_SHADER_COMPUTE ||
       nir->info.stage == MESA_SHADER_TASK ||
       nir->info.stage == MESA_SHADER_MESH) {
      nir_variable_mode var_modes = nir_var_mem_shared;

      if (nir->info.stage == MESA_SHADER_TASK ||
          nir->info.stage == MESA_SHADER_MESH)
         var_modes |= nir_var_mem_task_payload;

      if (!nir->info.shared_memory_explicit_layout)
         NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, var_modes, shared_var_info);
      else if (var_modes & ~nir_var_mem_shared)
         NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, var_modes & ~nir_var_mem_shared,
                  shared_var_info);
      NIR_PASS(_, nir, nir_lower_explicit_io, var_modes, nir_address_format_32bit_offset);

      if (nir->info.zero_initialize_shared_memory && nir->info.shared_size > 0) {
         const unsigned chunk_size = 16; /* max single store size */
         const unsigned shared_size = ALIGN(nir->info.shared_size, chunk_size);
         NIR_PASS(_, nir, nir_zero_initialize_shared_memory, shared_size, chunk_size);
      }
   }

   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global | nir_var_mem_constant,
            nir_address_format_64bit_global);

   /* Lower large variables that are always constant with load_constant
    * intrinsics, which get turned into PC-relative loads from a data
    * section next to the shader.
    */
   NIR_PASS(_, nir, nir_opt_large_constants, glsl_get_natural_size_align_bytes, 16);

   /* Lower primitive shading rate to match HW requirements. */
   if ((nir->info.stage == MESA_SHADER_VERTEX ||
        nir->info.stage == MESA_SHADER_GEOMETRY ||
        nir->info.stage == MESA_SHADER_MESH) &&
       nir->info.outputs_written & BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_SHADING_RATE)) {
      /* Lower primitive shading rate to match HW requirements. */
      NIR_PASS(_, nir, radv_lower_primitive_shading_rate,
               device->physical_device->rad_info.gfx_level);
   }

   /* Indirect lowering must be called after the radv_optimize_nir() loop
    * has been called at least once. Otherwise indirect lowering can
    * bloat the instruction count of the loop and cause it to be
    * considered too large for unrolling.
    */
   if (ac_nir_lower_indirect_derefs(nir, device->physical_device->rad_info.gfx_level) &&
       !key->optimisations_disabled && nir->info.stage != MESA_SHADER_COMPUTE) {
      /* Optimize the lowered code before the linking optimizations. */
      radv_optimize_nir(nir, false);
   }


   return nir;
}

static int
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static nir_variable *
find_layer_in_var(nir_shader *nir)
{
   nir_variable *var = nir_find_variable_with_location(nir, nir_var_shader_in, VARYING_SLOT_LAYER);
   if (var != NULL)
      return var;

   var = nir_variable_create(nir, nir_var_shader_in, glsl_int_type(), "layer id");
   var->data.location = VARYING_SLOT_LAYER;
   var->data.interpolation = INTERP_MODE_FLAT;
   return var;
}

/* We use layered rendering to implement multiview, which means we need to map
 * view_index to gl_Layer. The code generates a load from the layer_id sysval,
 * but since we don't have a way to get at this information from the fragment
 * shader, we also need to lower this to the gl_Layer varying.  This pass
 * lowers both to a varying load from the LAYER slot, before lowering io, so
 * that nir_assign_var_locations() will give the LAYER varying the correct
 * driver_location.
 */

bool
radv_lower_view_index(nir_shader *nir, bool per_primitive)
{
   bool progress = false;
   nir_function_impl *entry = nir_shader_get_entrypoint(nir);
   nir_builder b;
   nir_builder_init(&b, entry);

   nir_variable *layer = NULL;
   nir_foreach_block (block, entry) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
         if (load->intrinsic != nir_intrinsic_load_view_index)
            continue;

         if (!layer)
            layer = find_layer_in_var(nir);

         layer->data.per_primitive = per_primitive;
         b.cursor = nir_before_instr(instr);
         nir_ssa_def *def = nir_load_var(&b, layer);
         nir_ssa_def_rewrite_uses(&load->dest.ssa, def);

         /* Update inputs_read to reflect that the pass added a new input. */
         nir->info.inputs_read |= VARYING_BIT_LAYER;
         if (per_primitive)
            nir->info.per_primitive_inputs |= VARYING_BIT_LAYER;

         nir_instr_remove(instr);
         progress = true;
      }
   }

   if (progress)
      nir_metadata_preserve(entry, nir_metadata_block_index | nir_metadata_dominance);
   else
      nir_metadata_preserve(entry, nir_metadata_all);

   return progress;
}

void
radv_lower_io(struct radv_device *device, nir_shader *nir)
{
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      nir_assign_io_var_locations(nir, nir_var_shader_in, &nir->num_inputs, MESA_SHADER_FRAGMENT);
   }

   if (nir->info.stage == MESA_SHADER_VERTEX) {
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in, type_size_vec4, 0);
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_out, type_size_vec4,
               nir_lower_io_lower_64bit_to_32);
   } else {
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out, type_size_vec4,
               nir_lower_io_lower_64bit_to_32);
   }

   /* This pass needs actual constants */
   NIR_PASS(_, nir, nir_opt_constant_folding);

   NIR_PASS(_, nir, nir_io_add_const_offset_to_base, nir_var_shader_in | nir_var_shader_out);

   if (device->physical_device->use_ngg_streamout && nir->xfb_info) {
      NIR_PASS_V(nir, nir_io_add_intrinsic_xfb_info);

      /* The total number of shader outputs is required for computing the pervertex LDS size for
       * VS/TES when lowering NGG streamout.
       */
      nir_assign_io_var_locations(nir, nir_var_shader_out, &nir->num_outputs, nir->info.stage);
   }
}

bool
radv_lower_io_to_mem(struct radv_device *device, struct radv_pipeline_stage *stage)
{
   const struct radv_shader_info *info = &stage->info;
   nir_shader *nir = stage->nir;

   if (nir->info.stage == MESA_SHADER_VERTEX) {
      if (info->vs.as_ls) {
         NIR_PASS_V(nir, ac_nir_lower_ls_outputs_to_mem, NULL, info->vs.tcs_in_out_eq,
                    info->vs.tcs_temp_only_input_mask);
         return true;
      } else if (info->vs.as_es) {
         NIR_PASS_V(nir, ac_nir_lower_es_outputs_to_mem, NULL,
                    device->physical_device->rad_info.gfx_level, info->esgs_itemsize);
         return true;
      }
   } else if (nir->info.stage == MESA_SHADER_TESS_CTRL) {
      NIR_PASS_V(nir, ac_nir_lower_hs_inputs_to_mem, NULL, info->vs.tcs_in_out_eq);
      NIR_PASS_V(nir, ac_nir_lower_hs_outputs_to_mem, NULL,
                 device->physical_device->rad_info.gfx_level,
                 info->tcs.tes_reads_tess_factors, info->tcs.tes_inputs_read,
                 info->tcs.tes_patch_inputs_read, info->tcs.num_linked_outputs,
                 info->tcs.num_linked_patch_outputs, info->wave_size,
                 false, false, true);

      return true;
   } else if (nir->info.stage == MESA_SHADER_TESS_EVAL) {
      NIR_PASS_V(nir, ac_nir_lower_tes_inputs_to_mem, NULL);

      if (info->tes.as_es) {
         NIR_PASS_V(nir, ac_nir_lower_es_outputs_to_mem, NULL,
                    device->physical_device->rad_info.gfx_level, info->esgs_itemsize);
      }

      return true;
   } else if (nir->info.stage == MESA_SHADER_GEOMETRY) {
      NIR_PASS_V(nir, ac_nir_lower_gs_inputs_to_mem, NULL,
                 device->physical_device->rad_info.gfx_level, false);
      return true;
   } else if (nir->info.stage == MESA_SHADER_TASK) {
      ac_nir_apply_first_task_to_task_shader(nir);
      ac_nir_lower_task_outputs_to_mem(nir, AC_TASK_PAYLOAD_ENTRY_BYTES,
                                       device->physical_device->task_info.num_entries);
      return true;
   } else if (nir->info.stage == MESA_SHADER_MESH) {
      ac_nir_lower_mesh_inputs_to_mem(nir, AC_TASK_PAYLOAD_ENTRY_BYTES,
                                      device->physical_device->task_info.num_entries);
      return true;
   }

   return false;
}

bool
radv_consider_culling(const struct radv_physical_device *pdevice, struct nir_shader *nir, uint64_t ps_inputs_read,
                      unsigned num_vertices_per_primitive, const struct radv_shader_info *info)
{
   /* Culling doesn't make sense for meta shaders. */
   if (is_meta_shader(nir))
      return false;

   /* We don't support culling with multiple viewports yet. */
   if (nir->info.outputs_written & (VARYING_BIT_VIEWPORT | VARYING_BIT_VIEWPORT_MASK))
      return false;

   /* We don't support culling with vertex shader prologs. */
   if (info->vs.has_prolog)
      return false;

   if (!pdevice->use_ngg_culling)
      return false;

   /* Shader based culling efficiency can depend on PS throughput.
    * Estimate an upper limit for PS input param count based on GPU info.
    */
   unsigned max_ps_params;
   unsigned max_render_backends = pdevice->rad_info.max_render_backends;
   unsigned max_se = pdevice->rad_info.max_se;

   if (max_render_backends / max_se == 4)
      max_ps_params = 6; /* Navi21 and other GFX10.3 dGPUs. */
   else
      max_ps_params = 4; /* Navi 1x. */

   /* TODO: consider other heuristics here, such as PS execution time */
   if (util_bitcount64(ps_inputs_read & ~VARYING_BIT_POS) > max_ps_params)
      return false;

   /* Only triangle culling is supported. */
   if (num_vertices_per_primitive != 3)
      return false;

   /* When the shader writes memory, it is difficult to guarantee correctness.
    * Future work:
    * - if only write-only SSBOs are used
    * - if we can prove that non-position outputs don't rely on memory stores
    * then may be okay to keep the memory stores in the 1st shader part, and delete them from the 2nd.
    */
   if (nir->info.writes_memory)
      return false;

   /* When the shader relies on the subgroup invocation ID, we'd break it, because the ID changes after the culling.
    * Future work: try to save this to LDS and reload, but it can still be broken in subtle ways.
    */
   if (BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SUBGROUP_INVOCATION))
      return false;

   return true;
}

static void
setup_ngg_lds_layout(struct radv_device *device, nir_shader *nir, struct radv_shader_info *info,
                     unsigned max_vtx_in)
{
   unsigned scratch_lds_base = 0;
   gl_shader_stage stage = nir->info.stage;

   if (stage == MESA_SHADER_VERTEX || stage == MESA_SHADER_TESS_EVAL) {
      /* Get pervertex LDS usage. */
      bool uses_instanceid =
         BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_INSTANCE_ID);
      bool uses_primtive_id =
         BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_PRIMITIVE_ID);
      bool streamout_enabled = nir->xfb_info && device->physical_device->use_ngg_streamout;
      unsigned pervertex_lds_bytes =
         ac_ngg_nogs_get_pervertex_lds_size(stage,
                                            nir->num_outputs,
                                            streamout_enabled,
                                            info->outinfo.export_prim_id,
                                            false, /* user edge flag */
                                            info->has_ngg_culling,
                                            uses_instanceid,
                                            uses_primtive_id);

      unsigned total_es_lds_bytes = pervertex_lds_bytes * max_vtx_in;
      scratch_lds_base = ALIGN(total_es_lds_bytes, 8u);
   } else if (stage == MESA_SHADER_GEOMETRY) {
      unsigned esgs_ring_lds_bytes = info->ngg_info.esgs_ring_size;
      unsigned gs_total_out_vtx_bytes = info->ngg_info.ngg_emit_size * 4u;
      scratch_lds_base =
         ALIGN(esgs_ring_lds_bytes + gs_total_out_vtx_bytes, 8u /* for the repacking code */);
   } else {
      /* not handled here */
      return;
   }

   /* Get scratch LDS usage. */
   unsigned scratch_lds_size =
      ac_ngg_get_scratch_lds_size(stage,
                                  info->workgroup_size,
                                  info->wave_size,
                                  device->physical_device->use_ngg_streamout,
                                  info->has_ngg_culling);

   /* Get total LDS usage. */
   nir->info.shared_size = scratch_lds_base + scratch_lds_size;

   /* Record scratch base for abi lower of nir_load_lds_ngg_scratch_base_amd. */
   info->ngg_info.scratch_lds_base = scratch_lds_base;
}

void radv_lower_ngg(struct radv_device *device, struct radv_pipeline_stage *ngg_stage,
                    const struct radv_pipeline_key *pl_key)
{
   const struct radv_shader_info *info = &ngg_stage->info;
   nir_shader *nir = ngg_stage->nir;

   assert(nir->info.stage == MESA_SHADER_VERTEX ||
          nir->info.stage == MESA_SHADER_TESS_EVAL ||
          nir->info.stage == MESA_SHADER_GEOMETRY ||
          nir->info.stage == MESA_SHADER_MESH);

   const struct gfx10_ngg_info *ngg_info = &info->ngg_info;
   unsigned num_vertices_per_prim = 3;

   /* Get the number of vertices per input primitive */
   if (nir->info.stage == MESA_SHADER_TESS_EVAL) {
      if (nir->info.tess.point_mode)
         num_vertices_per_prim = 1;
      else if (nir->info.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES)
         num_vertices_per_prim = 2;

      /* Manually mark the primitive ID used, so the shader can repack it. */
      if (info->outinfo.export_prim_id)
         BITSET_SET(nir->info.system_values_read, SYSTEM_VALUE_PRIMITIVE_ID);

   } else if (nir->info.stage == MESA_SHADER_VERTEX) {
      num_vertices_per_prim = radv_get_num_vertices_per_prim(pl_key);

      /* Manually mark the instance ID used, so the shader can repack it. */
      if (pl_key->vs.instance_rate_inputs)
         BITSET_SET(nir->info.system_values_read, SYSTEM_VALUE_INSTANCE_ID);

   } else if (nir->info.stage == MESA_SHADER_GEOMETRY) {
      num_vertices_per_prim = nir->info.gs.vertices_in;
   } else if (nir->info.stage == MESA_SHADER_MESH) {
      if (nir->info.mesh.primitive_type == SHADER_PRIM_POINTS)
         num_vertices_per_prim = 1;
      else if (nir->info.mesh.primitive_type == SHADER_PRIM_LINES)
         num_vertices_per_prim = 2;
      else
         assert(nir->info.mesh.primitive_type == SHADER_PRIM_TRIANGLES);
   } else {
      unreachable("NGG needs to be VS, TES or GS.");
   }

   /* Invocations that process an input vertex */
   unsigned max_vtx_in = MIN2(256, ngg_info->hw_max_esverts);

   setup_ngg_lds_layout(device, nir, &ngg_stage->info, max_vtx_in);

   ac_nir_lower_ngg_options options = {0};
   options.family = device->physical_device->rad_info.family;
   options.gfx_level = device->physical_device->rad_info.gfx_level;
   options.max_workgroup_size = info->workgroup_size;
   options.wave_size = info->wave_size;
   options.clipdist_enable_mask = info->outinfo.clip_dist_mask | info->outinfo.cull_dist_mask;
   options.vs_output_param_offset = info->outinfo.vs_output_param_offset;
   options.has_param_exports = info->outinfo.param_exports;
   options.can_cull = nir->info.stage != MESA_SHADER_GEOMETRY && info->has_ngg_culling;
   options.disable_streamout = !device->physical_device->use_ngg_streamout;
   options.has_gen_prim_query = info->has_ngg_prim_query;
   options.has_xfb_prim_query = info->has_ngg_xfb_query;
   options.force_vrs = info->force_vrs_per_vertex;

   if (nir->info.stage == MESA_SHADER_VERTEX ||
       nir->info.stage == MESA_SHADER_TESS_EVAL) {
      assert(info->is_ngg);

      if (info->has_ngg_culling)
         radv_optimize_nir_algebraic(nir, false);

      options.num_vertices_per_primitive = num_vertices_per_prim;
      options.early_prim_export = info->has_ngg_early_prim_export;
      options.passthrough = info->is_ngg_passthrough;
      options.export_primitive_id = info->outinfo.export_prim_id;
      options.instance_rate_inputs = pl_key->vs.instance_rate_inputs << VERT_ATTRIB_GENERIC0;

      NIR_PASS_V(nir, ac_nir_lower_ngg_nogs, &options);

      /* Increase ESGS ring size so the LLVM binary contains the correct LDS size. */
      ngg_stage->info.ngg_info.esgs_ring_size = nir->info.shared_size;
   } else if (nir->info.stage == MESA_SHADER_GEOMETRY) {
      assert(info->is_ngg);

      options.gs_out_vtx_bytes = info->gs.gsvs_vertex_size;

      NIR_PASS_V(nir, ac_nir_lower_ngg_gs, &options);
   } else if (nir->info.stage == MESA_SHADER_MESH) {
      bool scratch_ring = false;
      NIR_PASS_V(nir, ac_nir_lower_ngg_ms,
                 options.gfx_level,
                 options.clipdist_enable_mask,
                 options.vs_output_param_offset,
                 options.has_param_exports,
                 &scratch_ring,
                 info->wave_size,
                 pl_key->has_multiview_view_index);
      ngg_stage->info.ms.needs_ms_scratch_ring = scratch_ring;
   } else {
      unreachable("invalid SW stage passed to radv_lower_ngg");
   }
}

static unsigned
get_size_class(unsigned size, bool round_up)
{
   size = round_up ? util_logbase2_ceil(size) : util_logbase2(size);
   unsigned size_class =
      MAX2(size, RADV_SHADER_ALLOC_MIN_SIZE_CLASS) - RADV_SHADER_ALLOC_MIN_SIZE_CLASS;
   return MIN2(size_class, RADV_SHADER_ALLOC_NUM_FREE_LISTS - 1);
}

static void
remove_hole(struct radv_device *device, union radv_shader_arena_block *hole)
{
   unsigned size_class = get_size_class(hole->size, false);
   list_del(&hole->freelist);
   if (list_is_empty(&device->shader_free_lists[size_class]))
      device->shader_free_list_mask &= ~(1u << size_class);
}

static void
add_hole(struct radv_device *device, union radv_shader_arena_block *hole)
{
   unsigned size_class = get_size_class(hole->size, false);
   list_addtail(&hole->freelist, &device->shader_free_lists[size_class]);
   device->shader_free_list_mask |= 1u << size_class;
}

static union radv_shader_arena_block *
alloc_block_obj(struct radv_device *device)
{
   if (!list_is_empty(&device->shader_block_obj_pool)) {
      union radv_shader_arena_block *block =
         list_first_entry(&device->shader_block_obj_pool, union radv_shader_arena_block, pool);
      list_del(&block->pool);
      return block;
   }

   return malloc(sizeof(union radv_shader_arena_block));
}

static void
free_block_obj(struct radv_device *device, union radv_shader_arena_block *block)
{
   list_add(&block->pool, &device->shader_block_obj_pool);
}

/* Segregated fit allocator, implementing a good-fit allocation policy.
 *
 * This is an variation of sequential fit allocation with several lists of free blocks ("holes")
 * instead of one. Each list of holes only contains holes of a certain range of sizes, so holes that
 * are too small can easily be ignored while allocating. Because this also ignores holes that are
 * larger than necessary (approximating best-fit allocation), this could be described as a
 * "good-fit" allocator.
 *
 * Typically, shaders are allocated and only free'd when the device is destroyed. For this pattern,
 * this should allocate blocks for shaders fast and with no fragmentation, while still allowing
 * free'd memory to be re-used.
 */
union radv_shader_arena_block *
radv_alloc_shader_memory(struct radv_device *device, uint32_t size, void *ptr)
{
   size = align(size, RADV_SHADER_ALLOC_ALIGNMENT);

   mtx_lock(&device->shader_arena_mutex);

   /* Try to use an existing hole. Unless the shader is very large, this should only have to look
    * at the first one available.
    */
   unsigned free_list_mask = BITFIELD_MASK(RADV_SHADER_ALLOC_NUM_FREE_LISTS);
   unsigned size_class =
      ffs(device->shader_free_list_mask & (free_list_mask << get_size_class(size, true)));
   if (size_class) {
      size_class--;

      list_for_each_entry(union radv_shader_arena_block, hole,
                          &device->shader_free_lists[size_class], freelist)
      {
         if (hole->size < size)
            continue;

         assert(hole->offset % RADV_SHADER_ALLOC_ALIGNMENT == 0);

         if (size == hole->size) {
            remove_hole(device, hole);
            hole->freelist.next = ptr;
            mtx_unlock(&device->shader_arena_mutex);
            return hole;
         } else {
            union radv_shader_arena_block *alloc = alloc_block_obj(device);
            if (!alloc) {
               mtx_unlock(&device->shader_arena_mutex);
               return NULL;
            }
            list_addtail(&alloc->list, &hole->list);
            alloc->freelist.prev = NULL;
            alloc->freelist.next = ptr;
            alloc->arena = hole->arena;
            alloc->offset = hole->offset;
            alloc->size = size;

            remove_hole(device, hole);
            hole->offset += size;
            hole->size -= size;
            add_hole(device, hole);

            mtx_unlock(&device->shader_arena_mutex);
            return alloc;
         }
      }
   }

   /* Allocate a new shader arena. */
   struct radv_shader_arena *arena = calloc(1, sizeof(struct radv_shader_arena));
   union radv_shader_arena_block *alloc = NULL, *hole = NULL;
   if (!arena)
      goto fail;

   unsigned arena_size =
      MAX2(RADV_SHADER_ALLOC_MIN_ARENA_SIZE
              << MIN2(RADV_SHADER_ALLOC_MAX_ARENA_SIZE_SHIFT, device->shader_arena_shift),
           size);
   VkResult result = device->ws->buffer_create(
      device->ws, arena_size, RADV_SHADER_ALLOC_ALIGNMENT, RADEON_DOMAIN_VRAM,
      RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_32BIT |
         (device->physical_device->rad_info.cpdma_prefetch_writes_memory ? 0
                                                                         : RADEON_FLAG_READ_ONLY),
      RADV_BO_PRIORITY_SHADER, 0, &arena->bo);
   if (result != VK_SUCCESS)
      goto fail;
   radv_rmv_log_bo_allocate(device, arena->bo, arena_size, true);

   list_inithead(&arena->entries);

   arena->ptr = (char *)device->ws->buffer_map(arena->bo);
   if (!arena->ptr)
      goto fail;

   alloc = alloc_block_obj(device);
   hole = arena_size - size > 0 ? alloc_block_obj(device) : alloc;
   if (!alloc || !hole)
      goto fail;
   list_addtail(&alloc->list, &arena->entries);
   alloc->freelist.prev = NULL;
   alloc->freelist.next = ptr;
   alloc->arena = arena;
   alloc->offset = 0;
   alloc->size = size;

   if (hole != alloc) {
      hole->arena = arena;
      hole->offset = size;
      hole->size = arena_size - size;

      list_addtail(&hole->list, &arena->entries);
      add_hole(device, hole);
   }

   ++device->shader_arena_shift;
   list_addtail(&arena->list, &device->shader_arenas);

   mtx_unlock(&device->shader_arena_mutex);
   return alloc;

fail:
   mtx_unlock(&device->shader_arena_mutex);
   free(alloc);
   free(hole);
   if (arena && arena->bo) {
      radv_rmv_log_bo_destroy(device, arena->bo);
      device->ws->buffer_destroy(device->ws, arena->bo);
   }
   free(arena);
   return NULL;
}

static union radv_shader_arena_block *
get_hole(struct radv_shader_arena *arena, struct list_head *head)
{
   if (head == &arena->entries)
      return NULL;

   union radv_shader_arena_block *hole = list_entry(head, union radv_shader_arena_block, list);
   return hole->freelist.prev ? hole : NULL;
}

void
radv_free_shader_memory(struct radv_device *device, union radv_shader_arena_block *alloc)
{
   mtx_lock(&device->shader_arena_mutex);

   union radv_shader_arena_block *hole_prev = get_hole(alloc->arena, alloc->list.prev);
   union radv_shader_arena_block *hole_next = get_hole(alloc->arena, alloc->list.next);

   union radv_shader_arena_block *hole = alloc;

   /* merge with previous hole */
   if (hole_prev) {
      remove_hole(device, hole_prev);

      hole_prev->size += hole->size;
      list_del(&hole->list);
      free_block_obj(device, hole);

      hole = hole_prev;
   }

   /* merge with next hole */
   if (hole_next) {
      remove_hole(device, hole_next);

      hole_next->offset -= hole->size;
      hole_next->size += hole->size;
      list_del(&hole->list);
      free_block_obj(device, hole);

      hole = hole_next;
   }

   if (list_is_singular(&hole->list)) {
      struct radv_shader_arena *arena = hole->arena;
      free_block_obj(device, hole);

      radv_rmv_log_bo_destroy(device, arena->bo);
      device->ws->buffer_destroy(device->ws, arena->bo);
      list_del(&arena->list);
      free(arena);
   } else {
      add_hole(device, hole);
   }

   mtx_unlock(&device->shader_arena_mutex);
}

void
radv_init_shader_arenas(struct radv_device *device)
{
   mtx_init(&device->shader_arena_mutex, mtx_plain);

   device->shader_free_list_mask = 0;

   list_inithead(&device->shader_arenas);
   list_inithead(&device->shader_block_obj_pool);
   for (unsigned i = 0; i < RADV_SHADER_ALLOC_NUM_FREE_LISTS; i++)
      list_inithead(&device->shader_free_lists[i]);
}

void
radv_destroy_shader_arenas(struct radv_device *device)
{
   list_for_each_entry_safe(union radv_shader_arena_block, block, &device->shader_block_obj_pool,
                            pool) free(block);

   list_for_each_entry_safe(struct radv_shader_arena, arena, &device->shader_arenas, list)
   {
      radv_rmv_log_bo_destroy(device, arena->bo);
      device->ws->buffer_destroy(device->ws, arena->bo);
      free(arena);
   }
   mtx_destroy(&device->shader_arena_mutex);
}

/* For the UMR disassembler. */
#define DEBUGGER_END_OF_CODE_MARKER 0xbf9f0000 /* invalid instruction */
#define DEBUGGER_NUM_MARKERS        5

static unsigned
radv_get_shader_binary_size(size_t code_size)
{
   return code_size + DEBUGGER_NUM_MARKERS * 4;
}

static bool
radv_should_use_wgp_mode(const struct radv_device *device, gl_shader_stage stage,
                         const struct radv_shader_info *info)
{
   enum amd_gfx_level chip = device->physical_device->rad_info.gfx_level;
   switch (stage) {
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_TESS_CTRL:
      return chip >= GFX10;
   case MESA_SHADER_GEOMETRY:
      return chip == GFX10 || (chip >= GFX10_3 && !info->is_ngg);
   case MESA_SHADER_VERTEX:
   case MESA_SHADER_TESS_EVAL:
      return chip == GFX10 && info->is_ngg;
   default:
      return false;
   }
}

static void
radv_postprocess_config(const struct radv_device *device, const struct ac_shader_config *config_in,
                        const struct radv_shader_info *info, gl_shader_stage stage,
                        const struct radv_shader_args *args,
                        struct ac_shader_config *config_out)
{
   const struct radv_physical_device *pdevice = device->physical_device;
   bool scratch_enabled = config_in->scratch_bytes_per_wave > 0;
   bool trap_enabled = !!device->trap_handler_shader;
   unsigned vgpr_comp_cnt = 0;
   unsigned num_input_vgprs = args->ac.num_vgprs_used;

   if (stage == MESA_SHADER_FRAGMENT) {
      num_input_vgprs = ac_get_fs_input_vgpr_cnt(config_in, NULL, NULL, NULL);
   }

   unsigned num_vgprs = MAX2(config_in->num_vgprs, num_input_vgprs);
   /* +2 for the ring offsets, +3 for scratch wave offset and VCC */
   unsigned num_sgprs = MAX2(config_in->num_sgprs, args->ac.num_sgprs_used + 2 + 3);
   unsigned num_shared_vgprs = config_in->num_shared_vgprs;
   /* shared VGPRs are introduced in Navi and are allocated in blocks of 8 (RDNA ref 3.6.5) */
   assert((pdevice->rad_info.gfx_level >= GFX10 && num_shared_vgprs % 8 == 0) ||
          (pdevice->rad_info.gfx_level < GFX10 && num_shared_vgprs == 0));
   unsigned num_shared_vgpr_blocks = num_shared_vgprs / 8;
   unsigned excp_en = 0;

   *config_out = *config_in;
   config_out->num_vgprs = num_vgprs;
   config_out->num_sgprs = num_sgprs;
   config_out->num_shared_vgprs = num_shared_vgprs;

   config_out->rsrc2 = S_00B12C_USER_SGPR(args->num_user_sgprs) |
                       S_00B12C_SCRATCH_EN(scratch_enabled) | S_00B12C_TRAP_PRESENT(trap_enabled);

   if (trap_enabled) {
      /* Configure the shader exceptions like memory violation, etc.
       * TODO: Enable (and validate) more exceptions.
       */
      excp_en = 1 << 8; /* mem_viol */
   }

   if (!pdevice->use_ngg_streamout) {
      config_out->rsrc2 |=
         S_00B12C_SO_BASE0_EN(!!info->so.strides[0]) | S_00B12C_SO_BASE1_EN(!!info->so.strides[1]) |
         S_00B12C_SO_BASE2_EN(!!info->so.strides[2]) | S_00B12C_SO_BASE3_EN(!!info->so.strides[3]) |
         S_00B12C_SO_EN(!!info->so.num_outputs);
   }

   config_out->rsrc1 = S_00B848_VGPRS((num_vgprs - 1) / (info->wave_size == 32 ? 8 : 4)) |
                       S_00B848_DX10_CLAMP(1) | S_00B848_FLOAT_MODE(config_out->float_mode);

   if (pdevice->rad_info.gfx_level >= GFX10) {
      config_out->rsrc2 |= S_00B22C_USER_SGPR_MSB_GFX10(args->num_user_sgprs >> 5);
   } else {
      config_out->rsrc1 |= S_00B228_SGPRS((num_sgprs - 1) / 8);
      config_out->rsrc2 |= S_00B22C_USER_SGPR_MSB_GFX9(args->num_user_sgprs >> 5);
   }

   bool wgp_mode = radv_should_use_wgp_mode(device, stage, info);

   switch (stage) {
   case MESA_SHADER_TESS_EVAL:
      if (info->is_ngg) {
         config_out->rsrc1 |= S_00B228_MEM_ORDERED(pdevice->rad_info.gfx_level >= GFX10);
         config_out->rsrc2 |= S_00B22C_OC_LDS_EN(1) | S_00B22C_EXCP_EN(excp_en);
      } else if (info->tes.as_es) {
         assert(pdevice->rad_info.gfx_level <= GFX8);
         vgpr_comp_cnt = info->uses_prim_id ? 3 : 2;

         config_out->rsrc2 |= S_00B12C_OC_LDS_EN(1) | S_00B12C_EXCP_EN(excp_en);
      } else {
         bool enable_prim_id = info->outinfo.export_prim_id || info->uses_prim_id;
         vgpr_comp_cnt = enable_prim_id ? 3 : 2;

         config_out->rsrc1 |= S_00B128_MEM_ORDERED(pdevice->rad_info.gfx_level >= GFX10);
         config_out->rsrc2 |= S_00B12C_OC_LDS_EN(1) | S_00B12C_EXCP_EN(excp_en);
      }
      config_out->rsrc2 |= S_00B22C_SHARED_VGPR_CNT(num_shared_vgpr_blocks);
      break;
   case MESA_SHADER_TESS_CTRL:
      if (pdevice->rad_info.gfx_level >= GFX9) {
         /* We need at least 2 components for LS.
          * VGPR0-3: (VertexID, RelAutoindex, InstanceID / StepRate0, InstanceID).
          * StepRate0 is set to 1. so that VGPR3 doesn't have to be loaded.
          */
         if (pdevice->rad_info.gfx_level >= GFX10) {
            if (info->vs.needs_instance_id) {
               vgpr_comp_cnt = 3;
            } else if (pdevice->rad_info.gfx_level <= GFX10_3) {
               vgpr_comp_cnt = 1;
            }
            config_out->rsrc2 |= S_00B42C_EXCP_EN_GFX6(excp_en);
         } else {
            vgpr_comp_cnt = info->vs.needs_instance_id ? 2 : 1;
            config_out->rsrc2 |= S_00B42C_EXCP_EN_GFX9(excp_en);
         }
      } else {
         config_out->rsrc2 |= S_00B12C_OC_LDS_EN(1) | S_00B12C_EXCP_EN(excp_en);
      }
      config_out->rsrc1 |=
         S_00B428_MEM_ORDERED(pdevice->rad_info.gfx_level >= GFX10) | S_00B428_WGP_MODE(wgp_mode);
      config_out->rsrc2 |= S_00B42C_SHARED_VGPR_CNT(num_shared_vgpr_blocks);
      break;
   case MESA_SHADER_VERTEX:
      if (info->is_ngg) {
         config_out->rsrc1 |= S_00B228_MEM_ORDERED(pdevice->rad_info.gfx_level >= GFX10);
      } else if (info->vs.as_ls) {
         assert(pdevice->rad_info.gfx_level <= GFX8);
         /* We need at least 2 components for LS.
          * VGPR0-3: (VertexID, RelAutoindex, InstanceID / StepRate0, InstanceID).
          * StepRate0 is set to 1. so that VGPR3 doesn't have to be loaded.
          */
         vgpr_comp_cnt = info->vs.needs_instance_id ? 2 : 1;
      } else if (info->vs.as_es) {
         assert(pdevice->rad_info.gfx_level <= GFX8);
         /* VGPR0-3: (VertexID, InstanceID / StepRate0, ...) */
         vgpr_comp_cnt = info->vs.needs_instance_id ? 1 : 0;
      } else {
         /* VGPR0-3: (VertexID, InstanceID / StepRate0, PrimID, InstanceID)
          * If PrimID is disabled. InstanceID / StepRate1 is loaded instead.
          * StepRate0 is set to 1. so that VGPR3 doesn't have to be loaded.
          */
         if (info->vs.needs_instance_id && pdevice->rad_info.gfx_level >= GFX10) {
            vgpr_comp_cnt = 3;
         } else if (info->outinfo.export_prim_id) {
            vgpr_comp_cnt = 2;
         } else if (info->vs.needs_instance_id) {
            vgpr_comp_cnt = 1;
         } else {
            vgpr_comp_cnt = 0;
         }

         config_out->rsrc1 |= S_00B128_MEM_ORDERED(pdevice->rad_info.gfx_level >= GFX10);
      }
      config_out->rsrc2 |=
         S_00B12C_SHARED_VGPR_CNT(num_shared_vgpr_blocks) | S_00B12C_EXCP_EN(excp_en);
      break;
   case MESA_SHADER_MESH:
      config_out->rsrc1 |= S_00B228_MEM_ORDERED(1);
      config_out->rsrc2 |=
         S_00B12C_SHARED_VGPR_CNT(num_shared_vgpr_blocks) | S_00B12C_EXCP_EN(excp_en);
      break;
   case MESA_SHADER_FRAGMENT:
      config_out->rsrc1 |= S_00B028_MEM_ORDERED(pdevice->rad_info.gfx_level >= GFX10);
      config_out->rsrc2 |= S_00B02C_SHARED_VGPR_CNT(num_shared_vgpr_blocks) |
                           S_00B02C_EXCP_EN(excp_en);
      break;
   case MESA_SHADER_GEOMETRY:
      config_out->rsrc1 |= S_00B228_MEM_ORDERED(pdevice->rad_info.gfx_level >= GFX10);
      config_out->rsrc2 |=
         S_00B22C_SHARED_VGPR_CNT(num_shared_vgpr_blocks) | S_00B22C_EXCP_EN(excp_en);
      break;
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_TASK:
      config_out->rsrc1 |=
         S_00B848_MEM_ORDERED(pdevice->rad_info.gfx_level >= GFX10) | S_00B848_WGP_MODE(wgp_mode);
      config_out->rsrc2 |= S_00B84C_TGID_X_EN(info->cs.uses_block_id[0]) |
                           S_00B84C_TGID_Y_EN(info->cs.uses_block_id[1]) |
                           S_00B84C_TGID_Z_EN(info->cs.uses_block_id[2]) |
                           S_00B84C_TIDIG_COMP_CNT(info->cs.uses_thread_id[2]   ? 2
                                                   : info->cs.uses_thread_id[1] ? 1
                                                                                : 0) |
                           S_00B84C_TG_SIZE_EN(info->cs.uses_local_invocation_idx) |
                           S_00B84C_LDS_SIZE(config_in->lds_size) | S_00B84C_EXCP_EN(excp_en);
      config_out->rsrc3 |= S_00B8A0_SHARED_VGPR_CNT(num_shared_vgpr_blocks);

      break;
   default:
      unreachable("unsupported shader type");
      break;
   }

   if (pdevice->rad_info.gfx_level >= GFX10 && info->is_ngg &&
       (stage == MESA_SHADER_VERTEX || stage == MESA_SHADER_TESS_EVAL ||
        stage == MESA_SHADER_GEOMETRY || stage == MESA_SHADER_MESH)) {
      unsigned gs_vgpr_comp_cnt, es_vgpr_comp_cnt;
      gl_shader_stage es_stage = stage;
      if (stage == MESA_SHADER_GEOMETRY)
         es_stage = info->gs.es_type;

      /* VGPR5-8: (VertexID, UserVGPR0, UserVGPR1, UserVGPR2 / InstanceID) */
      if (es_stage == MESA_SHADER_VERTEX) {
         es_vgpr_comp_cnt = info->vs.needs_instance_id ? 3 : 0;
      } else if (es_stage == MESA_SHADER_TESS_EVAL) {
         bool enable_prim_id = info->outinfo.export_prim_id || info->uses_prim_id;
         es_vgpr_comp_cnt = enable_prim_id ? 3 : 2;
      } else if (es_stage == MESA_SHADER_MESH) {
         es_vgpr_comp_cnt = 0;
      } else {
         unreachable("Unexpected ES shader stage");
      }

      /* GS vertex offsets in NGG:
       * - in passthrough mode, they are all packed into VGPR0
       * - in the default mode: VGPR0: offsets 0, 1; VGPR1: offsets 2, 3
       *
       * The vertex offset 2 is always needed when NGG isn't in passthrough mode
       * and uses triangle input primitives, including with NGG culling.
       */
      bool need_gs_vtx_offset2 = !info->is_ngg_passthrough || info->gs.vertices_in >= 3;

      /* TES only needs vertex offset 2 for triangles or quads. */
      if (stage == MESA_SHADER_TESS_EVAL)
         need_gs_vtx_offset2 &= info->tes._primitive_mode == TESS_PRIMITIVE_TRIANGLES ||
                                info->tes._primitive_mode == TESS_PRIMITIVE_QUADS;

      if (info->uses_invocation_id) {
         gs_vgpr_comp_cnt = 3; /* VGPR3 contains InvocationID. */
      } else if (info->uses_prim_id || (es_stage == MESA_SHADER_VERTEX &&
                                        info->outinfo.export_prim_id)) {
         gs_vgpr_comp_cnt = 2; /* VGPR2 contains PrimitiveID. */
      } else if (need_gs_vtx_offset2) {
         gs_vgpr_comp_cnt = 1; /* VGPR1 contains offsets 2, 3 */
      } else {
         gs_vgpr_comp_cnt = 0; /* VGPR0 contains offsets 0, 1 (or passthrough prim) */
      }

      /* Disable the WGP mode on gfx10.3 because it can hang. (it
       * happened on VanGogh) Let's disable it on all chips that
       * disable exactly 1 CU per SA for GS.
       */
      config_out->rsrc1 |=
         S_00B228_GS_VGPR_COMP_CNT(gs_vgpr_comp_cnt) | S_00B228_WGP_MODE(wgp_mode);
      config_out->rsrc2 |= S_00B22C_ES_VGPR_COMP_CNT(es_vgpr_comp_cnt) |
                           S_00B22C_LDS_SIZE(config_in->lds_size) |
                           S_00B22C_OC_LDS_EN(es_stage == MESA_SHADER_TESS_EVAL);
   } else if (pdevice->rad_info.gfx_level >= GFX9 && stage == MESA_SHADER_GEOMETRY) {
      unsigned es_type = info->gs.es_type;
      unsigned gs_vgpr_comp_cnt, es_vgpr_comp_cnt;

      if (es_type == MESA_SHADER_VERTEX) {
         /* VGPR0-3: (VertexID, InstanceID / StepRate0, ...) */
         if (info->vs.needs_instance_id) {
            es_vgpr_comp_cnt = pdevice->rad_info.gfx_level >= GFX10 ? 3 : 1;
         } else {
            es_vgpr_comp_cnt = 0;
         }
      } else if (es_type == MESA_SHADER_TESS_EVAL) {
         es_vgpr_comp_cnt = info->uses_prim_id ? 3 : 2;
      } else {
         unreachable("invalid shader ES type");
      }

      /* If offsets 4, 5 are used, GS_VGPR_COMP_CNT is ignored and
       * VGPR[0:4] are always loaded.
       */
      if (info->uses_invocation_id) {
         gs_vgpr_comp_cnt = 3; /* VGPR3 contains InvocationID. */
      } else if (info->uses_prim_id) {
         gs_vgpr_comp_cnt = 2; /* VGPR2 contains PrimitiveID. */
      } else if (info->gs.vertices_in >= 3) {
         gs_vgpr_comp_cnt = 1; /* VGPR1 contains offsets 2, 3 */
      } else {
         gs_vgpr_comp_cnt = 0; /* VGPR0 contains offsets 0, 1 */
      }

      config_out->rsrc1 |=
         S_00B228_GS_VGPR_COMP_CNT(gs_vgpr_comp_cnt) | S_00B228_WGP_MODE(wgp_mode);
      config_out->rsrc2 |= S_00B22C_ES_VGPR_COMP_CNT(es_vgpr_comp_cnt) |
                           S_00B22C_OC_LDS_EN(es_type == MESA_SHADER_TESS_EVAL);
   } else if (pdevice->rad_info.gfx_level >= GFX9 && stage == MESA_SHADER_TESS_CTRL) {
      config_out->rsrc1 |= S_00B428_LS_VGPR_COMP_CNT(vgpr_comp_cnt);
   } else {
      config_out->rsrc1 |= S_00B128_VGPR_COMP_CNT(vgpr_comp_cnt);
   }
}

#if defined(USE_LIBELF)
static bool
radv_open_rtld_binary(struct radv_device *device, const struct radv_shader *shader,
                      const struct radv_shader_binary *binary, struct ac_rtld_binary *rtld_binary)
{
   const char *elf_data = (const char *)((struct radv_shader_binary_rtld *)binary)->data;
   size_t elf_size = ((struct radv_shader_binary_rtld *)binary)->elf_size;
   struct ac_rtld_symbol lds_symbols[3];
   unsigned num_lds_symbols = 0;

   if (device->physical_device->rad_info.gfx_level >= GFX9 &&
       (binary->stage == MESA_SHADER_GEOMETRY || binary->info.is_ngg)) {
      struct ac_rtld_symbol *sym = &lds_symbols[num_lds_symbols++];
      sym->name = "esgs_ring";
      sym->size = binary->info.ngg_info.esgs_ring_size;
      sym->align = 64 * 1024;
   }

   if (binary->info.is_ngg && binary->stage == MESA_SHADER_GEOMETRY) {
      struct ac_rtld_symbol *sym = &lds_symbols[num_lds_symbols++];
      sym->name = "ngg_emit";
      sym->size = binary->info.ngg_info.ngg_emit_size * 4;
      sym->align = 4;

      sym = &lds_symbols[num_lds_symbols++];
      sym->name = "ngg_scratch";
      sym->size = 8;
      sym->align = 4;
   }

   struct ac_rtld_open_info open_info = {
      .info = &device->physical_device->rad_info,
      .shader_type = binary->stage,
      .wave_size = binary->info.wave_size,
      .num_parts = 1,
      .elf_ptrs = &elf_data,
      .elf_sizes = &elf_size,
      .num_shared_lds_symbols = num_lds_symbols,
      .shared_lds_symbols = lds_symbols,
   };

   return ac_rtld_open(rtld_binary, open_info);
}
#endif

static bool
radv_shader_binary_upload(struct radv_device *device, const struct radv_shader_binary *binary,
                          struct radv_shader *shader)
{
   void *dest_ptr;

   shader->alloc = radv_alloc_shader_memory(device, shader->code_size, shader);
   if (!shader->alloc)
      return false;

   shader->bo = shader->alloc->arena->bo;
   shader->va = radv_buffer_get_va(shader->bo) + shader->alloc->offset;

   dest_ptr = shader->alloc->arena->ptr + shader->alloc->offset;

   if (binary->type == RADV_BINARY_TYPE_RTLD) {
#if !defined(USE_LIBELF)
      return false;
#else
      struct ac_rtld_binary rtld_binary = {0};

      if (!radv_open_rtld_binary(device, shader, binary, &rtld_binary)) {
         free(shader);
         return false;
      }

      struct ac_rtld_upload_info info = {
         .binary = &rtld_binary,
         .rx_va = radv_shader_get_va(shader),
         .rx_ptr = dest_ptr,
      };

      if (!ac_rtld_upload(&info)) {
         radv_shader_unref(device, shader);
         ac_rtld_close(&rtld_binary);
         return false;
      }

      shader->code_ptr = dest_ptr;
      ac_rtld_close(&rtld_binary);
#endif
   } else {
      struct radv_shader_binary_legacy *bin = (struct radv_shader_binary_legacy *)binary;
      memcpy(dest_ptr, bin->data + bin->stats_size, bin->code_size);

      /* Add end-of-code markers for the UMR disassembler. */
      uint32_t *ptr32 = (uint32_t *)dest_ptr + bin->code_size / 4;
      for (unsigned i = 0; i < DEBUGGER_NUM_MARKERS; i++)
         ptr32[i] = DEBUGGER_END_OF_CODE_MARKER;

      shader->code_ptr = dest_ptr;
   }

   return true;
}

struct radv_shader *
radv_shader_create(struct radv_device *device, const struct radv_shader_binary *binary,
                   bool keep_shader_info, bool from_cache, const struct radv_shader_args *args)
{
   struct ac_shader_config config = {0};
   struct radv_shader *shader = calloc(1, sizeof(struct radv_shader));
   if (!shader)
      return NULL;

   shader->ref_count = 1;

   if (binary->type == RADV_BINARY_TYPE_RTLD) {
#if !defined(USE_LIBELF)
      free(shader);
      return NULL;
#else
      struct ac_rtld_binary rtld_binary = {0};

      if (!radv_open_rtld_binary(device, shader, binary, &rtld_binary)) {
         free(shader);
         return NULL;
      }

      if (!ac_rtld_read_config(&device->physical_device->rad_info, &rtld_binary, &config)) {
         ac_rtld_close(&rtld_binary);
         free(shader);
         return NULL;
      }

      if (rtld_binary.lds_size > 0) {
         unsigned encode_granularity = device->physical_device->rad_info.lds_encode_granularity;
         config.lds_size = DIV_ROUND_UP(rtld_binary.lds_size, encode_granularity);
      }
      if (!config.lds_size && binary->stage == MESA_SHADER_TESS_CTRL) {
         /* This is used for reporting LDS statistics */
         config.lds_size = binary->info.tcs.num_lds_blocks;
      }

      assert(!binary->info.has_ngg_culling || config.lds_size);

      shader->code_size = rtld_binary.rx_size;
      shader->exec_size = rtld_binary.exec_size;
      ac_rtld_close(&rtld_binary);
#endif
   } else {
      assert(binary->type == RADV_BINARY_TYPE_LEGACY);
      config = ((struct radv_shader_binary_legacy *)binary)->base.config;
      shader->code_size =
         radv_get_shader_binary_size(((struct radv_shader_binary_legacy *)binary)->code_size);
      shader->exec_size = ((struct radv_shader_binary_legacy *)binary)->exec_size;
   }

   shader->info = binary->info;

   if (from_cache) {
      /* Copy the shader binary configuration from the cache. */
      memcpy(&shader->config, &binary->config, sizeof(shader->config));
   } else {
      assert(args);
      radv_postprocess_config(device, &config, &binary->info, binary->stage, args, &shader->config);
   }

   if (binary->type == RADV_BINARY_TYPE_RTLD) {
#if !defined(USE_LIBELF)
      free(shader);
      return NULL;
#else
      struct radv_shader_binary_rtld *bin = (struct radv_shader_binary_rtld *)binary;
      struct ac_rtld_binary rtld_binary = {0};

      if (!radv_open_rtld_binary(device, shader, binary, &rtld_binary)) {
         free(shader);
         return NULL;
      }

      if (keep_shader_info || (device->instance->debug_flags & RADV_DEBUG_DUMP_SHADERS)) {
         const char *disasm_data;
         size_t disasm_size;
         if (!ac_rtld_get_section_by_name(&rtld_binary, ".AMDGPU.disasm", &disasm_data,
                                          &disasm_size)) {
            radv_shader_unref(device, shader);
            ac_rtld_close(&rtld_binary);
            return NULL;
         }

         shader->ir_string =
            bin->llvm_ir_size ? strdup((const char *)(bin->data + bin->elf_size)) : NULL;
         shader->disasm_string = malloc(disasm_size + 1);
         memcpy(shader->disasm_string, disasm_data, disasm_size);
         shader->disasm_string[disasm_size] = 0;
      }
      ac_rtld_close(&rtld_binary);
#endif
   } else {
      struct radv_shader_binary_legacy *bin = (struct radv_shader_binary_legacy *)binary;

      shader->ir_string =
         bin->ir_size ? strdup((const char *)(bin->data + bin->stats_size + bin->code_size)) : NULL;
      shader->disasm_string =
         bin->disasm_size
            ? strdup((const char *)(bin->data + bin->stats_size + bin->code_size + bin->ir_size))
            : NULL;

      if (bin->stats_size) {
         shader->statistics = calloc(bin->stats_size, 1);
         memcpy(shader->statistics, bin->data, bin->stats_size);
      }
   }

   if (!radv_shader_binary_upload(device, binary, shader))
      return NULL;

   return shader;
}

static struct radv_shader_part *
radv_shader_part_create(struct radv_shader_part_binary *binary, unsigned wave_size)
{
   uint32_t code_size = radv_get_shader_binary_size(binary->code_size);
   struct radv_shader_part *shader_part;

   shader_part = calloc(1, sizeof(struct radv_shader_part));
   if (!shader_part)
      return NULL;

   shader_part->ref_count = 1;
   shader_part->binary = binary;
   shader_part->code_size = code_size;
   shader_part->rsrc1 = S_00B848_VGPRS((binary->num_vgprs - 1) / (wave_size == 32 ? 8 : 4)) |
                        S_00B228_SGPRS((binary->num_sgprs - 1) / 8);
   shader_part->num_preserved_sgprs = binary->num_preserved_sgprs;
   shader_part->disasm_string =
      binary->disasm_size ? strdup((const char *)(binary->data + binary->code_size)) : NULL;

   return shader_part;
}

void
radv_shader_part_binary_upload(const struct radv_shader_part_binary *binary, void *dest_ptr)
{
   memcpy(dest_ptr, binary->data, binary->code_size);

   /* Add end-of-code markers for the UMR disassembler. */
   uint32_t *ptr32 = (uint32_t *)dest_ptr + binary->code_size / 4;
   for (unsigned i = 0; i < DEBUGGER_NUM_MARKERS; i++)
      ptr32[i] = DEBUGGER_END_OF_CODE_MARKER;
}

static char *
radv_dump_nir_shaders(struct nir_shader *const *shaders, int shader_count)
{
   char *data = NULL;
   char *ret = NULL;
   size_t size = 0;
   struct u_memstream mem;
   if (u_memstream_open(&mem, &data, &size)) {
      FILE *const memf = u_memstream_get(&mem);
      for (int i = 0; i < shader_count; ++i)
         nir_print_shader(shaders[i], memf);
      u_memstream_close(&mem);
   }

   ret = malloc(size + 1);
   if (ret) {
      memcpy(ret, data, size);
      ret[size] = 0;
   }
   free(data);
   return ret;
}

static void
radv_aco_build_shader_binary(void **bin,
                             gl_shader_stage stage,
                             const struct ac_shader_config *config,
                             const char *llvm_ir_str,
                             unsigned llvm_ir_size,
                             const char *disasm_str,
                             unsigned disasm_size,
                             uint32_t *statistics,
                             uint32_t stats_size,
                             uint32_t exec_size,
                             const uint32_t *code,
                             uint32_t code_dw)
{
   struct radv_shader_binary **binary = (struct radv_shader_binary **)bin;
   size_t size = llvm_ir_size;

   size += disasm_size;
   size += stats_size;

   size += code_dw * sizeof(uint32_t) + sizeof(struct radv_shader_binary_legacy);

   /* We need to calloc to prevent unintialized data because this will be used
    * directly for the disk cache. Uninitialized data can appear because of
    * padding in the struct or because legacy_binary->data can be at an offset
    * from the start less than sizeof(radv_shader_binary_legacy). */
   struct radv_shader_binary_legacy *legacy_binary = (struct radv_shader_binary_legacy *)calloc(size, 1);
   legacy_binary->base.type = RADV_BINARY_TYPE_LEGACY;
   legacy_binary->base.stage = stage;
   legacy_binary->base.total_size = size;
   legacy_binary->base.config = *config;

   if (stats_size)
      memcpy(legacy_binary->data, statistics, stats_size);
   legacy_binary->stats_size = stats_size;

   memcpy(legacy_binary->data + legacy_binary->stats_size, code,
          code_dw * sizeof(uint32_t));
   legacy_binary->exec_size = exec_size;
   legacy_binary->code_size = code_dw * sizeof(uint32_t);

   legacy_binary->disasm_size = 0;
   legacy_binary->ir_size = llvm_ir_size;

   memcpy((char*)legacy_binary->data + legacy_binary->stats_size + legacy_binary->code_size,
          llvm_ir_str, llvm_ir_size);

   legacy_binary->disasm_size = disasm_size;
   if (disasm_size) {
      memcpy((char*)legacy_binary->data + legacy_binary->stats_size +
             legacy_binary->code_size + llvm_ir_size, disasm_str,
             disasm_size);
   }
   *binary = (struct radv_shader_binary*)legacy_binary;
}

static void
radv_fill_nir_compiler_options(struct radv_nir_compiler_options *options,
                               struct radv_device *device, const struct radv_pipeline_key *key,
                               bool should_use_wgp, bool can_dump_shader, bool is_meta_shader,
                               bool keep_shader_info, bool keep_statistic_info)
{
   if (key)
      options->key = *key;

   options->robust_buffer_access = device->robust_buffer_access;
   options->wgp_mode = should_use_wgp;
   options->family = device->physical_device->rad_info.family;
   options->gfx_level = device->physical_device->rad_info.gfx_level;
   options->has_3d_cube_border_color_mipmap = device->physical_device->rad_info.has_3d_cube_border_color_mipmap;
   options->dump_shader = can_dump_shader;
   options->dump_preoptir =
      options->dump_shader && device->instance->debug_flags & RADV_DEBUG_PREOPTIR;
   options->record_ir = keep_shader_info;
   options->record_stats = keep_statistic_info;
   options->check_ir = device->instance->debug_flags & RADV_DEBUG_CHECKIR;
   options->address32_hi = device->physical_device->rad_info.address32_hi;
   options->has_ls_vgpr_init_bug = device->physical_device->rad_info.has_ls_vgpr_init_bug;

   if (!is_meta_shader)
      options->enable_mrt_output_nan_fixup = options->key.ps.epilog.enable_mrt_output_nan_fixup;
}

static struct radv_shader *
shader_compile(struct radv_device *device, struct nir_shader *const *shaders, int shader_count,
               gl_shader_stage stage, const struct radv_shader_info *info,
               const struct radv_shader_args *args, const struct radv_pipeline_key *key,
               bool trap_handler_shader, bool keep_shader_info, bool keep_statistic_info,
               struct radv_shader_binary **binary_out)
{
   struct radv_nir_compiler_options options = {0};
   radv_fill_nir_compiler_options(
      &options, device, key, radv_should_use_wgp_mode(device, stage, info),
      radv_can_dump_shader(device, shaders[0], trap_handler_shader), is_meta_shader(shaders[0]),
      keep_shader_info, keep_statistic_info);

   struct radv_shader_debug_data debug_data = {
      .device = device,
      .object = NULL,
   };
   options.debug.func = radv_compiler_debug;
   options.debug.private_data = &debug_data;

   struct radv_shader_binary *binary = NULL;

#ifdef LLVM_AVAILABLE
   if (radv_use_llvm_for_stage(device, stage) || options.dump_shader || options.record_ir)
      ac_init_llvm_once();

   if (radv_use_llvm_for_stage(device, stage)) {
      llvm_compile_shader(&options, info, shader_count, shaders, &binary, args);
#else
   if (false) {
#endif
   } else {
      struct aco_shader_info ac_info;
      struct aco_compiler_options ac_opts;
      radv_aco_convert_opts(&ac_opts, &options);
      radv_aco_convert_shader_info(&ac_info, info);
      aco_compile_shader(&ac_opts, &ac_info, shader_count, shaders, args, &radv_aco_build_shader_binary, (void **)&binary);
   }

   binary->info = *info;

   struct radv_shader *shader = radv_shader_create(device, binary, keep_shader_info, false, args);
   if (!shader) {
      free(binary);
      return NULL;
   }

   if (options.dump_shader) {
      fprintf(stderr, "%s", radv_get_shader_name(info, shaders[0]->info.stage));
      for (int i = 1; i < shader_count; ++i)
         fprintf(stderr, " + %s", radv_get_shader_name(info, shaders[i]->info.stage));

      fprintf(stderr, "\ndisasm:\n%s\n", shader->disasm_string);
   }

   if (keep_shader_info) {
      shader->nir_string = radv_dump_nir_shaders(shaders, shader_count);
   }

   /* Copy the shader binary configuration to store it in the cache. */
   memcpy(&binary->config, &shader->config, sizeof(binary->config));

   *binary_out = binary;
   return shader;
}

struct radv_shader *
radv_shader_nir_to_asm(struct radv_device *device, struct radv_pipeline_stage *pl_stage,
                       struct nir_shader *const *shaders, int shader_count,
                       const struct radv_pipeline_key *key, bool keep_shader_info,
                       bool keep_statistic_info, struct radv_shader_binary **binary_out)
{
   gl_shader_stage stage = shaders[shader_count - 1]->info.stage;

   return shader_compile(device, shaders, shader_count, stage, &pl_stage->info, &pl_stage->args,
                         key, false, keep_shader_info, keep_statistic_info, binary_out);
}

struct radv_shader *
radv_create_gs_copy_shader(struct radv_device *device, struct nir_shader *shader,
                           const struct radv_shader_info *info, const struct radv_shader_args *args,
                           struct radv_shader_binary **binary_out, bool keep_shader_info,
                           bool keep_statistic_info, bool disable_optimizations)
{
   gl_shader_stage stage = MESA_SHADER_VERTEX;

   struct radv_pipeline_key key = {
      .optimisations_disabled = disable_optimizations,
   };

   return shader_compile(device, &shader, 1, stage, info, args, &key, false, keep_shader_info,
                         keep_statistic_info, binary_out);
}

struct radv_trap_handler_shader *
radv_create_trap_handler_shader(struct radv_device *device)
{
   gl_shader_stage stage = MESA_SHADER_COMPUTE;
   struct radv_shader *shader = NULL;
   struct radv_shader_binary *binary = NULL;
   struct radv_shader_info info = {0};
   struct radv_pipeline_key key = {0};
   struct radv_trap_handler_shader *trap;

   trap = malloc(sizeof(struct radv_trap_handler_shader));
   if (!trap)
      return NULL;

   nir_builder b = radv_meta_init_shader(device, stage, "meta_trap_handler");

   info.wave_size = 64;

   struct radv_shader_args args = {0};
   args.explicit_scratch_args = true;
   args.is_trap_handler_shader = true;
   radv_declare_shader_args(device->physical_device->rad_info.gfx_level, &key, &info, stage, false,
                            MESA_SHADER_VERTEX, &args);

   shader =
      shader_compile(device, &b.shader, 1, stage, &info, &args, &key, true, false, false, &binary);

   trap->alloc = radv_alloc_shader_memory(device, shader->code_size, NULL);

   trap->bo = trap->alloc->arena->bo;
   char *dest_ptr = trap->alloc->arena->ptr + trap->alloc->offset;

   struct radv_shader_binary_legacy *bin = (struct radv_shader_binary_legacy *)binary;
   memcpy(dest_ptr, bin->data, bin->code_size);

   ralloc_free(b.shader);
   free(shader);
   free(binary);

   return trap;
}

uint64_t radv_trap_handler_shader_get_va(const struct radv_trap_handler_shader *trap)
{
   return radv_buffer_get_va(trap->alloc->arena->bo) + trap->alloc->offset;
}

void
radv_trap_handler_shader_destroy(struct radv_device *device, struct radv_trap_handler_shader *trap)
{
   if (!trap)
      return;

   radv_free_shader_memory(device, trap->alloc);
   free(trap);
}

static void radv_aco_build_shader_part(void **bin,
                                       uint32_t num_sgprs,
                                       uint32_t num_vgprs,
                                       uint32_t num_preserved_sgprs,
                                       const uint32_t *code,
                                       uint32_t code_size,
                                       const char *disasm_str,
                                       uint32_t disasm_size)
{
   struct radv_shader_part_binary **binary = (struct radv_shader_part_binary **)bin;
   size_t size = code_size * sizeof(uint32_t) + sizeof(struct radv_shader_part_binary);

   size += disasm_size;
   struct radv_shader_part_binary *part_binary = (struct radv_shader_part_binary *)calloc(size, 1);

   part_binary->num_sgprs = num_sgprs;
   part_binary->num_vgprs = num_vgprs;
   part_binary->num_preserved_sgprs = num_preserved_sgprs;
   part_binary->code_size = code_size * sizeof(uint32_t);
   memcpy(part_binary->data, code, part_binary->code_size);
   if (disasm_size) {
      memcpy((char*)part_binary->data + part_binary->code_size,
             disasm_str, disasm_size);
      part_binary->disasm_size = disasm_size;
   }

   *binary = part_binary;
}

struct radv_shader_part *
radv_create_vs_prolog(struct radv_device *device, const struct radv_vs_prolog_key *key)
{
   struct radv_shader_part *prolog;
   struct radv_shader_args args = {0};
   struct radv_nir_compiler_options options = {0};
   radv_fill_nir_compiler_options(&options, device, NULL, false,
                                  device->instance->debug_flags & RADV_DEBUG_DUMP_PROLOGS, false,
                                  device->instance->debug_flags & RADV_DEBUG_HANG, false);

   struct radv_shader_info info = {0};
   info.wave_size = key->wave32 ? 32 : 64;
   info.vs.needs_instance_id = true;
   info.vs.needs_base_instance = true;
   info.vs.needs_draw_id = true;
   info.vs.use_per_attribute_vb_descs = true;
   info.vs.vb_desc_usage_mask = BITFIELD_MASK(key->num_attributes);
   info.vs.has_prolog = true;
   info.vs.as_ls = key->as_ls;
   info.is_ngg = key->is_ngg;

   struct radv_pipeline_key pipeline_key = {0};

   args.explicit_scratch_args = true;
   radv_declare_shader_args(options.gfx_level, &pipeline_key, &info, key->next_stage,
                            key->next_stage != MESA_SHADER_VERTEX, MESA_SHADER_VERTEX, &args);

   info.user_sgprs_locs = args.user_sgprs_locs;
   info.inline_push_constant_mask = args.ac.inline_push_const_mask;

#ifdef LLVM_AVAILABLE
   if (options.dump_shader || options.record_ir)
      ac_init_llvm_once();
#endif

   struct radv_shader_part_binary *binary = NULL;
   struct aco_shader_info ac_info;
   struct aco_vs_prolog_key ac_key;
   struct aco_compiler_options ac_opts;
   radv_aco_convert_shader_info(&ac_info, &info);
   radv_aco_convert_opts(&ac_opts, &options);
   radv_aco_convert_vs_prolog_key(&ac_key, key);
   aco_compile_vs_prolog(&ac_opts, &ac_info, &ac_key, &args, &radv_aco_build_shader_part,
                         (void **)&binary);

   prolog = radv_shader_part_create(binary, info.wave_size);
   if (!prolog)
      goto fail_create;

   prolog->nontrivial_divisors = key->state->nontrivial_divisors;

   /* Allocate memory and upload the prolog. */
   prolog->alloc = radv_alloc_shader_memory(device, prolog->code_size, NULL);
   if (!prolog->alloc)
      goto fail_alloc;

   prolog->bo = prolog->alloc->arena->bo;
   prolog->va = radv_buffer_get_va(prolog->bo) + prolog->alloc->offset;

   void *dest_ptr = prolog->alloc->arena->ptr + prolog->alloc->offset;
   radv_shader_part_binary_upload(binary, dest_ptr);

   if (options.dump_shader) {
      fprintf(stderr, "Vertex prolog");
      fprintf(stderr, "\ndisasm:\n%s\n", prolog->disasm_string);
   }

   free(prolog->binary);
   prolog->binary = NULL;

   return prolog;

fail_alloc:
   radv_shader_part_destroy(device, prolog);
fail_create:
   free(binary);
   return NULL;
}

struct radv_shader_part *
radv_create_ps_epilog(struct radv_device *device, const struct radv_ps_epilog_key *key)
{
   struct radv_shader_part *epilog;
   struct radv_shader_args args = {0};
   struct radv_nir_compiler_options options = {0};
   radv_fill_nir_compiler_options(&options, device, NULL, false,
                                  device->instance->debug_flags & RADV_DEBUG_DUMP_EPILOGS, false,
                                  device->instance->debug_flags & RADV_DEBUG_HANG, false);

   struct radv_shader_info info = {0};
   info.wave_size = device->physical_device->ps_wave_size;
   info.workgroup_size = 64;

   radv_declare_ps_epilog_args(device->physical_device->rad_info.gfx_level, key, &args);

#ifdef LLVM_AVAILABLE
   if (options.dump_shader || options.record_ir)
      ac_init_llvm_once();
#endif

   struct radv_shader_part_binary *binary = NULL;
   struct aco_shader_info ac_info;
   struct aco_ps_epilog_key ac_key;
   struct aco_compiler_options ac_opts;
   radv_aco_convert_shader_info(&ac_info, &info);
   radv_aco_convert_opts(&ac_opts, &options);
   radv_aco_convert_ps_epilog_key(&ac_key, key);
   aco_compile_ps_epilog(&ac_opts, &ac_info, &ac_key, &args, &radv_aco_build_shader_part,
                         (void **)&binary);

   epilog = radv_shader_part_create(binary, info.wave_size);
   if (!epilog)
      goto fail_create;

   epilog->spi_shader_col_format = key->spi_shader_col_format;

   /* Allocate memory and upload the epilog. */
   epilog->alloc = radv_alloc_shader_memory(device, epilog->code_size, NULL);
   if (!epilog->alloc)
      goto fail_alloc;

   epilog->bo = epilog->alloc->arena->bo;
   epilog->va = radv_buffer_get_va(epilog->bo) + epilog->alloc->offset;

   void *dest_ptr = epilog->alloc->arena->ptr + epilog->alloc->offset;
   radv_shader_part_binary_upload(binary, dest_ptr);

   if (options.dump_shader) {
      fprintf(stderr, "Fragment epilog");
      fprintf(stderr, "\ndisasm:\n%s\n", epilog->disasm_string);
   }

   free(epilog->binary);
   epilog->binary = NULL;

   return epilog;

fail_alloc:
   radv_shader_part_destroy(device, epilog);
fail_create:
   free(binary);
   return NULL;
}

void
radv_shader_destroy(struct radv_device *device, struct radv_shader *shader)
{
   assert(shader->ref_count == 0);

   radv_free_shader_memory(device, shader->alloc);

   free(shader->spirv);
   free(shader->nir_string);
   free(shader->disasm_string);
   free(shader->ir_string);
   free(shader->statistics);
   free(shader);
}

void
radv_shader_part_destroy(struct radv_device *device, struct radv_shader_part *shader_part)
{
   assert(shader_part->ref_count == 0);

   if (shader_part->alloc)
      radv_free_shader_memory(device, shader_part->alloc);
   free(shader_part->binary);
   free(shader_part->disasm_string);
   free(shader_part);
}

uint64_t
radv_shader_get_va(const struct radv_shader *shader)
{
   return shader->va;
}

struct radv_shader *
radv_find_shader(struct radv_device *device, uint64_t pc)
{
   mtx_lock(&device->shader_arena_mutex);
   list_for_each_entry(struct radv_shader_arena, arena, &device->shader_arenas, list)
   {
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
      list_for_each_entry(union radv_shader_arena_block, block, &arena->entries, list)
      {
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
         uint64_t start = radv_buffer_get_va(block->arena->bo) + block->offset;
         if (!block->freelist.prev && pc >= start && pc < start + block->size) {
            mtx_unlock(&device->shader_arena_mutex);

            struct radv_pipeline *pipeline = (struct radv_pipeline *)block->freelist.next;
            for (uint32_t i = 0; i < MESA_VULKAN_SHADER_STAGES; i++) {
               struct radv_shader *shader = pipeline->shaders[i];
               if (!shader)
                  continue;

               if (pc >= shader->va &&
                   pc < shader->va + align(shader->code_size, RADV_SHADER_ALLOC_ALIGNMENT))
                  return shader;
            }
         }
      }
   }

   mtx_unlock(&device->shader_arena_mutex);
   return NULL;
}

const char *
radv_get_shader_name(const struct radv_shader_info *info, gl_shader_stage stage)
{
   switch (stage) {
   case MESA_SHADER_VERTEX:
      if (info->vs.as_ls)
         return "Vertex Shader as LS";
      else if (info->vs.as_es)
         return "Vertex Shader as ES";
      else if (info->is_ngg)
         return "Vertex Shader as ESGS";
      else
         return "Vertex Shader as VS";
   case MESA_SHADER_TESS_CTRL:
      return "Tessellation Control Shader";
   case MESA_SHADER_TESS_EVAL:
      if (info->tes.as_es)
         return "Tessellation Evaluation Shader as ES";
      else if (info->is_ngg)
         return "Tessellation Evaluation Shader as ESGS";
      else
         return "Tessellation Evaluation Shader as VS";
   case MESA_SHADER_GEOMETRY:
      return "Geometry Shader";
   case MESA_SHADER_FRAGMENT:
      return "Pixel Shader";
   case MESA_SHADER_COMPUTE:
      return "Compute Shader";
   case MESA_SHADER_MESH:
      return "Mesh Shader as NGG";
   case MESA_SHADER_TASK:
      return "Task Shader as CS";
   default:
      return "Unknown shader";
   };
}

unsigned
radv_get_max_waves(const struct radv_device *device, struct radv_shader *shader,
                   gl_shader_stage stage)
{
   struct radeon_info *info = &device->physical_device->rad_info;
   enum amd_gfx_level gfx_level = info->gfx_level;
   uint8_t wave_size = shader->info.wave_size;
   struct ac_shader_config *conf = &shader->config;
   unsigned max_simd_waves;
   unsigned lds_per_wave = 0;

   max_simd_waves = info->max_wave64_per_simd * (64 / wave_size);

   if (stage == MESA_SHADER_FRAGMENT) {
      lds_per_wave =
         conf->lds_size * info->lds_encode_granularity + shader->info.ps.num_interp * 48;
      lds_per_wave = align(lds_per_wave, info->lds_alloc_granularity);
   } else if (stage == MESA_SHADER_COMPUTE || stage == MESA_SHADER_TASK) {
      unsigned max_workgroup_size = shader->info.workgroup_size;
      lds_per_wave =
         align(conf->lds_size * info->lds_encode_granularity, info->lds_alloc_granularity);
      lds_per_wave /= DIV_ROUND_UP(max_workgroup_size, wave_size);
   }

   if (conf->num_sgprs && gfx_level < GFX10) {
      unsigned sgprs = align(conf->num_sgprs, gfx_level >= GFX8 ? 16 : 8);
      max_simd_waves = MIN2(max_simd_waves, info->num_physical_sgprs_per_simd / sgprs);
   }

   if (conf->num_vgprs) {
      unsigned physical_vgprs = info->num_physical_wave64_vgprs_per_simd * (64 / wave_size);
      unsigned vgprs = align(conf->num_vgprs, wave_size == 32 ? 8 : 4);
      if (gfx_level >= GFX10_3) {
         unsigned real_vgpr_gran = info->num_physical_wave64_vgprs_per_simd / 64;
         vgprs = util_align_npot(vgprs, real_vgpr_gran * (wave_size == 32 ? 2 : 1));
      }
      max_simd_waves = MIN2(max_simd_waves, physical_vgprs / vgprs);
   }

   unsigned simd_per_workgroup = info->num_simd_per_compute_unit;
   if (gfx_level >= GFX10)
      simd_per_workgroup *= 2; /* like lds_size_per_workgroup, assume WGP on GFX10+ */

   unsigned max_lds_per_simd = info->lds_size_per_workgroup / simd_per_workgroup;
   if (lds_per_wave)
      max_simd_waves = MIN2(max_simd_waves, DIV_ROUND_UP(max_lds_per_simd, lds_per_wave));

   return gfx_level >= GFX10 ? max_simd_waves * (wave_size / 32) : max_simd_waves;
}

unsigned
radv_compute_spi_ps_input(const struct radv_pipeline_key *pipeline_key,
                          const struct radv_shader_info *info)
{
   unsigned spi_ps_input;

   spi_ps_input = S_0286CC_PERSP_CENTER_ENA(info->ps.reads_persp_center) |
                  S_0286CC_PERSP_CENTROID_ENA(info->ps.reads_persp_centroid) |
                  S_0286CC_PERSP_SAMPLE_ENA(info->ps.reads_persp_sample) |
                  S_0286CC_LINEAR_CENTER_ENA(info->ps.reads_linear_center) |
                  S_0286CC_LINEAR_CENTROID_ENA(info->ps.reads_linear_centroid) |
                  S_0286CC_LINEAR_SAMPLE_ENA(info->ps.reads_linear_sample)|
                  S_0286CC_PERSP_PULL_MODEL_ENA(info->ps.reads_barycentric_model) |
                  S_0286CC_FRONT_FACE_ENA(info->ps.reads_front_face);

   if (info->ps.reads_frag_coord_mask ||
       info->ps.reads_sample_pos_mask) {
      uint8_t mask = info->ps.reads_frag_coord_mask | info->ps.reads_sample_pos_mask;

      for (unsigned i = 0; i < 4; i++) {
         if (mask & (1 << i))
            spi_ps_input |= S_0286CC_POS_X_FLOAT_ENA(1) << i;
      }

      if (pipeline_key->adjust_frag_coord_z && info->ps.reads_frag_coord_mask & (1 << 2)) {
         spi_ps_input |= S_0286CC_ANCILLARY_ENA(1);
      }
   }

   if (info->ps.reads_sample_id || info->ps.reads_frag_shading_rate || info->ps.reads_sample_mask_in) {
      spi_ps_input |= S_0286CC_ANCILLARY_ENA(1);
   }

   if (info->ps.reads_sample_mask_in) {
      spi_ps_input |= S_0286CC_SAMPLE_COVERAGE_ENA(1);
   }

   if (G_0286CC_POS_W_FLOAT_ENA(spi_ps_input)) {
      /* If POS_W_FLOAT (11) is enabled, at least one of PERSP_* must be enabled too */
      spi_ps_input |= S_0286CC_PERSP_CENTER_ENA(1);
   }

   if (!(spi_ps_input & 0x7F)) {
      /* At least one of PERSP_* (0xF) or LINEAR_* (0x70) must be enabled */
      spi_ps_input |= S_0286CC_PERSP_CENTER_ENA(1);
   }

   return spi_ps_input;
}

VkResult
radv_dump_shader_stats(struct radv_device *device, struct radv_pipeline *pipeline,
                       gl_shader_stage stage, FILE *output)
{
   struct radv_shader *shader = pipeline->shaders[stage];
   VkPipelineExecutablePropertiesKHR *props = NULL;
   uint32_t prop_count = 0;
   VkResult result;

   VkPipelineInfoKHR pipeline_info = {0};
   pipeline_info.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR;
   pipeline_info.pipeline = radv_pipeline_to_handle(pipeline);

   result = radv_GetPipelineExecutablePropertiesKHR(radv_device_to_handle(device), &pipeline_info,
                                                    &prop_count, NULL);
   if (result != VK_SUCCESS)
      return result;

   props = calloc(prop_count, sizeof(*props));
   if (!props)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   result = radv_GetPipelineExecutablePropertiesKHR(radv_device_to_handle(device), &pipeline_info,
                                                    &prop_count, props);
   if (result != VK_SUCCESS)
      goto fail;

   for (unsigned exec_idx = 0; exec_idx < prop_count; exec_idx++) {
      if (!(props[exec_idx].stages & mesa_to_vk_shader_stage(stage)))
         continue;

      VkPipelineExecutableStatisticKHR *stats = NULL;
      uint32_t stat_count = 0;

      VkPipelineExecutableInfoKHR exec_info = {0};
      exec_info.pipeline = radv_pipeline_to_handle(pipeline);
      exec_info.executableIndex = exec_idx;

      result = radv_GetPipelineExecutableStatisticsKHR(radv_device_to_handle(device), &exec_info,
                                                       &stat_count, NULL);
      if (result != VK_SUCCESS)
         goto fail;

      stats = calloc(stat_count, sizeof(*stats));
      if (!stats) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail;
      }

      result = radv_GetPipelineExecutableStatisticsKHR(radv_device_to_handle(device), &exec_info,
                                                       &stat_count, stats);
      if (result != VK_SUCCESS) {
         free(stats);
         goto fail;
      }

      fprintf(output, "\n%s:\n", radv_get_shader_name(&shader->info, stage));
      fprintf(output, "*** SHADER STATS ***\n");

      for (unsigned i = 0; i < stat_count; i++) {
         fprintf(output, "%s: ", stats[i].name);
         switch (stats[i].format) {
         case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
            fprintf(output, "%s", stats[i].value.b32 == VK_TRUE ? "true" : "false");
            break;
         case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
            fprintf(output, "%" PRIi64, stats[i].value.i64);
            break;
         case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
            fprintf(output, "%" PRIu64, stats[i].value.u64);
            break;
         case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
            fprintf(output, "%f", stats[i].value.f64);
            break;
         default:
            unreachable("Invalid pipeline statistic format");
         }
         fprintf(output, "\n");
      }

      fprintf(output, "********************\n\n\n");

      free(stats);
   }

fail:
   free(props);
   return result;
}
