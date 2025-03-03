/*
 * Copyright © 2022 Valve Corporation
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

#include "nir.h"
#include "nir_builder.h"
#include "ac_nir.h"
#include "radv_constants.h"
#include "radv_private.h"
#include "radv_shader.h"
#include "radv_shader_args.h"

typedef struct {
   enum amd_gfx_level gfx_level;
   const struct radv_shader_args *args;
   const struct radv_shader_info *info;
   const struct radv_pipeline_key *pl_key;
   uint32_t address32_hi;
   nir_ssa_def *gsvs_ring[4];
} lower_abi_state;

static nir_ssa_def *
load_ring(nir_builder *b, unsigned ring, lower_abi_state *s)
{
   struct ac_arg arg =
      b->shader->info.stage == MESA_SHADER_TASK ?
      s->args->task_ring_offsets :
      s->args->ac.ring_offsets;

   nir_ssa_def *ring_offsets = ac_nir_load_arg(b, &s->args->ac, arg);
   ring_offsets = nir_pack_64_2x32_split(b, nir_channel(b, ring_offsets, 0), nir_channel(b, ring_offsets, 1));
   return nir_load_smem_amd(b, 4, ring_offsets, nir_imm_int(b, ring * 16u), .align_mul = 4u);
}

static nir_ssa_def *
nggc_bool_setting(nir_builder *b, unsigned mask, lower_abi_state *s)
{
   nir_ssa_def *settings = ac_nir_load_arg(b, &s->args->ac, s->args->ngg_culling_settings);
   return nir_test_mask(b, settings, mask);
}

static nir_ssa_def *
ngg_query_bool_setting(nir_builder *b, unsigned mask, lower_abi_state *s)
{
   nir_ssa_def *settings = ac_nir_load_arg(b, &s->args->ac, s->args->ngg_query_state);
   return nir_test_mask(b, settings, mask);
}

static nir_ssa_def *
lower_load_vs_input_from_prolog(nir_builder *b,
                                nir_intrinsic_instr *intrin,
                                lower_abi_state *s)
{
   nir_src *offset_src = nir_get_io_offset_src(intrin);
   assert(nir_src_is_const(*offset_src));

   const unsigned base = nir_intrinsic_base(intrin);
   const unsigned base_offset = nir_src_as_uint(*offset_src);
   const unsigned driver_location = base + base_offset - VERT_ATTRIB_GENERIC0;
   const unsigned component = nir_intrinsic_component(intrin);
   const unsigned bit_size = intrin->dest.ssa.bit_size;
   const unsigned num_components = intrin->dest.ssa.num_components;

   /* 64-bit inputs: they occupy twice as many 32-bit components.
    * 16-bit inputs: they occupy a 32-bit component (not packed).
    */
   const unsigned arg_bit_size = MAX2(bit_size, 32);

   unsigned num_input_args = 1;
   nir_ssa_def *input_args[2] = {ac_nir_load_arg(b, &s->args->ac, s->args->vs_inputs[driver_location]), NULL};
   if (component * 32 + arg_bit_size * num_components > 128) {
      assert(bit_size == 64);

      num_input_args++;
      input_args[1] = ac_nir_load_arg(b, &s->args->ac, s->args->vs_inputs[driver_location + 1]);
   }

   nir_ssa_def *extracted = nir_extract_bits(b, input_args, num_input_args, component * 32,
                                             num_components, arg_bit_size);

   if (bit_size < arg_bit_size) {
      assert(bit_size == 16);

      if (nir_alu_type_get_base_type(nir_intrinsic_dest_type(intrin)) == nir_type_float)
         return nir_f2f16(b, extracted);
      else
         return nir_u2u16(b, extracted);
   }

   return extracted;
}

