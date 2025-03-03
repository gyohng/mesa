/*
 * Copyright 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef AC_SHADER_ABI_H
#define AC_SHADER_ABI_H

#include "ac_shader_args.h"
#include "ac_shader_util.h"
#include "compiler/shader_enums.h"
#include "nir.h"
#include <llvm-c/Core.h>

#include <assert.h>

#define AC_LLVM_MAX_OUTPUTS (VARYING_SLOT_VAR31 + 1)

/* Document the shader ABI during compilation. This is what allows radeonsi and
 * radv to share a compiler backend.
 */
struct ac_shader_abi {
   /* Each entry is a pointer to a f32 or a f16 value (only possible for FS) */
   LLVMValueRef outputs[AC_LLVM_MAX_OUTPUTS * 4];
   bool is_16bit[AC_LLVM_MAX_OUTPUTS * 4];

   /* These input registers sometimes need to be fixed up. */
   LLVMValueRef vertex_id;
   LLVMValueRef instance_id;
   LLVMValueRef persp_centroid, linear_centroid;
   LLVMValueRef color0, color1;
   LLVMValueRef user_data;

   /* replaced registers when culling enabled */
   LLVMValueRef vertex_id_replaced;
   LLVMValueRef instance_id_replaced;
   LLVMValueRef tes_u_replaced;
   LLVMValueRef tes_v_replaced;
   LLVMValueRef tes_rel_patch_id_replaced;
   LLVMValueRef tes_patch_id_replaced;

   /* Varying -> attribute number mapping. Also NIR-only */
   unsigned fs_input_attr_indices[MAX_VARYING];

   void (*emit_primitive)(struct ac_shader_abi *abi, unsigned stream);

   void (*emit_vertex_with_counter)(struct ac_shader_abi *abi, unsigned stream,
                                    LLVMValueRef vertexidx, LLVMValueRef *addrs);

   LLVMValueRef (*load_inputs)(struct ac_shader_abi *abi,
                               unsigned driver_location, unsigned component,
                               unsigned num_components, unsigned vertex_index,
                               LLVMTypeRef type);

   LLVMValueRef (*load_tess_varyings)(struct ac_shader_abi *abi, LLVMTypeRef type,
                                      LLVMValueRef vertex_index, LLVMValueRef param_index,
                                      unsigned driver_location, unsigned component,
                                      unsigned num_components, bool load_inputs);

   LLVMValueRef (*load_ubo)(struct ac_shader_abi *abi, LLVMValueRef index);

   /**
    * Load the descriptor for the given buffer.
    *
    * \param buffer the buffer as presented in NIR: this is the descriptor
    *               in Vulkan, and the buffer index in OpenGL/Gallium
    * \param write whether buffer contents will be written
    * \param non_uniform whether the buffer descriptor is not assumed to be uniform
    */
   LLVMValueRef (*load_ssbo)(struct ac_shader_abi *abi, LLVMValueRef buffer, bool write, bool non_uniform);

   /**
    * Load a descriptor associated to a sampler.
    *
    * \param index of the descriptor
    * \param desc_type the type of descriptor to load
    */
   LLVMValueRef (*load_sampler_desc)(struct ac_shader_abi *abi, LLVMValueRef index,
                                     enum ac_descriptor_type desc_type);

   LLVMValueRef (*load_sample_position)(struct ac_shader_abi *abi, LLVMValueRef sample_id);

   LLVMValueRef (*emit_fbfetch)(struct ac_shader_abi *abi);

   LLVMValueRef (*intrinsic_load)(struct ac_shader_abi *abi, nir_intrinsic_instr *intrin);

   /* Whether to clamp the shadow reference value to [0,1]on GFX8. Radeonsi currently
    * uses it due to promoting D16 to D32, but radv needs it off. */
   bool clamp_shadow_reference;
   bool interp_at_sample_force_center;

   /* Whether bounds checks are required */
   bool robust_buffer_access;

   /* Check for Inf interpolation coeff */
   bool kill_ps_if_inf_interp;

   /* Whether undef values must be converted to zero */
   bool convert_undef_to_zero;

   /* Clamp div by 0 (so it won't produce NaN) */
   bool clamp_div_by_zero;

   /* Whether to inline the compute dispatch size in user sgprs. */
   bool load_grid_size_from_user_sgpr;

   /* Whether to detect divergent textures/samplers index and apply
    * waterfall to avoid incorrect rendering. */
   bool use_waterfall_for_divergent_tex_samplers;

   /* Whether to disable anisotropic filtering. */
   bool disable_aniso_single_level;

   /* Number of all interpolated inputs */
   unsigned num_interp;
};

#endif /* AC_SHADER_ABI_H */