static bool
lower_abi_instr(nir_builder *b, nir_instr *instr, void *state)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   lower_abi_state *s = (lower_abi_state *)state;
   gl_shader_stage stage = b->shader->info.stage;

   b->cursor = nir_before_instr(instr);

   nir_ssa_def *replacement = NULL;
   bool progress = true;

   switch (intrin->intrinsic) {
   case nir_intrinsic_load_ring_tess_factors_amd:
      replacement = load_ring(b, RING_HS_TESS_FACTOR, s);
      break;
   case nir_intrinsic_load_ring_tess_factors_offset_amd:
      replacement = ac_nir_load_arg(b, &s->args->ac, s->args->ac.tcs_factor_offset);
      break;
   case nir_intrinsic_load_ring_tess_offchip_amd:
      replacement = load_ring(b, RING_HS_TESS_OFFCHIP, s);
      break;
   case nir_intrinsic_load_ring_tess_offchip_offset_amd:
      replacement = ac_nir_load_arg(b, &s->args->ac, s->args->ac.tess_offchip_offset);
      break;
   case nir_intrinsic_load_tcs_num_patches_amd:
      if (s->pl_key->dynamic_patch_control_points) {
         if (stage == MESA_SHADER_TESS_CTRL) {
            nir_ssa_def *arg = ac_nir_load_arg(b, &s->args->ac, s->args->tcs_offchip_layout);
            replacement = nir_ubfe_imm(b, arg, 6, 8);
         } else {
            replacement = ac_nir_load_arg(b, &s->args->ac, s->args->tes_num_patches);
         }
      } else {
         replacement = nir_imm_int(b, s->info->num_tess_patches);
      }
      break;
   case nir_intrinsic_load_ring_esgs_amd:
      replacement = load_ring(b, stage == MESA_SHADER_GEOMETRY ? RING_ESGS_GS : RING_ESGS_VS, s);
      break;
   case nir_intrinsic_load_ring_gsvs_amd:
      if (stage == MESA_SHADER_VERTEX)
         replacement = load_ring(b, RING_GSVS_VS, s);
      else
         replacement = s->gsvs_ring[nir_intrinsic_stream_id(intrin)];
      break;
   case nir_intrinsic_load_ring_gs2vs_offset_amd:
      replacement = ac_nir_load_arg(b, &s->args->ac, s->args->ac.gs2vs_offset);
      break;
   case nir_intrinsic_load_ring_es2gs_offset_amd:
      replacement = ac_nir_load_arg(b, &s->args->ac, s->args->ac.es2gs_offset);
      break;

   case nir_intrinsic_load_ring_attr_amd:
      replacement = load_ring(b, RING_PS_ATTR, s);

      nir_ssa_def *dword1 = nir_channel(b, replacement, 1);
      dword1 = nir_ior_imm(b, dword1, S_008F04_STRIDE(16 * s->info->outinfo.param_exports));
      replacement = nir_vector_insert_imm(b, replacement, dword1, 1);
      break;

   case nir_intrinsic_load_ring_attr_offset_amd: {
      nir_ssa_def *ring_attr_offset = ac_nir_load_arg(b, &s->args->ac, s->args->ac.gs_attr_offset);
      replacement = nir_ishl(b, nir_ubfe(b, ring_attr_offset, nir_imm_int(b, 0), nir_imm_int(b, 15)),
                             nir_imm_int(b, 9)); /* 512b increments. */
      break;
   }

   case nir_intrinsic_load_tess_rel_patch_id_amd:
      if (stage == MESA_SHADER_TESS_CTRL) {
         replacement = nir_extract_u8(b, ac_nir_load_arg(b, &s->args->ac, s->args->ac.tcs_rel_ids),
                                      nir_imm_int(b, 0));
      } else if (stage == MESA_SHADER_TESS_EVAL) {
         /* Setting an upper bound like this will actually make it possible
          * to optimize some multiplications (in address calculations) so that
          * constant additions can be added to the const offset in memory load instructions.
          */
         nir_ssa_def *arg = ac_nir_load_arg(b, &s->args->ac, s->args->ac.tes_rel_patch_id);
         nir_intrinsic_instr *load_arg = nir_instr_as_intrinsic(arg->parent_instr);
         nir_intrinsic_set_arg_upper_bound_u32_amd(load_arg, 2048 / MAX2(b->shader->info.tess.tcs_vertices_out, 1));
         replacement = arg;
      } else {
         unreachable("invalid tessellation shader stage");
      }
      break;
   case nir_intrinsic_load_patch_vertices_in:
      if (stage == MESA_SHADER_TESS_CTRL) {
         if (s->pl_key->dynamic_patch_control_points) {
            nir_ssa_def *arg = ac_nir_load_arg(b, &s->args->ac, s->args->tcs_offchip_layout);
            replacement = nir_ubfe_imm(b, arg, 0, 6);
         } else {
            replacement = nir_imm_int(b, s->pl_key->tcs.tess_input_vertices);
         }
      } else if (stage == MESA_SHADER_TESS_EVAL) {
         replacement = nir_imm_int(b, b->shader->info.tess.tcs_vertices_out);
      }
      else
         unreachable("invalid tessellation shader stage");
      break;
   case nir_intrinsic_load_gs_vertex_offset_amd:
      replacement =
         ac_nir_load_arg(b, &s->args->ac, s->args->ac.gs_vtx_offset[nir_intrinsic_base(intrin)]);
      break;
   case nir_intrinsic_load_workgroup_num_input_vertices_amd:
      replacement = nir_ubfe(b, ac_nir_load_arg(b, &s->args->ac, s->args->ac.gs_tg_info),
                             nir_imm_int(b, 12), nir_imm_int(b, 9));
      break;
   case nir_intrinsic_load_workgroup_num_input_primitives_amd:
      replacement = nir_ubfe(b, ac_nir_load_arg(b, &s->args->ac, s->args->ac.gs_tg_info),
                             nir_imm_int(b, 22), nir_imm_int(b, 9));
      break;
   case nir_intrinsic_load_packed_passthrough_primitive_amd:
      /* NGG passthrough mode: the HW already packs the primitive export value to a single register. */
      replacement = ac_nir_load_arg(b, &s->args->ac, s->args->ac.gs_vtx_offset[0]);
      break;
   case nir_intrinsic_load_pipeline_stat_query_enabled_amd:
      replacement = ngg_query_bool_setting(b, radv_ngg_query_pipeline_stat, s);
      break;
   case nir_intrinsic_load_prim_gen_query_enabled_amd:
      replacement = ngg_query_bool_setting(b, radv_ngg_query_prim_gen, s);
      break;
   case nir_intrinsic_load_prim_xfb_query_enabled_amd:
      replacement = ngg_query_bool_setting(b, radv_ngg_query_prim_xfb, s);
      break;
   case nir_intrinsic_load_merged_wave_info_amd:
      replacement = ac_nir_load_arg(b, &s->args->ac, s->args->ac.merged_wave_info);
      break;
   case nir_intrinsic_load_cull_any_enabled_amd: {
      nir_ssa_def *gs_tg_info = ac_nir_load_arg(b, &s->args->ac, s->args->ac.gs_tg_info);

      /* Consider a workgroup small if it contains less than 16 triangles.
       *
       * The gs_tg_info[30:22] is the number of primitives, which we know is non-zero,
       * so the below is equivalent to: "ult(ubfe(gs_tg_info, 22, 9), 16)", but
       * ACO can optimize out the comparison to zero (see try_optimize_scc_nocompare).
       */
      nir_ssa_def *small_workgroup =
         nir_ieq_imm(b, nir_iand_imm(b, gs_tg_info, BITFIELD_RANGE(22 + 4, 9 - 4)), 0);

      nir_ssa_def *mask = nir_bcsel(
         b, small_workgroup, nir_imm_int(b, radv_nggc_none),
         nir_imm_int(b, radv_nggc_front_face | radv_nggc_back_face | radv_nggc_small_primitives));
      nir_ssa_def *settings = ac_nir_load_arg(b, &s->args->ac, s->args->ngg_culling_settings);
      replacement = nir_ine_imm(b, nir_iand(b, settings, mask), 0);
      break;
   }
   case nir_intrinsic_load_cull_front_face_enabled_amd:
      replacement = nggc_bool_setting(b, radv_nggc_front_face, s);
      break;
   case nir_intrinsic_load_cull_back_face_enabled_amd:
      replacement = nggc_bool_setting(b, radv_nggc_back_face, s);
      break;
   case nir_intrinsic_load_cull_ccw_amd:
      replacement = nggc_bool_setting(b, radv_nggc_face_is_ccw, s);
      break;
   case nir_intrinsic_load_cull_small_primitives_enabled_amd:
      replacement = nggc_bool_setting(b, radv_nggc_small_primitives, s);
      break;
   case nir_intrinsic_load_cull_small_prim_precision_amd: {
      /* To save space, only the exponent is stored in the high 8 bits.
       * We calculate the precision from those 8 bits:
       * exponent = nggc_settings >> 24
       * precision = 1.0 * 2 ^ exponent
       */
      nir_ssa_def *settings = ac_nir_load_arg(b, &s->args->ac, s->args->ngg_culling_settings);
      nir_ssa_def *exponent = nir_ishr_imm(b, settings, 24u);
      replacement = nir_ldexp(b, nir_imm_float(b, 1.0f), exponent);
      break;
   }

   case nir_intrinsic_load_viewport_xy_scale_and_offset: {
      nir_ssa_def *comps[] = {
         ac_nir_load_arg(b, &s->args->ac, s->args->ngg_viewport_scale[0]),
         ac_nir_load_arg(b, &s->args->ac, s->args->ngg_viewport_scale[1]),
         ac_nir_load_arg(b, &s->args->ac, s->args->ngg_viewport_translate[0]),
         ac_nir_load_arg(b, &s->args->ac, s->args->ngg_viewport_translate[1]),
      };
      replacement = nir_vec(b, comps, 4);
      break;
   }

   case nir_intrinsic_load_ring_task_draw_amd:
      replacement = load_ring(b, RING_TS_DRAW, s);
      break;
   case nir_intrinsic_load_ring_task_payload_amd:
      replacement = load_ring(b, RING_TS_PAYLOAD, s);
      break;
   case nir_intrinsic_load_ring_mesh_scratch_amd:
      replacement = load_ring(b, RING_MS_SCRATCH, s);
      break;
   case nir_intrinsic_load_ring_mesh_scratch_offset_amd:
      /* gs_tg_info[0:11] is ordered_wave_id. Multiply by the ring entry size. */
      replacement = nir_imul_imm(
         b, nir_iand_imm(b, ac_nir_load_arg(b, &s->args->ac, s->args->ac.gs_tg_info), 0xfff),
         RADV_MESH_SCRATCH_ENTRY_BYTES);
      break;
   case nir_intrinsic_load_task_ring_entry_amd:
      replacement = ac_nir_load_arg(b, &s->args->ac, s->args->ac.task_ring_entry);
      break;
   case nir_intrinsic_load_task_ib_addr:
      replacement = ac_nir_load_arg(b, &s->args->ac, s->args->task_ib_addr);
      break;
   case nir_intrinsic_load_task_ib_stride:
      replacement = ac_nir_load_arg(b, &s->args->ac, s->args->task_ib_stride);
      break;
   case nir_intrinsic_load_lshs_vertex_stride_amd: {
      unsigned io_num = stage == MESA_SHADER_VERTEX ?
         s->info->vs.num_linked_outputs :
         s->info->tcs.num_linked_inputs;
      replacement = nir_imm_int(b, io_num * 16);
      break;
   }
   case nir_intrinsic_load_hs_out_patch_data_offset_amd: {
      unsigned out_vertices_per_patch = b->shader->info.tess.tcs_vertices_out;
      unsigned num_tcs_outputs = stage == MESA_SHADER_TESS_CTRL ?
         s->info->tcs.num_linked_outputs : s->info->tes.num_linked_inputs;
      int per_vertex_output_patch_size = out_vertices_per_patch * num_tcs_outputs * 16u;

      if (s->pl_key->dynamic_patch_control_points) {
         nir_ssa_def *num_patches;

         if (stage == MESA_SHADER_TESS_CTRL) {
            nir_ssa_def *arg = ac_nir_load_arg(b, &s->args->ac, s->args->tcs_offchip_layout);
            num_patches = nir_ubfe_imm(b, arg, 6, 8);
         } else {
            num_patches = ac_nir_load_arg(b, &s->args->ac, s->args->tes_num_patches);
         }
         replacement = nir_imul_imm(b, num_patches, per_vertex_output_patch_size);
      } else {
         unsigned num_patches = s->info->num_tess_patches;
         replacement = nir_imm_int(b, num_patches * per_vertex_output_patch_size);
      }
      break;
   }
   case nir_intrinsic_load_sample_positions_amd: {
      uint32_t sample_pos_offset = (RING_PS_SAMPLE_POSITIONS * 16) - 8;

      nir_ssa_def *ring_offsets = ac_nir_load_arg(b, &s->args->ac, s->args->ac.ring_offsets);
      nir_ssa_def *addr = nir_pack_64_2x32(b, ring_offsets);
      nir_ssa_def *sample_id = nir_umin(b, intrin->src[0].ssa, nir_imm_int(b, 7));
      nir_ssa_def *offset = nir_ishl_imm(b, sample_id, 3); /* 2 floats containing samplepos.xy */

      nir_const_value *const_num_samples = nir_src_as_const_value(intrin->src[1]);
      if (const_num_samples) {
         sample_pos_offset += (const_num_samples->u32 << 3);
      } else {
         offset = nir_iadd(b, offset, nir_ishl_imm(b, intrin->src[1].ssa, 3));
      }

      replacement = nir_load_global_amd(b, 2, 32, addr, offset,
                                        .base = sample_pos_offset, .access = ACCESS_NON_WRITEABLE);
      break;
   }
   case nir_intrinsic_load_rasterization_samples_amd:
      if (s->pl_key->dynamic_rasterization_samples) {
         replacement = ac_nir_load_arg(b, &s->args->ac, s->args->ps_num_samples);
      } else {
         replacement = nir_imm_int(b, s->pl_key->ps.num_samples);
      }
      break;
   case nir_intrinsic_load_provoking_vtx_in_prim_amd: {
      if (s->pl_key->dynamic_provoking_vtx_mode) {
         replacement = ac_nir_load_arg(b, &s->args->ac, s->args->ngg_provoking_vtx);
      } else {
         unsigned provoking_vertex = 0;
         if (s->pl_key->vs.provoking_vtx_last) {
            if (stage == MESA_SHADER_VERTEX) {
               provoking_vertex = radv_get_num_vertices_per_prim(s->pl_key) - 1;
            } else if (stage == MESA_SHADER_GEOMETRY) {
               provoking_vertex = b->shader->info.gs.vertices_in - 1;
            } else {
               /* TES won't use this intrinsic, because it can get primitive id directly
                * instead of using this intrinsic to pass primitive id by LDS.
                */
               unreachable("load_provoking_vtx_in_prim_amd is only supported in VS and GS");
            }
         }

         replacement = nir_imm_int(b, provoking_vertex);
      }
      break;
   }
   case nir_intrinsic_atomic_add_gs_emit_prim_count_amd:
      nir_gds_atomic_add_amd(b, 32, intrin->src[0].ssa,
                             nir_imm_int(b, RADV_NGG_QUERY_PIPELINE_STAT_OFFSET),
                             nir_imm_int(b, 0x100));
      break;
   case nir_intrinsic_atomic_add_gen_prim_count_amd:
      nir_gds_atomic_add_amd(b, 32, intrin->src[0].ssa,
                             nir_imm_int(b, RADV_NGG_QUERY_PRIM_GEN_OFFSET(nir_intrinsic_stream_id(intrin))),
                             nir_imm_int(b, 0x100));
      break;
   case nir_intrinsic_atomic_add_xfb_prim_count_amd:
      nir_gds_atomic_add_amd(b, 32, intrin->src[0].ssa,
                             nir_imm_int(b, RADV_NGG_QUERY_PRIM_XFB_OFFSET(nir_intrinsic_stream_id(intrin))),
                             nir_imm_int(b, 0x100));
      break;
   case nir_intrinsic_atomic_add_gs_invocation_count_amd:
      /* TODO: add gs invocation query emulation. */
      break;

   case nir_intrinsic_load_streamout_config_amd:
      replacement = ac_nir_load_arg(b, &s->args->ac, s->args->ac.streamout_config);
      break;
   case nir_intrinsic_load_streamout_write_index_amd:
      replacement = ac_nir_load_arg(b, &s->args->ac, s->args->ac.streamout_write_index);
      break;
   case nir_intrinsic_load_streamout_buffer_amd: {
      nir_ssa_def *ptr =
         nir_pack_64_2x32_split(b, ac_nir_load_arg(b, &s->args->ac, s->args->streamout_buffers),
                                nir_imm_int(b, s->address32_hi));
      replacement = nir_load_smem_amd(b, 4, ptr, nir_imm_int(b, nir_intrinsic_base(intrin) * 16));
      break;
   }
   case nir_intrinsic_load_streamout_offset_amd:
      replacement =
         ac_nir_load_arg(b, &s->args->ac, s->args->ac.streamout_offset[nir_intrinsic_base(intrin)]);
      break;

   case nir_intrinsic_load_lds_ngg_gs_out_vertex_base_amd:
      replacement = nir_imm_int(b, s->info->ngg_info.esgs_ring_size);
      break;
   case nir_intrinsic_load_lds_ngg_scratch_base_amd:
      replacement = nir_imm_int(b, s->info->ngg_info.scratch_lds_base);
      break;
   case nir_intrinsic_load_num_vertices_per_primitive_amd: {
      unsigned num_vertices;

      if (stage == MESA_SHADER_VERTEX) {
         num_vertices = radv_get_num_vertices_per_prim(s->pl_key);
      } else if (stage == MESA_SHADER_TESS_EVAL) {
         if (s->info->tes.point_mode) {
            num_vertices = 1;
         } else if (s->info->tes._primitive_mode == TESS_PRIMITIVE_ISOLINES) {
            num_vertices = 2;
         } else {
            num_vertices = 3;
         }
      } else {
         assert(stage == MESA_SHADER_GEOMETRY);
         switch (s->info->gs.output_prim) {
         case SHADER_PRIM_POINTS:
            num_vertices = 1;
            break;
         case SHADER_PRIM_LINE_STRIP:
            num_vertices = 2;
            break;
         case SHADER_PRIM_TRIANGLE_STRIP:
            num_vertices = 3;
            break;
         default:
            unreachable("invalid GS output primitive");
            break;
         }
      }
      replacement = nir_imm_int(b, num_vertices);
      break;
   }
   case nir_intrinsic_load_ordered_id_amd:
      replacement = nir_ubfe_imm(b, ac_nir_load_arg(b, &s->args->ac, s->args->ac.gs_tg_info), 0, 12);
      break;
   case nir_intrinsic_load_input: {
      /* Only VS inputs need to be lowered at this point. */
      if (stage != MESA_SHADER_VERTEX)
         return false;

      if (s->info->vs.dynamic_inputs) {
         replacement = lower_load_vs_input_from_prolog(b, intrin, s);
      } else {
         /* TODO: Lower non-dynamic inputs too. */
         return false;
      }

      break;
   }
   case nir_intrinsic_load_force_vrs_rates_amd:
      replacement = ac_nir_load_arg(b, &s->args->ac, s->args->ac.force_vrs_rates);
      break;
   default:
      progress = false;
      break;
   }

   if (!progress)
      return false;

   if (replacement)
      nir_ssa_def_rewrite_uses(&intrin->dest.ssa, replacement);

   nir_instr_remove(instr);
   nir_instr_free(instr);

   return true;
}

static nir_ssa_def *
load_gsvs_ring(nir_builder *b, lower_abi_state *s, unsigned stream_id)
{
   nir_ssa_def *ring = load_ring(b, RING_GSVS_GS, s);
   unsigned stream_offset = 0;
   unsigned stride = 0;
   for (unsigned i = 0; i <= stream_id; i++) {
      stride = 4 * s->info->gs.num_stream_output_components[i] * s->info->gs.vertices_out;
      if (i < stream_id)
         stream_offset += stride * s->info->wave_size;
   }

   /* Limit on the stride field for <= GFX7. */
   assert(stride < (1 << 14));

   if (stream_offset) {
      nir_ssa_def *addr =
         nir_pack_64_2x32_split(b, nir_channel(b, ring, 0), nir_channel(b, ring, 1));
      addr = nir_iadd_imm(b, addr, stream_offset);
      ring = nir_vector_insert_imm(b, ring, nir_unpack_64_2x32_split_x(b, addr), 0);
      ring = nir_vector_insert_imm(b, ring, nir_unpack_64_2x32_split_y(b, addr), 1);
   }

   ring = nir_vector_insert_imm(
      b, ring, nir_ior_imm(b, nir_channel(b, ring, 1), S_008F04_STRIDE(stride)), 1);
   return nir_vector_insert_imm(b, ring, nir_imm_int(b, s->info->wave_size), 2);
}

void
radv_nir_lower_abi(nir_shader *shader, enum amd_gfx_level gfx_level,
                   const struct radv_shader_info *info, const struct radv_shader_args *args,
                   const struct radv_pipeline_key *pl_key, uint32_t address32_hi)
{
   lower_abi_state state = {
      .gfx_level = gfx_level,
      .info = info,
      .args = args,
      .pl_key = pl_key,
      .address32_hi = address32_hi,
   };

   if (shader->info.stage == MESA_SHADER_GEOMETRY && !info->is_ngg) {
      nir_function_impl *impl = nir_shader_get_entrypoint(shader);

      nir_builder b;
      nir_builder_init(&b, impl);
      b.cursor = nir_before_cf_list(&impl->body);

      u_foreach_bit (i, shader->info.gs.active_stream_mask)
         state.gsvs_ring[i] = load_gsvs_ring(&b, &state, i);
   }

   nir_shader_instructions_pass(shader, lower_abi_instr,
                                nir_metadata_dominance | nir_metadata_block_index, &state);
}
