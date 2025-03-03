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

#include "radv_cs.h"
#include "radv_debug.h"
#include "radv_meta.h"
#include "radv_private.h"
#include "radv_radeon_winsys.h"
#include "radv_shader.h"
#include "sid.h"
#include "vk_format.h"
#include "vk_util.h"
#include "vk_enum_defines.h"
#include "vk_common_entrypoints.h"
#include "vk_render_pass.h"
#include "vk_framebuffer.h"

#include "ac_debug.h"
#include "ac_shader_args.h"

#include "util/fast_idiv_by_const.h"

enum {
   RADV_PREFETCH_VBO_DESCRIPTORS = (1 << 0),
   RADV_PREFETCH_VS = (1 << 1),
   RADV_PREFETCH_TCS = (1 << 2),
   RADV_PREFETCH_TES = (1 << 3),
   RADV_PREFETCH_GS = (1 << 4),
   RADV_PREFETCH_PS = (1 << 5),
   RADV_PREFETCH_MS = (1 << 6),
   RADV_PREFETCH_SHADERS = (RADV_PREFETCH_VS | RADV_PREFETCH_TCS | RADV_PREFETCH_TES |
                            RADV_PREFETCH_GS | RADV_PREFETCH_PS | RADV_PREFETCH_MS)
};

static void radv_handle_image_transition(struct radv_cmd_buffer *cmd_buffer,
                                         struct radv_image *image,
                                         VkImageLayout src_layout, VkImageLayout dst_layout,
                                         uint32_t src_family_index, uint32_t dst_family_index,
                                         const VkImageSubresourceRange *range,
                                         struct radv_sample_locations_state *sample_locs);

static void radv_set_rt_stack_size(struct radv_cmd_buffer *cmd_buffer, uint32_t size);

static void
radv_bind_dynamic_state(struct radv_cmd_buffer *cmd_buffer, const struct radv_dynamic_state *src)
{
   struct radv_dynamic_state *dest = &cmd_buffer->state.dynamic;
   uint64_t copy_mask = src->mask;
   uint64_t dest_mask = 0;

   dest->vk.dr.rectangle_count = src->vk.dr.rectangle_count;
   dest->sample_location.count = src->sample_location.count;

   if (copy_mask & RADV_DYNAMIC_VIEWPORT) {
      if (dest->vk.vp.viewport_count != src->vk.vp.viewport_count) {
         dest->vk.vp.viewport_count = src->vk.vp.viewport_count;
         dest_mask |= RADV_DYNAMIC_VIEWPORT;
      }

      if (memcmp(&dest->vk.vp.viewports, &src->vk.vp.viewports,
                 src->vk.vp.viewport_count * sizeof(VkViewport))) {
         typed_memcpy(dest->vk.vp.viewports, src->vk.vp.viewports, src->vk.vp.viewport_count);
         typed_memcpy(dest->hw_vp.xform, src->hw_vp.xform, src->vk.vp.viewport_count);
         dest_mask |= RADV_DYNAMIC_VIEWPORT;
      }
   }

   if (copy_mask & RADV_DYNAMIC_SCISSOR) {
      if (dest->vk.vp.scissor_count != src->vk.vp.scissor_count) {
         dest->vk.vp.scissor_count = src->vk.vp.scissor_count;
         dest_mask |= RADV_DYNAMIC_SCISSOR;
      }

      if (memcmp(&dest->vk.vp.scissors, &src->vk.vp.scissors,
                 src->vk.vp.scissor_count * sizeof(VkRect2D))) {
         typed_memcpy(dest->vk.vp.scissors, src->vk.vp.scissors, src->vk.vp.scissor_count);
         dest_mask |= RADV_DYNAMIC_SCISSOR;
      }
   }

   if (copy_mask & RADV_DYNAMIC_BLEND_CONSTANTS) {
      if (memcmp(&dest->vk.cb.blend_constants, &src->vk.cb.blend_constants, sizeof(src->vk.cb.blend_constants))) {
         typed_memcpy(dest->vk.cb.blend_constants, src->vk.cb.blend_constants, 4);
         dest_mask |= RADV_DYNAMIC_BLEND_CONSTANTS;
      }
   }

   if (copy_mask & RADV_DYNAMIC_DISCARD_RECTANGLE) {
      if (memcmp(&dest->vk.dr.rectangles, &src->vk.dr.rectangles,
                 src->vk.dr.rectangle_count * sizeof(VkRect2D))) {
         typed_memcpy(dest->vk.dr.rectangles, src->vk.dr.rectangles, src->vk.dr.rectangle_count);
         dest_mask |= RADV_DYNAMIC_DISCARD_RECTANGLE;
      }
   }

   if (copy_mask & RADV_DYNAMIC_SAMPLE_LOCATIONS) {
      if (dest->sample_location.per_pixel != src->sample_location.per_pixel ||
          dest->sample_location.grid_size.width != src->sample_location.grid_size.width ||
          dest->sample_location.grid_size.height != src->sample_location.grid_size.height ||
          memcmp(&dest->sample_location.locations, &src->sample_location.locations,
                 src->sample_location.count * sizeof(VkSampleLocationEXT))) {
         dest->sample_location.per_pixel = src->sample_location.per_pixel;
         dest->sample_location.grid_size = src->sample_location.grid_size;
         typed_memcpy(dest->sample_location.locations, src->sample_location.locations,
                      src->sample_location.count);
         dest_mask |= RADV_DYNAMIC_SAMPLE_LOCATIONS;
      }
   }

   if (copy_mask & RADV_DYNAMIC_COLOR_WRITE_MASK) {
      for (uint32_t i = 0; i < MAX_RTS; i++) {
         if (dest->vk.cb.attachments[i].write_mask != src->vk.cb.attachments[i].write_mask) {
            dest->vk.cb.attachments[i].write_mask = src->vk.cb.attachments[i].write_mask;
            dest_mask |= RADV_DYNAMIC_COLOR_WRITE_MASK;
         }
      }

      if (cmd_buffer->device->physical_device->rad_info.rbplus_allowed &&
          (dest_mask & RADV_DYNAMIC_COLOR_WRITE_MASK))
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RBPLUS;
   }

   if (copy_mask & RADV_DYNAMIC_COLOR_BLEND_ENABLE) {
      for (uint32_t i = 0; i < MAX_RTS; i++) {
         if (dest->vk.cb.attachments[i].blend_enable != src->vk.cb.attachments[i].blend_enable) {
            dest->vk.cb.attachments[i].blend_enable = src->vk.cb.attachments[i].blend_enable;
            dest_mask |= RADV_DYNAMIC_COLOR_BLEND_ENABLE;
         }
      }
   }

   if (copy_mask & RADV_DYNAMIC_COLOR_BLEND_EQUATION) {
      for (uint32_t i = 0; i < MAX_RTS; i++) {
         if (dest->vk.cb.attachments[i].src_color_blend_factor != src->vk.cb.attachments[i].src_color_blend_factor ||
             dest->vk.cb.attachments[i].dst_color_blend_factor != src->vk.cb.attachments[i].dst_color_blend_factor ||
             dest->vk.cb.attachments[i].color_blend_op != src->vk.cb.attachments[i].color_blend_op ||
             dest->vk.cb.attachments[i].src_alpha_blend_factor != src->vk.cb.attachments[i].src_alpha_blend_factor ||
             dest->vk.cb.attachments[i].dst_alpha_blend_factor != src->vk.cb.attachments[i].dst_alpha_blend_factor ||
             dest->vk.cb.attachments[i].alpha_blend_op != src->vk.cb.attachments[i].alpha_blend_op) {
            dest->vk.cb.attachments[i].src_color_blend_factor = src->vk.cb.attachments[i].src_color_blend_factor;
            dest->vk.cb.attachments[i].dst_color_blend_factor = src->vk.cb.attachments[i].dst_color_blend_factor;
            dest->vk.cb.attachments[i].color_blend_op = src->vk.cb.attachments[i].color_blend_op;
            dest->vk.cb.attachments[i].src_alpha_blend_factor = src->vk.cb.attachments[i].src_alpha_blend_factor;
            dest->vk.cb.attachments[i].dst_alpha_blend_factor = src->vk.cb.attachments[i].dst_alpha_blend_factor;
            dest->vk.cb.attachments[i].alpha_blend_op = src->vk.cb.attachments[i].alpha_blend_op;
            dest_mask |= RADV_DYNAMIC_COLOR_BLEND_EQUATION;
         }
      }
   }

#define RADV_CMP_COPY(field, flag)                                \
   if (copy_mask & flag) {                                        \
      if (dest->field != src->field) {                            \
         dest->field = src->field;                                \
         dest_mask |= flag;                                       \
      }                                                           \
   }

   RADV_CMP_COPY(vk.ia.primitive_topology, RADV_DYNAMIC_PRIMITIVE_TOPOLOGY);
   RADV_CMP_COPY(vk.ia.primitive_restart_enable, RADV_DYNAMIC_PRIMITIVE_RESTART_ENABLE);

   RADV_CMP_COPY(vk.vp.depth_clip_negative_one_to_one, RADV_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE);

   RADV_CMP_COPY(vk.ts.patch_control_points, RADV_DYNAMIC_PATCH_CONTROL_POINTS);
   RADV_CMP_COPY(vk.ts.domain_origin, RADV_DYNAMIC_TESS_DOMAIN_ORIGIN);

   RADV_CMP_COPY(vk.rs.line.width, RADV_DYNAMIC_LINE_WIDTH);
   RADV_CMP_COPY(vk.rs.depth_bias.constant, RADV_DYNAMIC_DEPTH_BIAS);
   RADV_CMP_COPY(vk.rs.depth_bias.clamp, RADV_DYNAMIC_DEPTH_BIAS);
   RADV_CMP_COPY(vk.rs.depth_bias.slope, RADV_DYNAMIC_DEPTH_BIAS);
   RADV_CMP_COPY(vk.rs.line.stipple.factor, RADV_DYNAMIC_LINE_STIPPLE);
   RADV_CMP_COPY(vk.rs.line.stipple.pattern, RADV_DYNAMIC_LINE_STIPPLE);
   RADV_CMP_COPY(vk.rs.cull_mode, RADV_DYNAMIC_CULL_MODE);
   RADV_CMP_COPY(vk.rs.front_face, RADV_DYNAMIC_FRONT_FACE);
   RADV_CMP_COPY(vk.rs.depth_bias.enable, RADV_DYNAMIC_DEPTH_BIAS_ENABLE);
   RADV_CMP_COPY(vk.rs.rasterizer_discard_enable, RADV_DYNAMIC_RASTERIZER_DISCARD_ENABLE);
   RADV_CMP_COPY(vk.rs.polygon_mode, RADV_DYNAMIC_POLYGON_MODE);
   RADV_CMP_COPY(vk.rs.line.stipple.enable, RADV_DYNAMIC_LINE_STIPPLE_ENABLE);
   RADV_CMP_COPY(vk.rs.depth_clip_enable, RADV_DYNAMIC_DEPTH_CLIP_ENABLE);
   RADV_CMP_COPY(vk.rs.conservative_mode, RADV_DYNAMIC_CONSERVATIVE_RAST_MODE);
   RADV_CMP_COPY(vk.rs.provoking_vertex, RADV_DYNAMIC_PROVOKING_VERTEX_MODE);
   RADV_CMP_COPY(vk.rs.depth_clamp_enable, RADV_DYNAMIC_DEPTH_CLAMP_ENABLE);
   RADV_CMP_COPY(vk.rs.line.mode, RADV_DYNAMIC_LINE_RASTERIZATION_MODE);

   RADV_CMP_COPY(vk.ms.alpha_to_coverage_enable, RADV_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE);
   RADV_CMP_COPY(vk.ms.sample_mask, RADV_DYNAMIC_SAMPLE_MASK);
   RADV_CMP_COPY(vk.ms.rasterization_samples, RADV_DYNAMIC_RASTERIZATION_SAMPLES);

   RADV_CMP_COPY(vk.ds.depth.bounds_test.min, RADV_DYNAMIC_DEPTH_BOUNDS);
   RADV_CMP_COPY(vk.ds.depth.bounds_test.max, RADV_DYNAMIC_DEPTH_BOUNDS);
   RADV_CMP_COPY(vk.ds.stencil.front.compare_mask, RADV_DYNAMIC_STENCIL_COMPARE_MASK);
   RADV_CMP_COPY(vk.ds.stencil.back.compare_mask, RADV_DYNAMIC_STENCIL_COMPARE_MASK);
   RADV_CMP_COPY(vk.ds.stencil.front.write_mask, RADV_DYNAMIC_STENCIL_WRITE_MASK);
   RADV_CMP_COPY(vk.ds.stencil.back.write_mask, RADV_DYNAMIC_STENCIL_WRITE_MASK);
   RADV_CMP_COPY(vk.ds.stencil.front.reference, RADV_DYNAMIC_STENCIL_REFERENCE);
   RADV_CMP_COPY(vk.ds.stencil.back.reference, RADV_DYNAMIC_STENCIL_REFERENCE);
   RADV_CMP_COPY(vk.ds.depth.test_enable, RADV_DYNAMIC_DEPTH_TEST_ENABLE);
   RADV_CMP_COPY(vk.ds.depth.write_enable, RADV_DYNAMIC_DEPTH_WRITE_ENABLE);
   RADV_CMP_COPY(vk.ds.depth.compare_op, RADV_DYNAMIC_DEPTH_COMPARE_OP);
   RADV_CMP_COPY(vk.ds.depth.bounds_test.enable, RADV_DYNAMIC_DEPTH_BOUNDS_TEST_ENABLE);
   RADV_CMP_COPY(vk.ds.stencil.test_enable, RADV_DYNAMIC_STENCIL_TEST_ENABLE);
   RADV_CMP_COPY(vk.ds.stencil.front.op.fail, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.front.op.pass, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.front.op.depth_fail, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.front.op.compare, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.back.op.fail, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.back.op.pass, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.back.op.depth_fail, RADV_DYNAMIC_STENCIL_OP);
   RADV_CMP_COPY(vk.ds.stencil.back.op.compare, RADV_DYNAMIC_STENCIL_OP);

   RADV_CMP_COPY(vk.cb.logic_op, RADV_DYNAMIC_LOGIC_OP);
   RADV_CMP_COPY(vk.cb.color_write_enables, RADV_DYNAMIC_COLOR_WRITE_ENABLE);
   RADV_CMP_COPY(vk.cb.logic_op_enable, RADV_DYNAMIC_LOGIC_OP_ENABLE);

   RADV_CMP_COPY(vk.fsr.fragment_size.width, RADV_DYNAMIC_FRAGMENT_SHADING_RATE);
   RADV_CMP_COPY(vk.fsr.fragment_size.height, RADV_DYNAMIC_FRAGMENT_SHADING_RATE);
   RADV_CMP_COPY(vk.fsr.combiner_ops[0], RADV_DYNAMIC_FRAGMENT_SHADING_RATE);
   RADV_CMP_COPY(vk.fsr.combiner_ops[1], RADV_DYNAMIC_FRAGMENT_SHADING_RATE);

#undef RADV_CMP_COPY

   cmd_buffer->state.dirty |= dest_mask;
}

bool
radv_cmd_buffer_uses_mec(struct radv_cmd_buffer *cmd_buffer)
{
   return cmd_buffer->qf == RADV_QUEUE_COMPUTE &&
          cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX7;
}

enum amd_ip_type
radv_queue_family_to_ring(struct radv_physical_device *physical_device,
                          enum radv_queue_family f)
{
   switch (f) {
   case RADV_QUEUE_GENERAL:
      return AMD_IP_GFX;
   case RADV_QUEUE_COMPUTE:
      return AMD_IP_COMPUTE;
   case RADV_QUEUE_TRANSFER:
      return AMD_IP_SDMA;
   case RADV_QUEUE_VIDEO_DEC:
      return radv_has_uvd(physical_device) ? AMD_IP_UVD : AMD_IP_VCN_DEC;
   case RADV_QUEUE_VIDEO_ENC:
      return AMD_IP_VCN_ENC;
   default:
      unreachable("Unknown queue family");
   }
}

static void
radv_emit_write_data_packet(struct radv_cmd_buffer *cmd_buffer, unsigned engine_sel, uint64_t va,
                            unsigned count, const uint32_t *data)
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   radeon_check_space(cmd_buffer->device->ws, cs, 4 + count);

   radeon_emit(cs, PKT3(PKT3_WRITE_DATA, 2 + count, 0));
   radeon_emit(cs, S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(engine_sel));
   radeon_emit(cs, va);
   radeon_emit(cs, va >> 32);
   radeon_emit_array(cs, data, count);
}

static void
radv_emit_clear_data(struct radv_cmd_buffer *cmd_buffer, unsigned engine_sel, uint64_t va,
                     unsigned size)
{
   uint32_t *zeroes = alloca(size);
   memset(zeroes, 0, size);
   radv_emit_write_data_packet(cmd_buffer, engine_sel, va, size / 4, zeroes);
}

static void
radv_destroy_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer)
{
   struct radv_cmd_buffer *cmd_buffer = container_of(vk_cmd_buffer, struct radv_cmd_buffer, vk);

   list_for_each_entry_safe(struct radv_cmd_buffer_upload, up, &cmd_buffer->upload.list, list)
   {
      radv_rmv_log_command_buffer_bo_destroy(cmd_buffer->device, up->upload_bo);
      cmd_buffer->device->ws->buffer_destroy(cmd_buffer->device->ws, up->upload_bo);
      list_del(&up->list);
      free(up);
   }

   if (cmd_buffer->upload.upload_bo) {
      radv_rmv_log_command_buffer_bo_destroy(cmd_buffer->device, cmd_buffer->upload.upload_bo);
      cmd_buffer->device->ws->buffer_destroy(cmd_buffer->device->ws, cmd_buffer->upload.upload_bo);
   }

   if (cmd_buffer->cs)
      cmd_buffer->device->ws->cs_destroy(cmd_buffer->cs);
   if (cmd_buffer->ace_internal.cs)
      cmd_buffer->device->ws->cs_destroy(cmd_buffer->ace_internal.cs);

   for (unsigned i = 0; i < MAX_BIND_POINTS; i++) {
      struct radv_descriptor_set_header *set = &cmd_buffer->descriptors[i].push_set.set;
      free(set->mapped_ptr);
      if (set->layout)
         vk_descriptor_set_layout_unref(&cmd_buffer->device->vk, &set->layout->vk);
      vk_object_base_finish(&set->base);
   }

   vk_object_base_finish(&cmd_buffer->meta_push_descriptors.base);

   vk_command_buffer_finish(&cmd_buffer->vk);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer);
}

static VkResult
radv_create_cmd_buffer(struct vk_command_pool *pool,
                       struct vk_command_buffer **cmd_buffer_out)
{
   struct radv_device *device = container_of(pool->base.device, struct radv_device, vk);

   struct radv_cmd_buffer *cmd_buffer;
   unsigned ring;
   cmd_buffer = vk_zalloc(&pool->alloc, sizeof(*cmd_buffer), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result =
      vk_command_buffer_init(pool, &cmd_buffer->vk, &radv_cmd_buffer_ops, 0);
   if (result != VK_SUCCESS) {
      vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer);
      return result;
   }

   cmd_buffer->device = device;

   cmd_buffer->qf = vk_queue_to_radv(device->physical_device, pool->queue_family_index);

   ring = radv_queue_family_to_ring(device->physical_device, cmd_buffer->qf);

   cmd_buffer->cs = device->ws->cs_create(device->ws, ring);
   if (!cmd_buffer->cs) {
      radv_destroy_cmd_buffer(&cmd_buffer->vk);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   vk_object_base_init(&device->vk, &cmd_buffer->meta_push_descriptors.base,
                       VK_OBJECT_TYPE_DESCRIPTOR_SET);

   for (unsigned i = 0; i < MAX_BIND_POINTS; i++)
      vk_object_base_init(&device->vk, &cmd_buffer->descriptors[i].push_set.set.base,
                          VK_OBJECT_TYPE_DESCRIPTOR_SET);

   *cmd_buffer_out = &cmd_buffer->vk;

   list_inithead(&cmd_buffer->upload.list);

   return VK_SUCCESS;
}

void
radv_cmd_buffer_reset_rendering(struct radv_cmd_buffer *cmd_buffer)
{
   memset(&cmd_buffer->state.render, 0, sizeof(cmd_buffer->state.render));
}

static void
radv_reset_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer,
                      UNUSED VkCommandBufferResetFlags flags)
{
   struct radv_cmd_buffer *cmd_buffer = container_of(vk_cmd_buffer, struct radv_cmd_buffer, vk);

   vk_command_buffer_reset(&cmd_buffer->vk);

   cmd_buffer->device->ws->cs_reset(cmd_buffer->cs);
   if (cmd_buffer->ace_internal.cs)
      cmd_buffer->device->ws->cs_reset(cmd_buffer->ace_internal.cs);

   list_for_each_entry_safe(struct radv_cmd_buffer_upload, up, &cmd_buffer->upload.list, list)
   {
      radv_rmv_log_command_buffer_bo_destroy(cmd_buffer->device, up->upload_bo);
      cmd_buffer->device->ws->buffer_destroy(cmd_buffer->device->ws, up->upload_bo);
      list_del(&up->list);
      free(up);
   }

   cmd_buffer->push_constant_stages = 0;
   cmd_buffer->scratch_size_per_wave_needed = 0;
   cmd_buffer->scratch_waves_wanted = 0;
   cmd_buffer->compute_scratch_size_per_wave_needed = 0;
   cmd_buffer->compute_scratch_waves_wanted = 0;
   cmd_buffer->esgs_ring_size_needed = 0;
   cmd_buffer->gsvs_ring_size_needed = 0;
   cmd_buffer->tess_rings_needed = false;
   cmd_buffer->task_rings_needed = false;
   cmd_buffer->mesh_scratch_ring_needed = false;
   cmd_buffer->gds_needed = false;
   cmd_buffer->gds_oa_needed = false;
   cmd_buffer->sample_positions_needed = false;
   cmd_buffer->ace_internal.sem.gfx2ace_value = 0;
   cmd_buffer->ace_internal.sem.emitted_gfx2ace_value = 0;
   cmd_buffer->ace_internal.sem.va = 0;

   if (cmd_buffer->upload.upload_bo)
      radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, cmd_buffer->upload.upload_bo);
   cmd_buffer->upload.offset = 0;

   memset(cmd_buffer->vertex_binding_buffers, 0, sizeof(struct radv_buffer *) * cmd_buffer->used_vertex_bindings);
   cmd_buffer->used_vertex_bindings = 0;

   for (unsigned i = 0; i < MAX_BIND_POINTS; i++) {
      cmd_buffer->descriptors[i].dirty = 0;
      cmd_buffer->descriptors[i].valid = 0;
   }

   radv_cmd_buffer_reset_rendering(cmd_buffer);
}

const struct vk_command_buffer_ops radv_cmd_buffer_ops = {
   .create = radv_create_cmd_buffer,
   .reset = radv_reset_cmd_buffer,
   .destroy = radv_destroy_cmd_buffer,
};

static bool
radv_cmd_buffer_resize_upload_buf(struct radv_cmd_buffer *cmd_buffer, uint64_t min_needed)
{
   uint64_t new_size;
   struct radeon_winsys_bo *bo = NULL;
   struct radv_cmd_buffer_upload *upload;
   struct radv_device *device = cmd_buffer->device;

   new_size = MAX2(min_needed, 16 * 1024);
   new_size = MAX2(new_size, 2 * cmd_buffer->upload.size);

   VkResult result =
      device->ws->buffer_create(device->ws, new_size, 4096, device->ws->cs_domain(device->ws),
                                RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING |
                                   RADEON_FLAG_32BIT | RADEON_FLAG_GTT_WC,
                                RADV_BO_PRIORITY_UPLOAD_BUFFER, 0, &bo);

   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, result);
      return false;
   }

   radv_cs_add_buffer(device->ws, cmd_buffer->cs, bo);
   if (cmd_buffer->upload.upload_bo) {
      upload = malloc(sizeof(*upload));

      if (!upload) {
         vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         device->ws->buffer_destroy(device->ws, bo);
         return false;
      }

      memcpy(upload, &cmd_buffer->upload, sizeof(*upload));
      list_add(&upload->list, &cmd_buffer->upload.list);
   }

   cmd_buffer->upload.upload_bo = bo;
   cmd_buffer->upload.size = new_size;
   cmd_buffer->upload.offset = 0;
   cmd_buffer->upload.map = device->ws->buffer_map(cmd_buffer->upload.upload_bo);

   if (!cmd_buffer->upload.map) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return false;
   }
   radv_rmv_log_command_buffer_bo_create(device, cmd_buffer->upload.upload_bo, 0,
                                         cmd_buffer->upload.size, 0);

   return true;
}

bool
radv_cmd_buffer_upload_alloc_aligned(struct radv_cmd_buffer *cmd_buffer, unsigned size,
                                     unsigned alignment,
                                     unsigned *out_offset, void **ptr)
{
   assert(size % 4 == 0);

   struct radeon_info *rad_info = &cmd_buffer->device->physical_device->rad_info;

   /* Align to the scalar cache line size if it results in this allocation
    * being placed in less of them.
    */
   unsigned offset = cmd_buffer->upload.offset;
   unsigned line_size = rad_info->gfx_level >= GFX10 ? 64 : 32;
   unsigned gap = align(offset, line_size) - offset;
   if ((size & (line_size - 1)) > gap)
      offset = align(offset, line_size);

   if (alignment)
      offset = align(offset, alignment);
   if (offset + size > cmd_buffer->upload.size) {
      if (!radv_cmd_buffer_resize_upload_buf(cmd_buffer, size))
         return false;
      offset = 0;
   }

   *out_offset = offset;
   *ptr = cmd_buffer->upload.map + offset;

   cmd_buffer->upload.offset = offset + size;
   return true;
}

bool
radv_cmd_buffer_upload_alloc(struct radv_cmd_buffer *cmd_buffer, unsigned size,
                             unsigned *out_offset, void **ptr)
{
   return radv_cmd_buffer_upload_alloc_aligned(cmd_buffer, size, 0, out_offset, ptr);
}

bool
radv_cmd_buffer_upload_data(struct radv_cmd_buffer *cmd_buffer, unsigned size, const void *data,
                            unsigned *out_offset)
{
   uint8_t *ptr;

   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, size, out_offset, (void **)&ptr))
      return false;
   assert(ptr);

   memcpy(ptr, data, size);
   return true;
}

void
radv_cmd_buffer_trace_emit(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = cmd_buffer->device;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint64_t va;

   va = radv_buffer_get_va(device->trace_bo);
   if (cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
      va += 4;

   ++cmd_buffer->state.trace_id;
   radv_emit_write_data_packet(cmd_buffer, V_370_ME, va, 1, &cmd_buffer->state.trace_id);

   radeon_check_space(cmd_buffer->device->ws, cs, 2);

   radeon_emit(cs, PKT3(PKT3_NOP, 0, 0));
   radeon_emit(cs, AC_ENCODE_TRACE_POINT(cmd_buffer->state.trace_id));
}

static void
radv_ace_internal_barrier(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 src_stage_mask,
                          VkPipelineStageFlags2 dst_stage_mask)
{
   /* Update flush bits from the main cmdbuf, except the stage flush. */
   cmd_buffer->ace_internal.flush_bits |=
      cmd_buffer->state.flush_bits & RADV_CMD_FLUSH_ALL_COMPUTE & ~RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   /* Add stage flush only when necessary. */
   if (src_stage_mask &
       (VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_TRANSFER_BIT |
        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT))
      cmd_buffer->ace_internal.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   /* Block task shaders when we have to wait for CP DMA on the GFX cmdbuf. */
   if (src_stage_mask &
       (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT |
        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT))
      dst_stage_mask |= cmd_buffer->state.dma_is_busy ? VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT : 0;

   /* Increment the GFX/ACE semaphore when task shaders are blocked. */
   if (dst_stage_mask &
       (VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT_KHR | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
        VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT))
      cmd_buffer->ace_internal.sem.gfx2ace_value++;
}

static void
radv_ace_internal_cache_flush(struct radv_cmd_buffer *cmd_buffer)
{
   struct radeon_cmdbuf *ace_cs = cmd_buffer->ace_internal.cs;
   const uint32_t flush_bits = cmd_buffer->ace_internal.flush_bits;
   enum rgp_flush_bits sqtt_flush_bits = 0;

   si_cs_emit_cache_flush(ace_cs, cmd_buffer->device->physical_device->rad_info.gfx_level, NULL, 0,
                          true, flush_bits, &sqtt_flush_bits, 0);

   cmd_buffer->ace_internal.flush_bits = 0;
}

static uint64_t
radv_ace_internal_sem_create(struct radv_cmd_buffer *cmd_buffer)
{
   /* DWORD 0: GFX->ACE semaphore (GFX blocks ACE, ie. ACE waits for GFX)
    * DWORD 1: ACE->GFX semaphore
    */
   uint64_t sem_init = 0;
   uint32_t va_off = 0;
   if (!radv_cmd_buffer_upload_data(cmd_buffer, sizeof(uint64_t), &sem_init, &va_off)) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return 0;
   }

   return radv_buffer_get_va(cmd_buffer->upload.upload_bo) + va_off;
}

static bool
radv_ace_internal_sem_dirty(const struct radv_cmd_buffer *cmd_buffer)
{
   return cmd_buffer->ace_internal.sem.gfx2ace_value !=
          cmd_buffer->ace_internal.sem.emitted_gfx2ace_value;
}

ALWAYS_INLINE static bool
radv_flush_gfx2ace_semaphore(struct radv_cmd_buffer *cmd_buffer)
{
   if (!radv_ace_internal_sem_dirty(cmd_buffer))
      return false;

   if (!cmd_buffer->ace_internal.sem.va) {
      cmd_buffer->ace_internal.sem.va = radv_ace_internal_sem_create(cmd_buffer);
      if (!cmd_buffer->ace_internal.sem.va)
         return false;
   }

   /* GFX writes a value to the semaphore which ACE can wait for.*/
   si_cs_emit_write_event_eop(
      cmd_buffer->cs, cmd_buffer->device->physical_device->rad_info.gfx_level,
      radv_cmd_buffer_uses_mec(cmd_buffer), V_028A90_BOTTOM_OF_PIPE_TS, 0, EOP_DST_SEL_MEM,
      EOP_DATA_SEL_VALUE_32BIT, cmd_buffer->ace_internal.sem.va,
      cmd_buffer->ace_internal.sem.gfx2ace_value, cmd_buffer->gfx9_eop_bug_va);

   cmd_buffer->ace_internal.sem.emitted_gfx2ace_value = cmd_buffer->ace_internal.sem.gfx2ace_value;
   return true;
}

ALWAYS_INLINE static void
radv_wait_gfx2ace_semaphore(struct radv_cmd_buffer *cmd_buffer)
{
   assert(cmd_buffer->ace_internal.sem.va);
   struct radeon_cmdbuf *ace_cs = cmd_buffer->ace_internal.cs;
   radeon_check_space(cmd_buffer->device->ws, ace_cs, 7);

   /* ACE waits for the semaphore which GFX wrote. */
   radv_cp_wait_mem(ace_cs, WAIT_REG_MEM_GREATER_OR_EQUAL, cmd_buffer->ace_internal.sem.va,
                    cmd_buffer->ace_internal.sem.gfx2ace_value, 0xffffffff);
}

static struct radeon_cmdbuf *
radv_ace_internal_create(struct radv_cmd_buffer *cmd_buffer)
{
   assert(!cmd_buffer->ace_internal.cs);
   struct radv_device *device = cmd_buffer->device;
   struct radeon_cmdbuf *ace_cs = device->ws->cs_create(device->ws, AMD_IP_COMPUTE);

   if (!ace_cs)
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

   return ace_cs;
}

static VkResult
radv_ace_internal_finalize(struct radv_cmd_buffer *cmd_buffer)
{
   assert(cmd_buffer->ace_internal.cs);
   struct radv_device *device = cmd_buffer->device;
   struct radeon_cmdbuf *ace_cs = cmd_buffer->ace_internal.cs;

   /* Emit pending cache flush. */
   radv_ace_internal_cache_flush(cmd_buffer);

   /* Clear the GFX<->ACE semaphores if they exist.
    * This is necessary in case the same cmd buffer is submitted again in the future.
    */
   if (cmd_buffer->ace_internal.sem.va) {
      struct radeon_cmdbuf *cs = cmd_buffer->cs;
      uint64_t gfx2ace_va = cmd_buffer->ace_internal.sem.va;
      uint64_t ace2gfx_va = cmd_buffer->ace_internal.sem.va + 4;

      /* ACE: write 0 to the GFX->ACE semaphore. */
      radeon_emit(ace_cs, PKT3(PKT3_WRITE_DATA, 3, 0));
      radeon_emit(ace_cs, S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(V_370_ME));
      radeon_emit(ace_cs, gfx2ace_va);
      radeon_emit(ace_cs, gfx2ace_va >> 32);
      radeon_emit(ace_cs, 0);

      /* GFX: write 0 to the ACE->GFX semaphore. */
      radeon_emit(cs, PKT3(PKT3_WRITE_DATA, 3, 0));
      radeon_emit(cs, S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(V_370_ME));
      radeon_emit(cs, ace2gfx_va);
      radeon_emit(cs, ace2gfx_va >> 32);
      radeon_emit(cs, 0);
   }

   return device->ws->cs_finalize(ace_cs);
}

static void
radv_cmd_buffer_after_draw(struct radv_cmd_buffer *cmd_buffer, enum radv_cmd_flush_bits flags)
{
   const struct radv_device *device = cmd_buffer->device;
   if (unlikely(device->thread_trace.bo)) {
      radeon_check_space(device->ws, cmd_buffer->cs, 2);

      radeon_emit(cmd_buffer->cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cmd_buffer->cs, EVENT_TYPE(V_028A90_THREAD_TRACE_MARKER) | EVENT_INDEX(0));
   }

   if (device->instance->debug_flags & RADV_DEBUG_SYNC_SHADERS) {
      enum rgp_flush_bits sqtt_flush_bits = 0;
      assert(flags & (RADV_CMD_FLAG_PS_PARTIAL_FLUSH | RADV_CMD_FLAG_CS_PARTIAL_FLUSH));

      ASSERTED const unsigned cdw_max = radeon_check_space(device->ws, cmd_buffer->cs, 4);

      /* Force wait for graphics or compute engines to be idle. */
      si_cs_emit_cache_flush(cmd_buffer->cs, device->physical_device->rad_info.gfx_level,
                             &cmd_buffer->gfx9_fence_idx, cmd_buffer->gfx9_fence_va,
                             radv_cmd_buffer_uses_mec(cmd_buffer), flags, &sqtt_flush_bits,
                             cmd_buffer->gfx9_eop_bug_va);

      assert(cmd_buffer->cs->cdw <= cdw_max);

      if (cmd_buffer->state.graphics_pipeline && (flags & RADV_CMD_FLAG_PS_PARTIAL_FLUSH) &&
          radv_pipeline_has_stage(cmd_buffer->state.graphics_pipeline, MESA_SHADER_TASK)) {
         /* Force wait for compute engines to be idle on the internal cmdbuf. */
         si_cs_emit_cache_flush(cmd_buffer->ace_internal.cs,
                                device->physical_device->rad_info.gfx_level, NULL, 0, true,
                                RADV_CMD_FLAG_CS_PARTIAL_FLUSH, &sqtt_flush_bits, 0);
      }
   }

   if (unlikely(device->trace_bo))
      radv_cmd_buffer_trace_emit(cmd_buffer);
}

static void
radv_save_pipeline(struct radv_cmd_buffer *cmd_buffer, struct radv_pipeline *pipeline)
{
   struct radv_device *device = cmd_buffer->device;
   enum amd_ip_type ring;
   uint32_t data[2];
   uint64_t va;

   va = radv_buffer_get_va(device->trace_bo);

   ring = radv_queue_family_to_ring(device->physical_device, cmd_buffer->qf);

   switch (ring) {
   case AMD_IP_GFX:
      va += 8;
      break;
   case AMD_IP_COMPUTE:
      va += 16;
      break;
   default:
      assert(!"invalid IP type");
   }

   uint64_t pipeline_address = (uintptr_t)pipeline;
   data[0] = pipeline_address;
   data[1] = pipeline_address >> 32;

   radv_emit_write_data_packet(cmd_buffer, V_370_ME, va, 2, data);
}

static void
radv_save_vertex_descriptors(struct radv_cmd_buffer *cmd_buffer, uint64_t vb_ptr)
{
   struct radv_device *device = cmd_buffer->device;
   uint32_t data[2];
   uint64_t va;

   va = radv_buffer_get_va(device->trace_bo);
   va += 24;

   data[0] = vb_ptr;
   data[1] = vb_ptr >> 32;

   radv_emit_write_data_packet(cmd_buffer, V_370_ME, va, 2, data);
}

static void
radv_save_vs_prolog(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader_part *prolog)
{
   struct radv_device *device = cmd_buffer->device;
   uint32_t data[2];
   uint64_t va;

   va = radv_buffer_get_va(device->trace_bo);
   va += 32;

   uint64_t prolog_address = (uintptr_t)prolog;
   data[0] = prolog_address;
   data[1] = prolog_address >> 32;

   radv_emit_write_data_packet(cmd_buffer, V_370_ME, va, 2, data);
}

void
radv_set_descriptor_set(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point,
                        struct radv_descriptor_set *set, unsigned idx)
{
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, bind_point);

   descriptors_state->sets[idx] = set;

   descriptors_state->valid |= (1u << idx); /* active descriptors */
   descriptors_state->dirty |= (1u << idx);
}

static void
radv_save_descriptors(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, bind_point);
   struct radv_device *device = cmd_buffer->device;
   uint32_t data[MAX_SETS * 2] = {0};
   uint64_t va;
   va = radv_buffer_get_va(device->trace_bo) + 40;

   u_foreach_bit(i, descriptors_state->valid)
   {
      struct radv_descriptor_set *set = descriptors_state->sets[i];
      data[i * 2] = (uint64_t)(uintptr_t)set;
      data[i * 2 + 1] = (uint64_t)(uintptr_t)set >> 32;
   }

   radv_emit_write_data_packet(cmd_buffer, V_370_ME, va, MAX_SETS * 2, data);
}

struct radv_userdata_info *
radv_lookup_user_sgpr(const struct radv_pipeline *pipeline, gl_shader_stage stage, int idx)
{
   struct radv_shader *shader = radv_get_shader(pipeline, stage);
   return &shader->info.user_sgprs_locs.shader_data[idx];
}

static void
radv_emit_userdata_address(struct radv_device *device, struct radeon_cmdbuf *cs,
                           struct radv_pipeline *pipeline, gl_shader_stage stage, int idx,
                           uint64_t va)
{
   struct radv_userdata_info *loc = radv_lookup_user_sgpr(pipeline, stage, idx);
   uint32_t base_reg = pipeline->user_data_0[stage];
   if (loc->sgpr_idx == -1)
      return;

   assert(loc->num_sgprs == 1);

   radv_emit_shader_pointer(device, cs, base_reg + loc->sgpr_idx * 4, va, false);
}

static uint64_t
radv_descriptor_get_va(const struct radv_descriptor_state *descriptors_state, unsigned set_idx)
{
   struct radv_descriptor_set *set = descriptors_state->sets[set_idx];
   uint64_t va;

   if (set) {
      va = set->header.va;
   } else {
      va = descriptors_state->descriptor_buffers[set_idx];
   }

   return va;
}

static void
radv_emit_descriptor_pointers(struct radv_device *device, struct radeon_cmdbuf *cs,
                              struct radv_pipeline *pipeline,
                              struct radv_descriptor_state *descriptors_state,
                              gl_shader_stage stage)
{
   uint32_t sh_base = pipeline->user_data_0[stage];
   struct radv_userdata_locations *locs = &pipeline->shaders[stage]->info.user_sgprs_locs;
   unsigned mask = locs->descriptor_sets_enabled;

   mask &= descriptors_state->dirty & descriptors_state->valid;

   while (mask) {
      int start, count;

      u_bit_scan_consecutive_range(&mask, &start, &count);

      struct radv_userdata_info *loc = &locs->descriptor_sets[start];
      unsigned sh_offset = sh_base + loc->sgpr_idx * 4;

      radv_emit_shader_pointer_head(cs, sh_offset, count, true);
      for (int i = 0; i < count; i++) {
         uint64_t va = radv_descriptor_get_va(descriptors_state, start + i);

         radv_emit_shader_pointer_body(device, cs, va, true);
      }
   }
}

static ALWAYS_INLINE unsigned
radv_get_rasterization_samples(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   if (d->vk.rs.line.mode == VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT &&
       radv_rast_prim_is_line(pipeline->rast_prim)) {
      /* From the Vulkan spec 1.3.221:
       *
       * "When Bresenham lines are being rasterized, sample locations may all be treated as being at
       * the pixel center (this may affect attribute and depth interpolation)."
       *
       * "One consequence of this is that Bresenham lines cover the same pixels regardless of the
       * number of rasterization samples, and cover all samples in those pixels (unless masked out
       * or killed)."
       */
      return 1;
   }

   return MAX2(1, d->vk.ms.rasterization_samples);
}

static ALWAYS_INLINE unsigned
radv_get_ps_iter_samples(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   unsigned ps_iter_samples = 1;

   if (pipeline->ms.sample_shading_enable) {
      unsigned rasterization_samples = radv_get_rasterization_samples(cmd_buffer);
      unsigned color_samples = MAX2(render->color_samples, rasterization_samples);

      ps_iter_samples = ceilf(pipeline->ms.min_sample_shading * color_samples);
      ps_iter_samples = util_next_power_of_two(ps_iter_samples);
   }

   return ps_iter_samples;
}

/**
 * Convert the user sample locations to hardware sample locations (the values
 * that will be emitted by PA_SC_AA_SAMPLE_LOCS_PIXEL_*).
 */
static void
radv_convert_user_sample_locs(const struct radv_sample_locations_state *state,
                              uint32_t x, uint32_t y, VkOffset2D *sample_locs)
{
   uint32_t x_offset = x % state->grid_size.width;
   uint32_t y_offset = y % state->grid_size.height;
   uint32_t num_samples = (uint32_t)state->per_pixel;
   uint32_t pixel_offset;

   pixel_offset = (x_offset + y_offset * state->grid_size.width) * num_samples;

   assert(pixel_offset <= MAX_SAMPLE_LOCATIONS);
   const VkSampleLocationEXT *user_locs = &state->locations[pixel_offset];

   for (uint32_t i = 0; i < num_samples; i++) {
      float shifted_pos_x = user_locs[i].x - 0.5;
      float shifted_pos_y = user_locs[i].y - 0.5;

      int32_t scaled_pos_x = floorf(shifted_pos_x * 16);
      int32_t scaled_pos_y = floorf(shifted_pos_y * 16);

      sample_locs[i].x = CLAMP(scaled_pos_x, -8, 7);
      sample_locs[i].y = CLAMP(scaled_pos_y, -8, 7);
   }
}

/**
 * Compute the PA_SC_AA_SAMPLE_LOCS_PIXEL_* mask based on hardware sample
 * locations.
 */
static void
radv_compute_sample_locs_pixel(uint32_t num_samples, VkOffset2D *sample_locs,
                               uint32_t *sample_locs_pixel)
{
   for (uint32_t i = 0; i < num_samples; i++) {
      uint32_t sample_reg_idx = i / 4;
      uint32_t sample_loc_idx = i % 4;
      int32_t pos_x = sample_locs[i].x;
      int32_t pos_y = sample_locs[i].y;

      uint32_t shift_x = 8 * sample_loc_idx;
      uint32_t shift_y = shift_x + 4;

      sample_locs_pixel[sample_reg_idx] |= (pos_x & 0xf) << shift_x;
      sample_locs_pixel[sample_reg_idx] |= (pos_y & 0xf) << shift_y;
   }
}

/**
 * Compute the PA_SC_CENTROID_PRIORITY_* mask based on the top left hardware
 * sample locations.
 */
static uint64_t
radv_compute_centroid_priority(struct radv_cmd_buffer *cmd_buffer, VkOffset2D *sample_locs,
                               uint32_t num_samples)
{
   uint32_t *centroid_priorities = alloca(num_samples * sizeof(*centroid_priorities));
   uint32_t sample_mask = num_samples - 1;
   uint32_t *distances = alloca(num_samples * sizeof(*distances));
   uint64_t centroid_priority = 0;

   /* Compute the distances from center for each sample. */
   for (int i = 0; i < num_samples; i++) {
      distances[i] = (sample_locs[i].x * sample_locs[i].x) + (sample_locs[i].y * sample_locs[i].y);
   }

   /* Compute the centroid priorities by looking at the distances array. */
   for (int i = 0; i < num_samples; i++) {
      uint32_t min_idx = 0;

      for (int j = 1; j < num_samples; j++) {
         if (distances[j] < distances[min_idx])
            min_idx = j;
      }

      centroid_priorities[i] = min_idx;
      distances[min_idx] = 0xffffffff;
   }

   /* Compute the final centroid priority. */
   for (int i = 0; i < 8; i++) {
      centroid_priority |= centroid_priorities[i & sample_mask] << (i * 4);
   }

   return centroid_priority << 32 | centroid_priority;
}

/**
 * Emit the sample locations that are specified with VK_EXT_sample_locations.
 */
static void
radv_emit_sample_locations(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   uint32_t num_samples = (uint32_t)d->sample_location.per_pixel;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint32_t sample_locs_pixel[4][2] = {0};
   VkOffset2D sample_locs[4][8]; /* 8 is the max. sample count supported */
   uint64_t centroid_priority;

   if (!d->sample_location.count)
      return;

   /* Convert the user sample locations to hardware sample locations. */
   radv_convert_user_sample_locs(&d->sample_location, 0, 0, sample_locs[0]);
   radv_convert_user_sample_locs(&d->sample_location, 1, 0, sample_locs[1]);
   radv_convert_user_sample_locs(&d->sample_location, 0, 1, sample_locs[2]);
   radv_convert_user_sample_locs(&d->sample_location, 1, 1, sample_locs[3]);

   /* Compute the PA_SC_AA_SAMPLE_LOCS_PIXEL_* mask. */
   for (uint32_t i = 0; i < 4; i++) {
      radv_compute_sample_locs_pixel(num_samples, sample_locs[i], sample_locs_pixel[i]);
   }

   /* Compute the PA_SC_CENTROID_PRIORITY_* mask. */
   centroid_priority = radv_compute_centroid_priority(cmd_buffer, sample_locs[0], num_samples);

   /* Emit the specified user sample locations. */
   switch (num_samples) {
   case 2:
   case 4:
      radeon_set_context_reg(cs, R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0,
                             sample_locs_pixel[0][0]);
      radeon_set_context_reg(cs, R_028C08_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0,
                             sample_locs_pixel[1][0]);
      radeon_set_context_reg(cs, R_028C18_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0,
                             sample_locs_pixel[2][0]);
      radeon_set_context_reg(cs, R_028C28_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0,
                             sample_locs_pixel[3][0]);
      break;
   case 8:
      radeon_set_context_reg(cs, R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0,
                             sample_locs_pixel[0][0]);
      radeon_set_context_reg(cs, R_028C08_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0,
                             sample_locs_pixel[1][0]);
      radeon_set_context_reg(cs, R_028C18_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0,
                             sample_locs_pixel[2][0]);
      radeon_set_context_reg(cs, R_028C28_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0,
                             sample_locs_pixel[3][0]);
      radeon_set_context_reg(cs, R_028BFC_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_1,
                             sample_locs_pixel[0][1]);
      radeon_set_context_reg(cs, R_028C0C_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_1,
                             sample_locs_pixel[1][1]);
      radeon_set_context_reg(cs, R_028C1C_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_1,
                             sample_locs_pixel[2][1]);
      radeon_set_context_reg(cs, R_028C2C_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_1,
                             sample_locs_pixel[3][1]);
      break;
   default:
      unreachable("invalid number of samples");
   }

   radeon_set_context_reg_seq(cs, R_028BD4_PA_SC_CENTROID_PRIORITY_0, 2);
   radeon_emit(cs, centroid_priority);
   radeon_emit(cs, centroid_priority >> 32);

   cmd_buffer->state.context_roll_without_scissor_emitted = true;
}

static void
radv_emit_inline_push_consts(struct radv_device *device, struct radeon_cmdbuf *cs,
                             struct radv_pipeline *pipeline, gl_shader_stage stage, int idx,
                             uint32_t *values)
{
   struct radv_userdata_info *loc = radv_lookup_user_sgpr(pipeline, stage, idx);
   uint32_t base_reg = pipeline->user_data_0[stage];
   if (loc->sgpr_idx == -1)
      return;

   radeon_check_space(device->ws, cs, 2 + loc->num_sgprs);

   radeon_set_sh_reg_seq(cs, base_reg + loc->sgpr_idx * 4, loc->num_sgprs);
   radeon_emit_array(cs, values, loc->num_sgprs);
}

struct radv_bin_size_entry {
   unsigned bpp;
   VkExtent2D extent;
};

static VkExtent2D
radv_gfx10_compute_bin_size(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_physical_device *pdevice = cmd_buffer->device->physical_device;
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   VkExtent2D extent = {512, 512};

   const unsigned db_tag_size = 64;
   const unsigned db_tag_count = 312;
   const unsigned color_tag_size = 1024;
   const unsigned color_tag_count = 31;
   const unsigned fmask_tag_size = 256;
   const unsigned fmask_tag_count = 44;

   const unsigned rb_count = pdevice->rad_info.max_render_backends;
   const unsigned pipe_count = MAX2(rb_count, pdevice->rad_info.num_tcc_blocks);

   const unsigned db_tag_part = (db_tag_count * rb_count / pipe_count) * db_tag_size * pipe_count;
   const unsigned color_tag_part =
      (color_tag_count * rb_count / pipe_count) * color_tag_size * pipe_count;
   const unsigned fmask_tag_part =
      (fmask_tag_count * rb_count / pipe_count) * fmask_tag_size * pipe_count;

   const unsigned total_samples = radv_get_rasterization_samples(cmd_buffer);
   const unsigned samples_log = util_logbase2_ceil(total_samples);

   unsigned color_bytes_per_pixel = 0;
   unsigned fmask_bytes_per_pixel = 0;

   for (unsigned i = 0; i < render->color_att_count; ++i) {
      struct radv_image_view *iview = render->color_att[i].iview;

      if (!iview)
         continue;

      if (!d->vk.cb.attachments[i].write_mask)
         continue;

      color_bytes_per_pixel += vk_format_get_blocksize(render->color_att[i].format);

      if (total_samples > 1) {
         assert(samples_log <= 3);
         const unsigned fmask_array[] = {0, 1, 1, 4};
         fmask_bytes_per_pixel += fmask_array[samples_log];
      }
   }

   color_bytes_per_pixel *= total_samples;
   color_bytes_per_pixel = MAX2(color_bytes_per_pixel, 1);

   const unsigned color_pixel_count_log = util_logbase2(color_tag_part / color_bytes_per_pixel);
   extent.width = 1ull << ((color_pixel_count_log + 1) / 2);
   extent.height = 1ull << (color_pixel_count_log / 2);

   if (fmask_bytes_per_pixel) {
      const unsigned fmask_pixel_count_log = util_logbase2(fmask_tag_part / fmask_bytes_per_pixel);

      const VkExtent2D fmask_extent =
         (VkExtent2D){.width = 1ull << ((fmask_pixel_count_log + 1) / 2),
                      .height = 1ull << (color_pixel_count_log / 2)};

      if (fmask_extent.width * fmask_extent.height < extent.width * extent.height)
         extent = fmask_extent;
   }

   if (render->ds_att.iview) {
      /* Coefficients taken from AMDVLK */
      unsigned depth_coeff = vk_format_has_depth(render->ds_att.format) ? 5 : 0;
      unsigned stencil_coeff = vk_format_has_stencil(render->ds_att.format) ? 1 : 0;
      unsigned db_bytes_per_pixel = (depth_coeff + stencil_coeff) * total_samples;

      const unsigned db_pixel_count_log = util_logbase2(db_tag_part / db_bytes_per_pixel);

      const VkExtent2D db_extent = (VkExtent2D){.width = 1ull << ((db_pixel_count_log + 1) / 2),
                                                .height = 1ull << (color_pixel_count_log / 2)};

      if (db_extent.width * db_extent.height < extent.width * extent.height)
         extent = db_extent;
   }

   extent.width = MAX2(extent.width, 128);
   extent.height = MAX2(extent.width, 64);

   return extent;
}

static VkExtent2D
radv_gfx9_compute_bin_size(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_physical_device *pdevice = cmd_buffer->device->physical_device;
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   static const struct radv_bin_size_entry color_size_table[][3][9] = {
      {
         /* One RB / SE */
         {
            /* One shader engine */
            {0, {128, 128}},
            {1, {64, 128}},
            {2, {32, 128}},
            {3, {16, 128}},
            {17, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Two shader engines */
            {0, {128, 128}},
            {2, {64, 128}},
            {3, {32, 128}},
            {5, {16, 128}},
            {17, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Four shader engines */
            {0, {128, 128}},
            {3, {64, 128}},
            {5, {16, 128}},
            {17, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
      {
         /* Two RB / SE */
         {
            /* One shader engine */
            {0, {128, 128}},
            {2, {64, 128}},
            {3, {32, 128}},
            {5, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Two shader engines */
            {0, {128, 128}},
            {3, {64, 128}},
            {5, {32, 128}},
            {9, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Four shader engines */
            {0, {256, 256}},
            {2, {128, 256}},
            {3, {128, 128}},
            {5, {64, 128}},
            {9, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
      {
         /* Four RB / SE */
         {
            /* One shader engine */
            {0, {128, 256}},
            {2, {128, 128}},
            {3, {64, 128}},
            {5, {32, 128}},
            {9, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Two shader engines */
            {0, {256, 256}},
            {2, {128, 256}},
            {3, {128, 128}},
            {5, {64, 128}},
            {9, {32, 128}},
            {17, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Four shader engines */
            {0, {256, 512}},
            {2, {256, 256}},
            {3, {128, 256}},
            {5, {128, 128}},
            {9, {64, 128}},
            {17, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
   };
   static const struct radv_bin_size_entry ds_size_table[][3][9] = {
      {
         // One RB / SE
         {
            // One shader engine
            {0, {128, 256}},
            {2, {128, 128}},
            {4, {64, 128}},
            {7, {32, 128}},
            {13, {16, 128}},
            {49, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Two shader engines
            {0, {256, 256}},
            {2, {128, 256}},
            {4, {128, 128}},
            {7, {64, 128}},
            {13, {32, 128}},
            {25, {16, 128}},
            {49, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Four shader engines
            {0, {256, 512}},
            {2, {256, 256}},
            {4, {128, 256}},
            {7, {128, 128}},
            {13, {64, 128}},
            {25, {16, 128}},
            {49, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
      {
         // Two RB / SE
         {
            // One shader engine
            {0, {256, 256}},
            {2, {128, 256}},
            {4, {128, 128}},
            {7, {64, 128}},
            {13, {32, 128}},
            {25, {16, 128}},
            {97, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Two shader engines
            {0, {256, 512}},
            {2, {256, 256}},
            {4, {128, 256}},
            {7, {128, 128}},
            {13, {64, 128}},
            {25, {32, 128}},
            {49, {16, 128}},
            {97, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Four shader engines
            {0, {512, 512}},
            {2, {256, 512}},
            {4, {256, 256}},
            {7, {128, 256}},
            {13, {128, 128}},
            {25, {64, 128}},
            {49, {16, 128}},
            {97, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
      {
         // Four RB / SE
         {
            // One shader engine
            {0, {256, 512}},
            {2, {256, 256}},
            {4, {128, 256}},
            {7, {128, 128}},
            {13, {64, 128}},
            {25, {32, 128}},
            {49, {16, 128}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Two shader engines
            {0, {512, 512}},
            {2, {256, 512}},
            {4, {256, 256}},
            {7, {128, 256}},
            {13, {128, 128}},
            {25, {64, 128}},
            {49, {32, 128}},
            {97, {16, 128}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Four shader engines
            {0, {512, 512}},
            {4, {256, 512}},
            {7, {256, 256}},
            {13, {128, 256}},
            {25, {128, 128}},
            {49, {64, 128}},
            {97, {16, 128}},
            {UINT_MAX, {0, 0}},
         },
      },
   };

   VkExtent2D extent = {512, 512};

   unsigned log_num_rb_per_se =
      util_logbase2_ceil(pdevice->rad_info.max_render_backends / pdevice->rad_info.max_se);
   unsigned log_num_se = util_logbase2_ceil(pdevice->rad_info.max_se);

   unsigned total_samples = radv_get_rasterization_samples(cmd_buffer);
   unsigned ps_iter_samples = radv_get_ps_iter_samples(cmd_buffer);
   unsigned effective_samples = total_samples;
   unsigned color_bytes_per_pixel = 0;

   for (unsigned i = 0; i < render->color_att_count; ++i) {
      struct radv_image_view *iview = render->color_att[i].iview;

      if (!iview)
         continue;

      if (!d->vk.cb.attachments[i].write_mask)
         continue;

      color_bytes_per_pixel += vk_format_get_blocksize(render->color_att[i].format);
   }

   /* MSAA images typically don't use all samples all the time. */
   if (effective_samples >= 2 && ps_iter_samples <= 1)
      effective_samples = 2;
   color_bytes_per_pixel *= effective_samples;

   const struct radv_bin_size_entry *color_entry = color_size_table[log_num_rb_per_se][log_num_se];
   while (color_entry[1].bpp <= color_bytes_per_pixel)
      ++color_entry;

   extent = color_entry->extent;

   if (render->ds_att.iview) {
      /* Coefficients taken from AMDVLK */
      unsigned depth_coeff = vk_format_has_depth(render->ds_att.format) ? 5 : 0;
      unsigned stencil_coeff = vk_format_has_stencil(render->ds_att.format) ? 1 : 0;
      unsigned ds_bytes_per_pixel = 4 * (depth_coeff + stencil_coeff) * total_samples;

      const struct radv_bin_size_entry *ds_entry = ds_size_table[log_num_rb_per_se][log_num_se];
      while (ds_entry[1].bpp <= ds_bytes_per_pixel)
         ++ds_entry;

      if (ds_entry->extent.width * ds_entry->extent.height < extent.width * extent.height)
         extent = ds_entry->extent;
   }

   return extent;
}

static unsigned
radv_get_disabled_binning_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_physical_device *pdevice = cmd_buffer->device->physical_device;
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   uint32_t pa_sc_binner_cntl_0;

   if (pdevice->rad_info.gfx_level >= GFX10) {
      unsigned min_bytes_per_pixel = 0;

      for (unsigned i = 0; i < render->color_att_count; ++i) {
         struct radv_image_view *iview = render->color_att[i].iview;

         if (!iview)
            continue;

         if (!d->vk.cb.attachments[i].write_mask)
            continue;

         unsigned bytes = vk_format_get_blocksize(render->color_att[i].format);
         if (!min_bytes_per_pixel || bytes < min_bytes_per_pixel)
            min_bytes_per_pixel = bytes;
      }

      pa_sc_binner_cntl_0 =
         S_028C44_BINNING_MODE(V_028C44_DISABLE_BINNING_USE_NEW_SC) | S_028C44_BIN_SIZE_X(0) |
         S_028C44_BIN_SIZE_Y(0) | S_028C44_BIN_SIZE_X_EXTEND(2) |       /* 128 */
         S_028C44_BIN_SIZE_Y_EXTEND(min_bytes_per_pixel <= 4 ? 2 : 1) | /* 128 or 64 */
         S_028C44_DISABLE_START_OF_PRIM(1) |
         S_028C44_FLUSH_ON_BINNING_TRANSITION(1);
   } else {
      pa_sc_binner_cntl_0 = S_028C44_BINNING_MODE(V_028C44_DISABLE_BINNING_USE_LEGACY_SC) |
                            S_028C44_DISABLE_START_OF_PRIM(1) |
                            S_028C44_FLUSH_ON_BINNING_TRANSITION(pdevice->rad_info.family == CHIP_VEGA12 ||
                                                                 pdevice->rad_info.family == CHIP_VEGA20 ||
                                                                 pdevice->rad_info.family >= CHIP_RAVEN2);
   }

   return pa_sc_binner_cntl_0;
}

static unsigned
radv_get_binning_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = cmd_buffer->device;
   unsigned pa_sc_binner_cntl_0;
   VkExtent2D bin_size;

   if (device->physical_device->rad_info.gfx_level >= GFX10) {
      bin_size = radv_gfx10_compute_bin_size(cmd_buffer);
   } else {
      assert(device->physical_device->rad_info.gfx_level == GFX9);
      bin_size = radv_gfx9_compute_bin_size(cmd_buffer);
   }

   if (device->pbb_allowed && bin_size.width && bin_size.height) {
      struct radv_binning_settings *settings = &device->physical_device->binning_settings;

      pa_sc_binner_cntl_0 = S_028C44_BINNING_MODE(V_028C44_BINNING_ALLOWED) |
                            S_028C44_BIN_SIZE_X(bin_size.width == 16) |
                            S_028C44_BIN_SIZE_Y(bin_size.height == 16) |
                            S_028C44_BIN_SIZE_X_EXTEND(util_logbase2(MAX2(bin_size.width, 32)) - 5) |
                            S_028C44_BIN_SIZE_Y_EXTEND(util_logbase2(MAX2(bin_size.height, 32)) - 5) |
                            S_028C44_CONTEXT_STATES_PER_BIN(settings->context_states_per_bin - 1) |
                            S_028C44_PERSISTENT_STATES_PER_BIN(settings->persistent_states_per_bin - 1) |
                            S_028C44_DISABLE_START_OF_PRIM(1) |
                            S_028C44_FPOVS_PER_BATCH(settings->fpovs_per_batch) |
                            S_028C44_OPTIMAL_BIN_SELECTION(1) |
                            S_028C44_FLUSH_ON_BINNING_TRANSITION(device->physical_device->rad_info.family == CHIP_VEGA12 ||
                                                                 device->physical_device->rad_info.family == CHIP_VEGA20 ||
                                                                 device->physical_device->rad_info.family >= CHIP_RAVEN2);
   } else {
      pa_sc_binner_cntl_0 = radv_get_disabled_binning_state(cmd_buffer);
   }

   return pa_sc_binner_cntl_0;
}

static void
radv_emit_binning_state(struct radv_cmd_buffer *cmd_buffer)
{
   unsigned pa_sc_binner_cntl_0;

   if (cmd_buffer->device->physical_device->rad_info.gfx_level < GFX9)
      return;

   pa_sc_binner_cntl_0 = radv_get_binning_state(cmd_buffer);

   if (pa_sc_binner_cntl_0 == cmd_buffer->state.last_pa_sc_binner_cntl_0)
      return;

   radeon_set_context_reg(cmd_buffer->cs, R_028C44_PA_SC_BINNER_CNTL_0, pa_sc_binner_cntl_0);

   cmd_buffer->state.context_roll_without_scissor_emitted = true;

   cmd_buffer->state.last_pa_sc_binner_cntl_0 = pa_sc_binner_cntl_0;
}

static void
radv_emit_shader_prefetch(struct radv_cmd_buffer *cmd_buffer, struct radv_shader *shader)
{
   uint64_t va;

   if (!shader)
      return;

   va = radv_shader_get_va(shader);

   si_cp_dma_prefetch(cmd_buffer, va, shader->code_size);
}

ALWAYS_INLINE static void
radv_emit_prefetch_L2(struct radv_cmd_buffer *cmd_buffer, struct radv_graphics_pipeline *pipeline,
                      bool first_stage_only)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint32_t mask = state->prefetch_L2_mask;

   /* Fast prefetch path for starting draws as soon as possible. */
   if (first_stage_only)
      mask &= RADV_PREFETCH_VS | RADV_PREFETCH_VBO_DESCRIPTORS | RADV_PREFETCH_MS;

   if (mask & RADV_PREFETCH_VS)
      radv_emit_shader_prefetch(cmd_buffer, pipeline->base.shaders[MESA_SHADER_VERTEX]);

   if (mask & RADV_PREFETCH_MS)
      radv_emit_shader_prefetch(cmd_buffer, pipeline->base.shaders[MESA_SHADER_MESH]);

   if (mask & RADV_PREFETCH_VBO_DESCRIPTORS)
      si_cp_dma_prefetch(cmd_buffer, state->vb_va, pipeline->vb_desc_alloc_size);

   if (mask & RADV_PREFETCH_TCS)
      radv_emit_shader_prefetch(cmd_buffer, pipeline->base.shaders[MESA_SHADER_TESS_CTRL]);

   if (mask & RADV_PREFETCH_TES)
      radv_emit_shader_prefetch(cmd_buffer, pipeline->base.shaders[MESA_SHADER_TESS_EVAL]);

   if (mask & RADV_PREFETCH_GS) {
      radv_emit_shader_prefetch(cmd_buffer, pipeline->base.shaders[MESA_SHADER_GEOMETRY]);
      if (radv_pipeline_has_gs_copy_shader(&pipeline->base))
         radv_emit_shader_prefetch(cmd_buffer, pipeline->base.gs_copy_shader);
   }

   if (mask & RADV_PREFETCH_PS) {
      radv_emit_shader_prefetch(cmd_buffer, pipeline->base.shaders[MESA_SHADER_FRAGMENT]);
      if (pipeline->ps_epilog) {
         struct radv_shader_part *ps_epilog = pipeline->ps_epilog;

         si_cp_dma_prefetch(cmd_buffer, ps_epilog->va, ps_epilog->code_size);
      }
   }

   state->prefetch_L2_mask &= ~mask;
}

static void
radv_emit_rbplus_state(struct radv_cmd_buffer *cmd_buffer)
{
   assert(cmd_buffer->device->physical_device->rad_info.rbplus_allowed);

   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_rendering_state *render = &cmd_buffer->state.render;

   unsigned sx_ps_downconvert = 0;
   unsigned sx_blend_opt_epsilon = 0;
   unsigned sx_blend_opt_control = 0;

   for (unsigned i = 0; i < render->color_att_count; i++) {
      unsigned format, swap;
      bool has_alpha, has_rgb;
      if (render->color_att[i].iview == NULL) {
         /* We don't set the DISABLE bits, because the HW can't have holes,
          * so the SPI color format is set to 32-bit 1-component. */
         sx_ps_downconvert |= V_028754_SX_RT_EXPORT_32_R << (i * 4);
         continue;
      }

      struct radv_color_buffer_info *cb = &render->color_att[i].cb;

      format = cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX11
                  ? G_028C70_FORMAT_GFX11(cb->cb_color_info)
                  : G_028C70_FORMAT_GFX6(cb->cb_color_info);
      swap = G_028C70_COMP_SWAP(cb->cb_color_info);
      has_alpha = cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX11
                     ? !G_028C74_FORCE_DST_ALPHA_1_GFX11(cb->cb_color_attrib)
                     : !G_028C74_FORCE_DST_ALPHA_1_GFX6(cb->cb_color_attrib);

      uint32_t spi_format = (cmd_buffer->state.col_format_non_compacted >> (i * 4)) & 0xf;
      uint32_t colormask = d->vk.cb.attachments[i].write_mask;

      if (format == V_028C70_COLOR_8 || format == V_028C70_COLOR_16 || format == V_028C70_COLOR_32)
         has_rgb = !has_alpha;
      else
         has_rgb = true;

      /* Check the colormask and export format. */
      if (!(colormask & 0x7))
         has_rgb = false;
      if (!(colormask & 0x8))
         has_alpha = false;

      if (spi_format == V_028714_SPI_SHADER_ZERO) {
         has_rgb = false;
         has_alpha = false;
      }

      /* The HW doesn't quite blend correctly with rgb9e5 if we disable the alpha
       * optimization, even though it has no alpha. */
      if (has_rgb && format == V_028C70_COLOR_5_9_9_9)
         has_alpha = true;

      /* Disable value checking for disabled channels. */
      if (!has_rgb)
         sx_blend_opt_control |= S_02875C_MRT0_COLOR_OPT_DISABLE(1) << (i * 4);
      if (!has_alpha)
         sx_blend_opt_control |= S_02875C_MRT0_ALPHA_OPT_DISABLE(1) << (i * 4);

      /* Enable down-conversion for 32bpp and smaller formats. */
      switch (format) {
      case V_028C70_COLOR_8:
      case V_028C70_COLOR_8_8:
      case V_028C70_COLOR_8_8_8_8:
         /* For 1 and 2-channel formats, use the superset thereof. */
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR ||
             spi_format == V_028714_SPI_SHADER_UINT16_ABGR ||
             spi_format == V_028714_SPI_SHADER_SINT16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_8_8_8_8 << (i * 4);

            if (G_028C70_NUMBER_TYPE(cb->cb_color_info) != V_028C70_NUMBER_SRGB)
               sx_blend_opt_epsilon |= V_028758_8BIT_FORMAT_0_5 << (i * 4);
         }
         break;

      case V_028C70_COLOR_5_6_5:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_5_6_5 << (i * 4);
            sx_blend_opt_epsilon |= V_028758_6BIT_FORMAT_0_5 << (i * 4);
         }
         break;

      case V_028C70_COLOR_1_5_5_5:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_1_5_5_5 << (i * 4);
            sx_blend_opt_epsilon |= V_028758_5BIT_FORMAT_0_5 << (i * 4);
         }
         break;

      case V_028C70_COLOR_4_4_4_4:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_4_4_4_4 << (i * 4);
            sx_blend_opt_epsilon |= V_028758_4BIT_FORMAT_0_5 << (i * 4);
         }
         break;

      case V_028C70_COLOR_32:
         if (swap == V_028C70_SWAP_STD && spi_format == V_028714_SPI_SHADER_32_R)
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_32_R << (i * 4);
         else if (swap == V_028C70_SWAP_ALT_REV && spi_format == V_028714_SPI_SHADER_32_AR)
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_32_A << (i * 4);
         break;

      case V_028C70_COLOR_16:
      case V_028C70_COLOR_16_16:
         /* For 1-channel formats, use the superset thereof. */
         if (spi_format == V_028714_SPI_SHADER_UNORM16_ABGR ||
             spi_format == V_028714_SPI_SHADER_SNORM16_ABGR ||
             spi_format == V_028714_SPI_SHADER_UINT16_ABGR ||
             spi_format == V_028714_SPI_SHADER_SINT16_ABGR) {
            if (swap == V_028C70_SWAP_STD || swap == V_028C70_SWAP_STD_REV)
               sx_ps_downconvert |= V_028754_SX_RT_EXPORT_16_16_GR << (i * 4);
            else
               sx_ps_downconvert |= V_028754_SX_RT_EXPORT_16_16_AR << (i * 4);
         }
         break;

      case V_028C70_COLOR_10_11_11:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR)
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_10_11_11 << (i * 4);
         break;

      case V_028C70_COLOR_2_10_10_10:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_2_10_10_10 << (i * 4);
            sx_blend_opt_epsilon |= V_028758_10BIT_FORMAT_0_5 << (i * 4);
         }
         break;
      case V_028C70_COLOR_5_9_9_9:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR)
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_9_9_9_E5 << (i * 4);
         break;
      }
   }

   /* Do not set the DISABLE bits for the unused attachments, as that
    * breaks dual source blending in SkQP and does not seem to improve
    * performance. */

   if (sx_ps_downconvert != cmd_buffer->state.last_sx_ps_downconvert ||
       sx_blend_opt_epsilon != cmd_buffer->state.last_sx_blend_opt_epsilon ||
       sx_blend_opt_control != cmd_buffer->state.last_sx_blend_opt_control) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028754_SX_PS_DOWNCONVERT, 3);
      radeon_emit(cmd_buffer->cs, sx_ps_downconvert);
      radeon_emit(cmd_buffer->cs, sx_blend_opt_epsilon);
      radeon_emit(cmd_buffer->cs, sx_blend_opt_control);

      cmd_buffer->state.context_roll_without_scissor_emitted = true;

      cmd_buffer->state.last_sx_ps_downconvert = sx_ps_downconvert;
      cmd_buffer->state.last_sx_blend_opt_epsilon = sx_blend_opt_epsilon;
      cmd_buffer->state.last_sx_blend_opt_control = sx_blend_opt_control;
   }

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_RBPLUS;
}

static void
radv_emit_ps_epilog_state(struct radv_cmd_buffer *cmd_buffer, struct radv_shader_part *ps_epilog,
                          bool pipeline_is_dirty)
{
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radv_shader *ps_shader = pipeline->base.shaders[MESA_SHADER_FRAGMENT];

   if (cmd_buffer->state.emitted_ps_epilog == ps_epilog && !pipeline_is_dirty)
      return;

   uint32_t col_format = ps_epilog->spi_shader_col_format;
   if (pipeline->need_null_export_workaround && !col_format)
      col_format = V_028714_SPI_SHADER_32_R;
   radeon_set_context_reg(cmd_buffer->cs, R_028714_SPI_SHADER_COL_FORMAT, col_format);
   radeon_set_context_reg(cmd_buffer->cs, R_02823C_CB_SHADER_MASK,
                          ac_get_cb_shader_mask(ps_epilog->spi_shader_col_format));

   /* The main shader must not use less VGPRs than the epilog, otherwise shared vgprs might not
    * work.
    */
   assert(G_00B848_VGPRS(ps_shader->config.rsrc1) >= G_00B848_VGPRS(ps_epilog->rsrc1));

   radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, ps_epilog->bo);

   assert((ps_epilog->va >> 32) == cmd_buffer->device->physical_device->rad_info.address32_hi);

   struct radv_userdata_info *loc =
      &ps_shader->info.user_sgprs_locs.shader_data[AC_UD_PS_EPILOG_PC];
   uint32_t base_reg = pipeline->base.user_data_0[MESA_SHADER_FRAGMENT];
   assert(loc->sgpr_idx != -1);
   assert(loc->num_sgprs == 1);
   radv_emit_shader_pointer(cmd_buffer->device, cmd_buffer->cs, base_reg + loc->sgpr_idx * 4,
                            ps_epilog->va, false);

   cmd_buffer->state.emitted_ps_epilog = ps_epilog;
}

static void
radv_emit_graphics_pipeline(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const struct radv_device *device = cmd_buffer->device;

   if (cmd_buffer->state.emitted_graphics_pipeline == pipeline)
      return;

   if (cmd_buffer->state.emitted_graphics_pipeline) {
      if (radv_rast_prim_is_points_or_lines(cmd_buffer->state.emitted_graphics_pipeline->rast_prim) != radv_rast_prim_is_points_or_lines(pipeline->rast_prim))
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_GUARDBAND;

      if (cmd_buffer->state.emitted_graphics_pipeline->custom_blend_mode != pipeline->custom_blend_mode)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_LOGIC_OP |
                                    RADV_CMD_DIRTY_DYNAMIC_LOGIC_OP_ENABLE;

      if (cmd_buffer->state.emitted_graphics_pipeline->ms.sample_shading_enable != pipeline->ms.sample_shading_enable ||
          cmd_buffer->state.emitted_graphics_pipeline->ms.min_sample_shading != pipeline->ms.min_sample_shading ||
          cmd_buffer->state.emitted_graphics_pipeline->pa_sc_mode_cntl_1 != pipeline->pa_sc_mode_cntl_1 ||
          cmd_buffer->state.emitted_graphics_pipeline->db_render_control != pipeline->db_render_control ||
          cmd_buffer->state.emitted_graphics_pipeline->rast_prim != pipeline->rast_prim)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_RASTERIZATION_SAMPLES;
   }

   radeon_emit_array(cmd_buffer->cs, pipeline->base.cs.buf, pipeline->base.cs.cdw);

   if (!cmd_buffer->state.emitted_graphics_pipeline ||
       cmd_buffer->state.emitted_graphics_pipeline->base.ctx_cs.cdw != pipeline->base.ctx_cs.cdw ||
       cmd_buffer->state.emitted_graphics_pipeline->base.ctx_cs_hash != pipeline->base.ctx_cs_hash ||
       memcmp(cmd_buffer->state.emitted_graphics_pipeline->base.ctx_cs.buf, pipeline->base.ctx_cs.buf,
              pipeline->base.ctx_cs.cdw * 4)) {
      radeon_emit_array(cmd_buffer->cs, pipeline->base.ctx_cs.buf, pipeline->base.ctx_cs.cdw);
      cmd_buffer->state.context_roll_without_scissor_emitted = true;
   }

   if (device->pbb_allowed) {
      struct radv_binning_settings *settings = &device->physical_device->binning_settings;

      if ((!cmd_buffer->state.emitted_graphics_pipeline ||
           cmd_buffer->state.emitted_graphics_pipeline->base.shaders[MESA_SHADER_FRAGMENT] !=
           cmd_buffer->state.graphics_pipeline->base.shaders[MESA_SHADER_FRAGMENT]) &&
          (settings->context_states_per_bin > 1 || settings->persistent_states_per_bin > 1)) {
         /* Break the batch on PS changes. */
         radeon_emit(cmd_buffer->cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
         radeon_emit(cmd_buffer->cs, EVENT_TYPE(V_028A90_BREAK_BATCH) | EVENT_INDEX(0));
      }
   }

   if (pipeline->sqtt_shaders_reloc) {
      /* Emit shaders relocation because RGP requires them to be contiguous in memory. */
      radv_sqtt_emit_relocated_shaders(cmd_buffer, pipeline);
   }

   for (unsigned s = 0; s < MESA_VULKAN_SHADER_STAGES; s++) {
      struct radv_shader *shader = pipeline->base.shaders[s];

      if (!shader)
         continue;

      radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, shader->bo);
   }

   if (pipeline->base.gs_copy_shader) {
      radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, pipeline->base.gs_copy_shader->bo);
   }

   if (unlikely(cmd_buffer->device->trace_bo))
      radv_save_pipeline(cmd_buffer, &pipeline->base);

   cmd_buffer->state.emitted_graphics_pipeline = pipeline;

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_PIPELINE;
}

static enum radv_depth_clamp_mode
radv_get_depth_clamp_mode(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const struct radv_device *device = cmd_buffer->device;
   enum radv_depth_clamp_mode mode;

   mode = RADV_DEPTH_CLAMP_MODE_VIEWPORT;
   if (!d->vk.rs.depth_clamp_enable) {
      /* For optimal performance, depth clamping should always be enabled except if the application
       * disables clamping explicitly or uses depth values outside of the [0.0, 1.0] range.
       */
      if (!d->vk.rs.depth_clip_enable ||
          device->vk.enabled_extensions.EXT_depth_range_unrestricted) {
         mode = RADV_DEPTH_CLAMP_MODE_DISABLED;
      } else {
         mode = RADV_DEPTH_CLAMP_MODE_ZERO_TO_ONE;
      }
   }

   return mode;
}

static void
radv_emit_viewport(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   enum radv_depth_clamp_mode depth_clamp_mode = radv_get_depth_clamp_mode(cmd_buffer);

   assert(d->vk.vp.viewport_count);
   radeon_set_context_reg_seq(cmd_buffer->cs, R_02843C_PA_CL_VPORT_XSCALE,
                              d->vk.vp.viewport_count * 6);

   for (unsigned i = 0; i < d->vk.vp.viewport_count; i++) {
      radeon_emit(cmd_buffer->cs, fui(d->hw_vp.xform[i].scale[0]));
      radeon_emit(cmd_buffer->cs, fui(d->hw_vp.xform[i].translate[0]));
      radeon_emit(cmd_buffer->cs, fui(d->hw_vp.xform[i].scale[1]));
      radeon_emit(cmd_buffer->cs, fui(d->hw_vp.xform[i].translate[1]));

      double scale_z, translate_z;
      if (d->vk.vp.depth_clip_negative_one_to_one) {
         scale_z = d->hw_vp.xform[i].scale[2] * 0.5f;
         translate_z = (d->hw_vp.xform[i].translate[2] + d->vk.vp.viewports[i].maxDepth) * 0.5f;
      } else {
         scale_z = d->hw_vp.xform[i].scale[2];
         translate_z = d->hw_vp.xform[i].translate[2];
      }
      radeon_emit(cmd_buffer->cs, fui(scale_z));
      radeon_emit(cmd_buffer->cs, fui(translate_z));
   }

   radeon_set_context_reg_seq(cmd_buffer->cs, R_0282D0_PA_SC_VPORT_ZMIN_0,
                              d->vk.vp.viewport_count * 2);
   for (unsigned i = 0; i < d->vk.vp.viewport_count; i++) {
      float zmin, zmax;

      if (depth_clamp_mode == RADV_DEPTH_CLAMP_MODE_ZERO_TO_ONE) {
         zmin = 0.0f;
         zmax = 1.0f;
      } else {
         zmin = MIN2(d->vk.vp.viewports[i].minDepth, d->vk.vp.viewports[i].maxDepth);
         zmax = MAX2(d->vk.vp.viewports[i].minDepth, d->vk.vp.viewports[i].maxDepth);
      }

      radeon_emit(cmd_buffer->cs, fui(zmin));
      radeon_emit(cmd_buffer->cs, fui(zmax));
   }
}

void
radv_write_scissors(struct radv_cmd_buffer *cmd_buffer, struct radeon_cmdbuf *cs)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   si_write_scissors(cs, d->vk.vp.scissor_count, d->vk.vp.scissors, d->vk.vp.viewports);
}

static void
radv_emit_scissor(struct radv_cmd_buffer *cmd_buffer)
{
   radv_write_scissors(cmd_buffer, cmd_buffer->cs);

   cmd_buffer->state.context_roll_without_scissor_emitted = false;
}

static void
radv_emit_discard_rectangle(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   if (!d->vk.dr.rectangle_count)
      return;

   radeon_set_context_reg_seq(cmd_buffer->cs, R_028210_PA_SC_CLIPRECT_0_TL,
                              d->vk.dr.rectangle_count * 2);
   for (unsigned i = 0; i < d->vk.dr.rectangle_count; ++i) {
      VkRect2D rect = d->vk.dr.rectangles[i];
      radeon_emit(cmd_buffer->cs, S_028210_TL_X(rect.offset.x) | S_028210_TL_Y(rect.offset.y));
      radeon_emit(cmd_buffer->cs, S_028214_BR_X(rect.offset.x + rect.extent.width) |
                                     S_028214_BR_Y(rect.offset.y + rect.extent.height));
   }
}

static void
radv_emit_line_width(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   radeon_set_context_reg(cmd_buffer->cs, R_028A08_PA_SU_LINE_CNTL,
                          S_028A08_WIDTH(CLAMP(d->vk.rs.line.width * 8, 0, 0xFFFF)));
}

static void
radv_emit_blend_constants(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   radeon_set_context_reg_seq(cmd_buffer->cs, R_028414_CB_BLEND_RED, 4);
   radeon_emit_array(cmd_buffer->cs, (uint32_t *)d->vk.cb.blend_constants, 4);
}

static void
radv_emit_stencil(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   radeon_set_context_reg_seq(cmd_buffer->cs, R_028430_DB_STENCILREFMASK, 2);
   radeon_emit(cmd_buffer->cs, S_028430_STENCILTESTVAL(d->vk.ds.stencil.front.reference) |
                                  S_028430_STENCILMASK(d->vk.ds.stencil.front.compare_mask) |
                                  S_028430_STENCILWRITEMASK(d->vk.ds.stencil.front.write_mask) |
                                  S_028430_STENCILOPVAL(1));
   radeon_emit(cmd_buffer->cs, S_028434_STENCILTESTVAL_BF(d->vk.ds.stencil.back.reference) |
                                  S_028434_STENCILMASK_BF(d->vk.ds.stencil.back.compare_mask) |
                                  S_028434_STENCILWRITEMASK_BF(d->vk.ds.stencil.back.write_mask) |
                                  S_028434_STENCILOPVAL_BF(1));
}

static void
radv_emit_depth_bounds(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   radeon_set_context_reg_seq(cmd_buffer->cs, R_028020_DB_DEPTH_BOUNDS_MIN, 2);
   radeon_emit(cmd_buffer->cs, fui(d->vk.ds.depth.bounds_test.min));
   radeon_emit(cmd_buffer->cs, fui(d->vk.ds.depth.bounds_test.max));
}

static void
radv_emit_depth_bias(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned slope = fui(d->vk.rs.depth_bias.slope * 16.0f);

   radeon_set_context_reg_seq(cmd_buffer->cs, R_028B7C_PA_SU_POLY_OFFSET_CLAMP, 5);
   radeon_emit(cmd_buffer->cs, fui(d->vk.rs.depth_bias.clamp)); /* CLAMP */
   radeon_emit(cmd_buffer->cs, slope);                    /* FRONT SCALE */
   radeon_emit(cmd_buffer->cs, fui(d->vk.rs.depth_bias.constant)); /* FRONT OFFSET */
   radeon_emit(cmd_buffer->cs, slope);                    /* BACK SCALE */
   radeon_emit(cmd_buffer->cs, fui(d->vk.rs.depth_bias.constant)); /* BACK OFFSET */
}

static void
radv_emit_line_stipple(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   uint32_t auto_reset_cntl = 1;

   if (d->vk.ia.primitive_topology == V_008958_DI_PT_LINESTRIP)
      auto_reset_cntl = 2;

   radeon_set_context_reg(cmd_buffer->cs, R_028A0C_PA_SC_LINE_STIPPLE,
                          S_028A0C_LINE_PATTERN(d->vk.rs.line.stipple.pattern) |
                             S_028A0C_REPEAT_COUNT(d->vk.rs.line.stipple.factor - 1) |
                             S_028A0C_AUTO_RESET_CNTL(auto_reset_cntl));
}

uint32_t
radv_get_pa_su_sc_mode_cntl(const struct radv_cmd_buffer *cmd_buffer)
{
   enum amd_gfx_level gfx_level = cmd_buffer->device->physical_device->rad_info.gfx_level;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned pa_su_sc_mode_cntl;

   pa_su_sc_mode_cntl = S_028814_CULL_FRONT(!!(d->vk.rs.cull_mode & VK_CULL_MODE_FRONT_BIT)) |
                        S_028814_CULL_BACK(!!(d->vk.rs.cull_mode & VK_CULL_MODE_BACK_BIT)) |
                        S_028814_FACE(d->vk.rs.front_face) |
                        S_028814_POLY_OFFSET_FRONT_ENABLE(d->vk.rs.depth_bias.enable) |
                        S_028814_POLY_OFFSET_BACK_ENABLE(d->vk.rs.depth_bias.enable) |
                        S_028814_POLY_OFFSET_PARA_ENABLE(d->vk.rs.depth_bias.enable) |
                        S_028814_POLY_MODE(d->vk.rs.polygon_mode != V_028814_X_DRAW_TRIANGLES) |
                        S_028814_POLYMODE_FRONT_PTYPE(d->vk.rs.polygon_mode) |
                        S_028814_POLYMODE_BACK_PTYPE(d->vk.rs.polygon_mode) |
                        S_028814_PROVOKING_VTX_LAST(d->vk.rs.provoking_vertex ==
                                                    VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);

   if (gfx_level >= GFX10) {
      pa_su_sc_mode_cntl |=
         S_028814_KEEP_TOGETHER_ENABLE(d->vk.rs.polygon_mode != V_028814_X_DRAW_TRIANGLES);
   }

   return pa_su_sc_mode_cntl;
}

static void
radv_emit_culling(struct radv_cmd_buffer *cmd_buffer)
{
   unsigned pa_su_sc_mode_cntl = radv_get_pa_su_sc_mode_cntl(cmd_buffer);

   radeon_set_context_reg(cmd_buffer->cs, R_028814_PA_SU_SC_MODE_CNTL, pa_su_sc_mode_cntl);
}

static void
radv_emit_provoking_vertex_mode(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const unsigned stage = pipeline->last_vgt_api_stage;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const struct radv_userdata_info *loc = &pipeline->last_vgt_api_stage_locs[AC_UD_NGG_PROVOKING_VTX];
   unsigned provoking_vtx = 0;
   uint32_t base_reg;

   if (loc->sgpr_idx == -1)
      return;

   if (d->vk.rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT) {
      if (stage == MESA_SHADER_VERTEX) {
         provoking_vtx = si_conv_prim_to_gs_out(d->vk.ia.primitive_topology);
      } else {
         assert(stage == MESA_SHADER_GEOMETRY);
         struct radv_shader *gs = pipeline->base.shaders[stage];
         provoking_vtx = gs->info.gs.vertices_in - 1;
      }
   }

   base_reg = pipeline->base.user_data_0[stage];
   radeon_set_sh_reg(cmd_buffer->cs, base_reg + loc->sgpr_idx * 4, provoking_vtx);
}

static void
radv_emit_primitive_topology(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   assert(!cmd_buffer->state.mesh_shading);

   if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX7) {
      radeon_set_uconfig_reg_idx(cmd_buffer->device->physical_device, cmd_buffer->cs,
                                 R_030908_VGT_PRIMITIVE_TYPE, 1, d->vk.ia.primitive_topology);
   } else {
      radeon_set_config_reg(cmd_buffer->cs, R_008958_VGT_PRIMITIVE_TYPE,
                            d->vk.ia.primitive_topology);
   }
}

static void
radv_emit_depth_control(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   radeon_set_context_reg(
      cmd_buffer->cs, R_028800_DB_DEPTH_CONTROL,
      S_028800_Z_ENABLE(d->vk.ds.depth.test_enable ? 1 : 0) |
         S_028800_Z_WRITE_ENABLE(d->vk.ds.depth.write_enable ? 1 : 0) |
         S_028800_ZFUNC(d->vk.ds.depth.compare_op) |
         S_028800_DEPTH_BOUNDS_ENABLE(d->vk.ds.depth.bounds_test.enable ? 1 : 0) |
         S_028800_STENCIL_ENABLE(d->vk.ds.stencil.test_enable ? 1 : 0) |
         S_028800_BACKFACE_ENABLE(d->vk.ds.stencil.test_enable ? 1 : 0) |
         S_028800_STENCILFUNC(d->vk.ds.stencil.front.op.compare) |
         S_028800_STENCILFUNC_BF(d->vk.ds.stencil.back.op.compare));
}

static void
radv_emit_stencil_control(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   radeon_set_context_reg(
      cmd_buffer->cs, R_02842C_DB_STENCIL_CONTROL,
      S_02842C_STENCILFAIL(si_translate_stencil_op(d->vk.ds.stencil.front.op.fail)) |
         S_02842C_STENCILZPASS(si_translate_stencil_op(d->vk.ds.stencil.front.op.pass)) |
         S_02842C_STENCILZFAIL(si_translate_stencil_op(d->vk.ds.stencil.front.op.depth_fail)) |
         S_02842C_STENCILFAIL_BF(si_translate_stencil_op(d->vk.ds.stencil.back.op.fail)) |
         S_02842C_STENCILZPASS_BF(si_translate_stencil_op(d->vk.ds.stencil.back.op.pass)) |
         S_02842C_STENCILZFAIL_BF(si_translate_stencil_op(d->vk.ds.stencil.back.op.depth_fail)));
}

static void
radv_emit_fragment_shading_rate(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   uint32_t rate_x = MIN2(2, d->vk.fsr.fragment_size.width) - 1;
   uint32_t rate_y = MIN2(2, d->vk.fsr.fragment_size.height) - 1;
   uint32_t pa_cl_vrs_cntl = pipeline->vrs.pa_cl_vrs_cntl;
   uint32_t pipeline_comb_mode = d->vk.fsr.combiner_ops[0];
   uint32_t htile_comb_mode = d->vk.fsr.combiner_ops[1];

   assert(cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX10_3);

   if (!cmd_buffer->state.render.vrs_att.iview) {
      /* When the current subpass has no VRS attachment, the VRS rates are expected to be 1x1, so we
       * can cheat by tweaking the different combiner modes.
       */
      switch (htile_comb_mode) {
      case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR:
         /* The result of min(A, 1x1) is always 1x1. */
         FALLTHROUGH;
      case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR:
         /* Force the per-draw VRS rate to 1x1. */
         rate_x = rate_y = 0;

         /* As the result of min(A, 1x1) or replace(A, 1x1) are always 1x1, set the vertex rate
          * combiner mode as passthrough.
          */
         pipeline_comb_mode = V_028848_VRS_COMB_MODE_PASSTHRU;
         break;
      case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR:
         /* The result of max(A, 1x1) is always A. */
         FALLTHROUGH;
      case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR:
         /* Nothing to do here because the SAMPLE_ITER combiner mode should already be passthrough. */
         break;
      default:
         break;
      }
   }

   /* Emit per-draw VRS rate which is the first combiner. */
   radeon_set_uconfig_reg(cmd_buffer->cs, R_03098C_GE_VRS_RATE,
                          S_03098C_RATE_X(rate_x) | S_03098C_RATE_Y(rate_y));

   /* VERTEX_RATE_COMBINER_MODE controls the combiner mode between the
    * draw rate and the vertex rate.
    */
   if (cmd_buffer->state.mesh_shading) {
      pa_cl_vrs_cntl |= S_028848_VERTEX_RATE_COMBINER_MODE(V_028848_VRS_COMB_MODE_PASSTHRU) |
                        S_028848_PRIMITIVE_RATE_COMBINER_MODE(pipeline_comb_mode);
   } else {
      pa_cl_vrs_cntl |= S_028848_VERTEX_RATE_COMBINER_MODE(pipeline_comb_mode) |
                        S_028848_PRIMITIVE_RATE_COMBINER_MODE(V_028848_VRS_COMB_MODE_PASSTHRU);
   }

   /* HTILE_RATE_COMBINER_MODE controls the combiner mode between the primitive rate and the HTILE
    * rate.
    */
   pa_cl_vrs_cntl |= S_028848_HTILE_RATE_COMBINER_MODE(htile_comb_mode);

   radeon_set_context_reg(cmd_buffer->cs, R_028848_PA_CL_VRS_CNTL, pa_cl_vrs_cntl);
}

static void
radv_emit_primitive_restart_enable(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX11) {
      radeon_set_uconfig_reg(cmd_buffer->cs, R_03092C_GE_MULTI_PRIM_IB_RESET_EN,
                             d->vk.ia.primitive_restart_enable);
   } else if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX9) {
      radeon_set_uconfig_reg(cmd_buffer->cs, R_03092C_VGT_MULTI_PRIM_IB_RESET_EN,
                             d->vk.ia.primitive_restart_enable);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028A94_VGT_MULTI_PRIM_IB_RESET_EN,
                             d->vk.ia.primitive_restart_enable);
   }
}

static void
radv_emit_clipping(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   radeon_set_context_reg(cmd_buffer->cs, R_028810_PA_CL_CLIP_CNTL,
                          S_028810_DX_RASTERIZATION_KILL(d->vk.rs.rasterizer_discard_enable) |
                             S_028810_ZCLIP_NEAR_DISABLE(!d->vk.rs.depth_clip_enable) |
                             S_028810_ZCLIP_FAR_DISABLE(!d->vk.rs.depth_clip_enable) |
                             S_028810_DX_CLIP_SPACE_DEF(!d->vk.vp.depth_clip_negative_one_to_one) |
                             S_028810_DX_LINEAR_ATTR_CLIP_ENA(1));
}

static bool
radv_is_mrt0_dual_src(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   if (!d->vk.cb.attachments[0].write_mask || !d->vk.cb.attachments[0].blend_enable)
      return false;

   const struct vk_color_blend_attachment_state att = {
      .color_blend_op = d->vk.cb.attachments[0].color_blend_op,
      .src_color_blend_factor = d->vk.cb.attachments[0].src_color_blend_factor,
      .dst_color_blend_factor = d->vk.cb.attachments[0].dst_color_blend_factor,
      .alpha_blend_op = d->vk.cb.attachments[0].alpha_blend_op,
      .src_alpha_blend_factor = d->vk.cb.attachments[0].src_alpha_blend_factor,
      .dst_alpha_blend_factor = d->vk.cb.attachments[0].dst_alpha_blend_factor,
   };

   return radv_can_enable_dual_src(&att);
}

static void
radv_emit_logic_op(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned cb_color_control = 0;

   if (d->vk.cb.logic_op_enable) {
      cb_color_control |= S_028808_ROP3(d->vk.cb.logic_op);
   } else {
      cb_color_control |= S_028808_ROP3(V_028808_ROP3_COPY);
   }

   if (cmd_buffer->device->physical_device->rad_info.has_rbplus) {
      /* RB+ doesn't work with dual source blending, logic op and CB_RESOLVE. */
      bool mrt0_is_dual_src = radv_is_mrt0_dual_src(cmd_buffer);

      cb_color_control |=
         S_028808_DISABLE_DUAL_QUAD(mrt0_is_dual_src || d->vk.cb.logic_op_enable ||
                                    pipeline->custom_blend_mode == V_028808_CB_RESOLVE);
   }

   if (pipeline->custom_blend_mode) {
      cb_color_control |= S_028808_MODE(pipeline->custom_blend_mode);
   } else {
      bool color_write_enabled = false;

      for (unsigned i = 0; i < MAX_RTS; i++) {
         if (d->vk.cb.attachments[i].write_mask) {
            color_write_enabled = true;
            break;
         }
      }

      if (color_write_enabled) {
         cb_color_control |= S_028808_MODE(V_028808_CB_NORMAL);
      } else {
         cb_color_control |= S_028808_MODE(V_028808_CB_DISABLE);
      }
   }

   radeon_set_context_reg(cmd_buffer->cs, R_028808_CB_COLOR_CONTROL, cb_color_control);
}

static void
radv_emit_color_write(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = cmd_buffer->device;
   const struct radv_binning_settings *settings = &device->physical_device->binning_settings;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   uint32_t color_write_enable = 0, color_write_mask = 0;

   u_foreach_bit(i, d->vk.cb.color_write_enables) {
      color_write_enable |= 0xfu << (i * 4);
   }

   for (unsigned i = 0; i < MAX_RTS; i++) {
      color_write_mask |= d->vk.cb.attachments[i].write_mask << (4 * i);
   }

   if (device->pbb_allowed && settings->context_states_per_bin > 1) {
      /* Flush DFSM on CB_TARGET_MASK changes. */
      radeon_emit(cmd_buffer->cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
      radeon_emit(cmd_buffer->cs, EVENT_TYPE(V_028A90_BREAK_BATCH) | EVENT_INDEX(0));
   }

   radeon_set_context_reg(cmd_buffer->cs, R_028238_CB_TARGET_MASK,
                          color_write_mask & color_write_enable);
}

static void
radv_emit_patch_control_points(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_physical_device *pdevice = cmd_buffer->device->physical_device;
   const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const struct radv_shader *tcs = pipeline->base.shaders[MESA_SHADER_TESS_CTRL];
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned ls_hs_config, base_reg;
   struct radv_userdata_info *loc;

   ls_hs_config = S_028B58_NUM_PATCHES(cmd_buffer->state.tess_num_patches) |
                  S_028B58_HS_NUM_INPUT_CP(d->vk.ts.patch_control_points) |
                  S_028B58_HS_NUM_OUTPUT_CP(tcs->info.tcs.tcs_vertices_out);

   if (pdevice->rad_info.gfx_level >= GFX7) {
      radeon_set_context_reg_idx(cmd_buffer->cs, R_028B58_VGT_LS_HS_CONFIG, 2, ls_hs_config);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028B58_VGT_LS_HS_CONFIG, ls_hs_config);
   }

   if (pdevice->rad_info.gfx_level >= GFX9) {
      unsigned hs_rsrc2 = tcs->config.rsrc2;

      if (pdevice->rad_info.gfx_level >= GFX10) {
         hs_rsrc2 |= S_00B42C_LDS_SIZE_GFX10(cmd_buffer->state.tess_lds_size);
      } else {
         hs_rsrc2 |= S_00B42C_LDS_SIZE_GFX9(cmd_buffer->state.tess_lds_size);
      }

      radeon_set_sh_reg(cmd_buffer->cs, R_00B42C_SPI_SHADER_PGM_RSRC2_HS, hs_rsrc2);
   } else {
      struct radv_shader *vs = pipeline->base.shaders[MESA_SHADER_VERTEX];
      unsigned ls_rsrc2 = vs->config.rsrc2 | S_00B52C_LDS_SIZE(cmd_buffer->state.tess_lds_size);

      radeon_set_sh_reg(cmd_buffer->cs, R_00B52C_SPI_SHADER_PGM_RSRC2_LS, ls_rsrc2);
   }

   /* Emit user SGPRs for dynamic patch control points. */
   loc = radv_lookup_user_sgpr(&pipeline->base, MESA_SHADER_TESS_CTRL, AC_UD_TCS_OFFCHIP_LAYOUT);
   if (loc->sgpr_idx == -1)
      return;
   assert(loc->num_sgprs == 1);

   base_reg = pipeline->base.user_data_0[MESA_SHADER_TESS_CTRL];
   radeon_set_sh_reg(cmd_buffer->cs, base_reg + loc->sgpr_idx * 4,
                     (cmd_buffer->state.tess_num_patches << 6) | d->vk.ts.patch_control_points);

   loc = radv_lookup_user_sgpr(&pipeline->base, MESA_SHADER_TESS_EVAL, AC_UD_TES_NUM_PATCHES);
   assert(loc->sgpr_idx != -1 && loc->num_sgprs == 1);

   base_reg = pipeline->base.user_data_0[MESA_SHADER_TESS_EVAL];
   radeon_set_sh_reg(cmd_buffer->cs, base_reg + loc->sgpr_idx * 4,
                     cmd_buffer->state.tess_num_patches);
}

static void
radv_emit_conservative_rast_mode(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_physical_device *pdevice = cmd_buffer->device->physical_device;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   if (pdevice->rad_info.gfx_level >= GFX9) {
      uint32_t pa_sc_conservative_rast = S_028C4C_NULL_SQUAD_AA_MASK_ENABLE(1);

      if (d->vk.rs.conservative_mode != VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT) {
         pa_sc_conservative_rast = S_028C4C_PREZ_AA_MASK_ENABLE(1) | S_028C4C_POSTZ_AA_MASK_ENABLE(1) |
                                   S_028C4C_CENTROID_SAMPLE_OVERRIDE(1);

         if (d->vk.rs.conservative_mode == VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT) {
            pa_sc_conservative_rast |=
               S_028C4C_OVER_RAST_ENABLE(1) | S_028C4C_OVER_RAST_SAMPLE_SELECT(0) |
               S_028C4C_UNDER_RAST_ENABLE(0) | S_028C4C_UNDER_RAST_SAMPLE_SELECT(1) |
               S_028C4C_PBB_UNCERTAINTY_REGION_ENABLE(1);
         } else {
            assert(d->vk.rs.conservative_mode ==
                   VK_CONSERVATIVE_RASTERIZATION_MODE_UNDERESTIMATE_EXT);
            pa_sc_conservative_rast |=
               S_028C4C_OVER_RAST_ENABLE(0) | S_028C4C_OVER_RAST_SAMPLE_SELECT(1) |
               S_028C4C_UNDER_RAST_ENABLE(1) | S_028C4C_UNDER_RAST_SAMPLE_SELECT(0) |
               S_028C4C_PBB_UNCERTAINTY_REGION_ENABLE(0);
         }
      }

      radeon_set_context_reg(cmd_buffer->cs, R_028C4C_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL,
                             pa_sc_conservative_rast);
   }
}

static void
radv_emit_depth_clamp_enable(struct radv_cmd_buffer *cmd_buffer)
{
   enum radv_depth_clamp_mode mode = radv_get_depth_clamp_mode(cmd_buffer);

   radeon_set_context_reg(cmd_buffer->cs, R_02800C_DB_RENDER_OVERRIDE,
                          S_02800C_FORCE_HIS_ENABLE0(V_02800C_FORCE_DISABLE) |
                          S_02800C_FORCE_HIS_ENABLE1(V_02800C_FORCE_DISABLE) |
                          S_02800C_DISABLE_VIEWPORT_CLAMP(mode == RADV_DEPTH_CLAMP_MODE_DISABLED));
}

static void
radv_emit_rasterization_samples(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const struct radv_physical_device *pdevice = pipeline->base.device->physical_device;
   unsigned rasterization_samples = radv_get_rasterization_samples(cmd_buffer);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   unsigned pa_sc_mode_cntl_1 = pipeline->pa_sc_mode_cntl_1;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned spi_baryc_cntl = S_0286E0_FRONT_FACE_ALL_BITS(1);
   unsigned db_render_control = pipeline->db_render_control;

   if (!d->sample_location.count)
      radv_emit_default_sample_locations(cmd_buffer->cs, rasterization_samples);

   if (rasterization_samples > 1) {
      unsigned ps_iter_samples = radv_get_ps_iter_samples(cmd_buffer);

      if (ps_iter_samples > 1) {
         spi_baryc_cntl |= S_0286E0_POS_FLOAT_LOCATION(2);
         pa_sc_mode_cntl_1 |= S_028A4C_PS_ITER_SAMPLE(1);
      }
   }

   if (pdevice->rad_info.gfx_level >= GFX11) {
      unsigned num_samples = render->max_samples;
      unsigned max_allowed_tiles_in_wave = 0;

      if (pdevice->rad_info.has_dedicated_vram) {
         if (num_samples == 8)
            max_allowed_tiles_in_wave = 7;
         else if (num_samples == 4)
            max_allowed_tiles_in_wave = 14;
      } else {
         if (num_samples == 8)
            max_allowed_tiles_in_wave = 8;
      }

      /* TODO: We may want to disable this workaround for future chips. */
      if (num_samples >= 4) {
         if (max_allowed_tiles_in_wave)
            max_allowed_tiles_in_wave--;
         else
            max_allowed_tiles_in_wave = 15;
      }

      db_render_control |= S_028000_OREO_MODE(V_028000_OMODE_O_THEN_B) |
                           S_028000_MAX_ALLOWED_TILES_IN_WAVE(max_allowed_tiles_in_wave);
   }

   radeon_set_context_reg(cmd_buffer->cs, R_028000_DB_RENDER_CONTROL, db_render_control);
   radeon_set_context_reg(cmd_buffer->cs, R_0286E0_SPI_BARYC_CNTL, spi_baryc_cntl);
   radeon_set_context_reg(cmd_buffer->cs, R_028A4C_PA_SC_MODE_CNTL_1, pa_sc_mode_cntl_1);

   /* Pass the number of samples to the fragment shader because it might be needed. */
   struct radv_userdata_info *loc =
      radv_lookup_user_sgpr(&pipeline->base, MESA_SHADER_FRAGMENT, AC_UD_PS_NUM_SAMPLES);
   if (loc->sgpr_idx != -1) {
      uint32_t base_reg = pipeline->base.user_data_0[MESA_SHADER_FRAGMENT];
      radeon_set_sh_reg(cmd_buffer->cs, base_reg + loc->sgpr_idx * 4, rasterization_samples);
   }
}

static void
radv_emit_fb_color_state(struct radv_cmd_buffer *cmd_buffer, int index,
                         struct radv_color_buffer_info *cb, struct radv_image_view *iview,
                         VkImageLayout layout)
{
   bool is_vi = cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX8;
   uint32_t cb_fdcc_control = cb->cb_dcc_control;
   uint32_t cb_color_info = cb->cb_color_info;
   struct radv_image *image = iview->image;

   if (!radv_layout_dcc_compressed(
          cmd_buffer->device, image, iview->vk.base_mip_level, layout,
          radv_image_queue_family_mask(image, cmd_buffer->qf,
                                       cmd_buffer->qf))) {
      if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX11) {
         cb_fdcc_control &= C_028C78_FDCC_ENABLE;
      } else {
         cb_color_info &= C_028C70_DCC_ENABLE;
      }
   }

   if (!radv_layout_fmask_compressed(
          cmd_buffer->device, image, layout,
          radv_image_queue_family_mask(image, cmd_buffer->qf,
                                       cmd_buffer->qf))) {
      cb_color_info &= C_028C70_COMPRESSION;
   }

   if (radv_image_is_tc_compat_cmask(image) && (radv_is_fmask_decompress_pipeline(cmd_buffer) ||
                                                radv_is_dcc_decompress_pipeline(cmd_buffer))) {
      /* If this bit is set, the FMASK decompression operation
       * doesn't occur (DCC_COMPRESS also implies FMASK_DECOMPRESS).
       */
      cb_color_info &= C_028C70_FMASK_COMPRESS_1FRAG_ONLY;
   }

   if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX11) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028C6C_CB_COLOR0_VIEW + index * 0x3c, 4);
      radeon_emit(cmd_buffer->cs, cb->cb_color_view);                      /* CB_COLOR0_VIEW */
      radeon_emit(cmd_buffer->cs, cb->cb_color_info);                      /* CB_COLOR0_INFO */
      radeon_emit(cmd_buffer->cs, cb->cb_color_attrib);                    /* CB_COLOR0_ATTRIB */
      radeon_emit(cmd_buffer->cs, cb_fdcc_control);                        /* CB_COLOR0_FDCC_CONTROL */

      radeon_set_context_reg(cmd_buffer->cs, R_028C60_CB_COLOR0_BASE + index * 0x3c, cb->cb_color_base);
      radeon_set_context_reg(cmd_buffer->cs, R_028E40_CB_COLOR0_BASE_EXT + index * 4, cb->cb_color_base >> 32);
      radeon_set_context_reg(cmd_buffer->cs, R_028C94_CB_COLOR0_DCC_BASE + index * 0x3c, cb->cb_dcc_base);
      radeon_set_context_reg(cmd_buffer->cs, R_028EA0_CB_COLOR0_DCC_BASE_EXT + index * 4, cb->cb_dcc_base >> 32);
      radeon_set_context_reg(cmd_buffer->cs, R_028EC0_CB_COLOR0_ATTRIB2 + index * 4, cb->cb_color_attrib2);
      radeon_set_context_reg(cmd_buffer->cs, R_028EE0_CB_COLOR0_ATTRIB3 + index * 4, cb->cb_color_attrib3);
   } else if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX10) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028C60_CB_COLOR0_BASE + index * 0x3c, 11);
      radeon_emit(cmd_buffer->cs, cb->cb_color_base);
      radeon_emit(cmd_buffer->cs, 0);
      radeon_emit(cmd_buffer->cs, 0);
      radeon_emit(cmd_buffer->cs, cb->cb_color_view);
      radeon_emit(cmd_buffer->cs, cb_color_info);
      radeon_emit(cmd_buffer->cs, cb->cb_color_attrib);
      radeon_emit(cmd_buffer->cs, cb->cb_dcc_control);
      radeon_emit(cmd_buffer->cs, cb->cb_color_cmask);
      radeon_emit(cmd_buffer->cs, 0);
      radeon_emit(cmd_buffer->cs, cb->cb_color_fmask);
      radeon_emit(cmd_buffer->cs, 0);

      radeon_set_context_reg(cmd_buffer->cs, R_028C94_CB_COLOR0_DCC_BASE + index * 0x3c, cb->cb_dcc_base);

      radeon_set_context_reg(cmd_buffer->cs, R_028E40_CB_COLOR0_BASE_EXT + index * 4,
                             cb->cb_color_base >> 32);
      radeon_set_context_reg(cmd_buffer->cs, R_028E60_CB_COLOR0_CMASK_BASE_EXT + index * 4,
                             cb->cb_color_cmask >> 32);
      radeon_set_context_reg(cmd_buffer->cs, R_028E80_CB_COLOR0_FMASK_BASE_EXT + index * 4,
                             cb->cb_color_fmask >> 32);
      radeon_set_context_reg(cmd_buffer->cs, R_028EA0_CB_COLOR0_DCC_BASE_EXT + index * 4,
                             cb->cb_dcc_base >> 32);
      radeon_set_context_reg(cmd_buffer->cs, R_028EC0_CB_COLOR0_ATTRIB2 + index * 4,
                             cb->cb_color_attrib2);
      radeon_set_context_reg(cmd_buffer->cs, R_028EE0_CB_COLOR0_ATTRIB3 + index * 4,
                             cb->cb_color_attrib3);
   } else if (cmd_buffer->device->physical_device->rad_info.gfx_level == GFX9) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028C60_CB_COLOR0_BASE + index * 0x3c, 11);
      radeon_emit(cmd_buffer->cs, cb->cb_color_base);
      radeon_emit(cmd_buffer->cs, S_028C64_BASE_256B(cb->cb_color_base >> 32));
      radeon_emit(cmd_buffer->cs, cb->cb_color_attrib2);
      radeon_emit(cmd_buffer->cs, cb->cb_color_view);
      radeon_emit(cmd_buffer->cs, cb_color_info);
      radeon_emit(cmd_buffer->cs, cb->cb_color_attrib);
      radeon_emit(cmd_buffer->cs, cb->cb_dcc_control);
      radeon_emit(cmd_buffer->cs, cb->cb_color_cmask);
      radeon_emit(cmd_buffer->cs, S_028C80_BASE_256B(cb->cb_color_cmask >> 32));
      radeon_emit(cmd_buffer->cs, cb->cb_color_fmask);
      radeon_emit(cmd_buffer->cs, S_028C88_BASE_256B(cb->cb_color_fmask >> 32));

      radeon_set_context_reg_seq(cmd_buffer->cs, R_028C94_CB_COLOR0_DCC_BASE + index * 0x3c, 2);
      radeon_emit(cmd_buffer->cs, cb->cb_dcc_base);
      radeon_emit(cmd_buffer->cs, S_028C98_BASE_256B(cb->cb_dcc_base >> 32));

      radeon_set_context_reg(cmd_buffer->cs, R_0287A0_CB_MRT0_EPITCH + index * 4,
                             cb->cb_mrt_epitch);
   } else {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028C60_CB_COLOR0_BASE + index * 0x3c, 11);
      radeon_emit(cmd_buffer->cs, cb->cb_color_base);
      radeon_emit(cmd_buffer->cs, cb->cb_color_pitch);
      radeon_emit(cmd_buffer->cs, cb->cb_color_slice);
      radeon_emit(cmd_buffer->cs, cb->cb_color_view);
      radeon_emit(cmd_buffer->cs, cb_color_info);
      radeon_emit(cmd_buffer->cs, cb->cb_color_attrib);
      radeon_emit(cmd_buffer->cs, cb->cb_dcc_control);
      radeon_emit(cmd_buffer->cs, cb->cb_color_cmask);
      radeon_emit(cmd_buffer->cs, cb->cb_color_cmask_slice);
      radeon_emit(cmd_buffer->cs, cb->cb_color_fmask);
      radeon_emit(cmd_buffer->cs, cb->cb_color_fmask_slice);

      if (is_vi) { /* DCC BASE */
         radeon_set_context_reg(cmd_buffer->cs, R_028C94_CB_COLOR0_DCC_BASE + index * 0x3c,
                                cb->cb_dcc_base);
      }
   }

   if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX11
          ? G_028C78_FDCC_ENABLE(cb_fdcc_control)
          : G_028C70_DCC_ENABLE(cb_color_info)) {
      /* Drawing with DCC enabled also compresses colorbuffers. */
      VkImageSubresourceRange range = {
         .aspectMask = iview->vk.aspects,
         .baseMipLevel = iview->vk.base_mip_level,
         .levelCount = iview->vk.level_count,
         .baseArrayLayer = iview->vk.base_array_layer,
         .layerCount = iview->vk.layer_count,
      };

      radv_update_dcc_metadata(cmd_buffer, image, &range, true);
   }
}

static void
radv_update_zrange_precision(struct radv_cmd_buffer *cmd_buffer, struct radv_ds_buffer_info *ds,
                             const struct radv_image_view *iview, VkImageLayout layout,
                             bool requires_cond_exec)
{
   const struct radv_image *image = iview->image;
   uint32_t db_z_info = ds->db_z_info;
   uint32_t db_z_info_reg;

   if (!cmd_buffer->device->physical_device->rad_info.has_tc_compat_zrange_bug ||
       !radv_image_is_tc_compat_htile(image))
      return;

   if (!radv_layout_is_htile_compressed(
          cmd_buffer->device, image, layout,
          radv_image_queue_family_mask(image, cmd_buffer->qf,
                                       cmd_buffer->qf))) {
      db_z_info &= C_028040_TILE_SURFACE_ENABLE;
   }

   db_z_info &= C_028040_ZRANGE_PRECISION;

   if (cmd_buffer->device->physical_device->rad_info.gfx_level == GFX9) {
      db_z_info_reg = R_028038_DB_Z_INFO;
   } else {
      db_z_info_reg = R_028040_DB_Z_INFO;
   }

   /* When we don't know the last fast clear value we need to emit a
    * conditional packet that will eventually skip the following
    * SET_CONTEXT_REG packet.
    */
   if (requires_cond_exec) {
      uint64_t va = radv_get_tc_compat_zrange_va(image, iview->vk.base_mip_level);

      radeon_emit(cmd_buffer->cs, PKT3(PKT3_COND_EXEC, 3, 0));
      radeon_emit(cmd_buffer->cs, va);
      radeon_emit(cmd_buffer->cs, va >> 32);
      radeon_emit(cmd_buffer->cs, 0);
      radeon_emit(cmd_buffer->cs, 3); /* SET_CONTEXT_REG size */
   }

   radeon_set_context_reg(cmd_buffer->cs, db_z_info_reg, db_z_info);
}

static void
radv_emit_fb_ds_state(struct radv_cmd_buffer *cmd_buffer, struct radv_ds_buffer_info *ds,
                      struct radv_image_view *iview, VkImageLayout layout)
{
   const struct radv_image *image = iview->image;
   uint32_t db_z_info = ds->db_z_info;
   uint32_t db_stencil_info = ds->db_stencil_info;
   uint32_t db_htile_surface = ds->db_htile_surface;

   if (!radv_layout_is_htile_compressed(
          cmd_buffer->device, image, layout,
          radv_image_queue_family_mask(image, cmd_buffer->qf,
                                       cmd_buffer->qf))) {
      db_z_info &= C_028040_TILE_SURFACE_ENABLE;
      db_stencil_info |= S_028044_TILE_STENCIL_DISABLE(1);
   }

   if (cmd_buffer->device->physical_device->rad_info.gfx_level == GFX10_3 &&
       !cmd_buffer->state.render.vrs_att.iview) {
      db_htile_surface &= C_028ABC_VRS_HTILE_ENCODING;
   }

   radeon_set_context_reg(cmd_buffer->cs, R_028008_DB_DEPTH_VIEW, ds->db_depth_view);
   radeon_set_context_reg(cmd_buffer->cs, R_028010_DB_RENDER_OVERRIDE2, ds->db_render_override2);
   radeon_set_context_reg(cmd_buffer->cs, R_028ABC_DB_HTILE_SURFACE, db_htile_surface);

   if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX10) {
      radeon_set_context_reg(cmd_buffer->cs, R_028014_DB_HTILE_DATA_BASE, ds->db_htile_data_base);
      radeon_set_context_reg(cmd_buffer->cs, R_02801C_DB_DEPTH_SIZE_XY, ds->db_depth_size);

      if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX11) {
         radeon_set_context_reg_seq(cmd_buffer->cs, R_028040_DB_Z_INFO, 6);
      } else {
         radeon_set_context_reg_seq(cmd_buffer->cs, R_02803C_DB_DEPTH_INFO, 7);
         radeon_emit(cmd_buffer->cs, S_02803C_RESOURCE_LEVEL(1));
      }
      radeon_emit(cmd_buffer->cs, db_z_info);
      radeon_emit(cmd_buffer->cs, db_stencil_info);
      radeon_emit(cmd_buffer->cs, ds->db_z_read_base);
      radeon_emit(cmd_buffer->cs, ds->db_stencil_read_base);
      radeon_emit(cmd_buffer->cs, ds->db_z_read_base);
      radeon_emit(cmd_buffer->cs, ds->db_stencil_read_base);

      radeon_set_context_reg_seq(cmd_buffer->cs, R_028068_DB_Z_READ_BASE_HI, 5);
      radeon_emit(cmd_buffer->cs, ds->db_z_read_base >> 32);
      radeon_emit(cmd_buffer->cs, ds->db_stencil_read_base >> 32);
      radeon_emit(cmd_buffer->cs, ds->db_z_read_base >> 32);
      radeon_emit(cmd_buffer->cs, ds->db_stencil_read_base >> 32);
      radeon_emit(cmd_buffer->cs, ds->db_htile_data_base >> 32);
   } else if (cmd_buffer->device->physical_device->rad_info.gfx_level == GFX9) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028014_DB_HTILE_DATA_BASE, 3);
      radeon_emit(cmd_buffer->cs, ds->db_htile_data_base);
      radeon_emit(cmd_buffer->cs, S_028018_BASE_HI(ds->db_htile_data_base >> 32));
      radeon_emit(cmd_buffer->cs, ds->db_depth_size);

      radeon_set_context_reg_seq(cmd_buffer->cs, R_028038_DB_Z_INFO, 10);
      radeon_emit(cmd_buffer->cs, db_z_info);          /* DB_Z_INFO */
      radeon_emit(cmd_buffer->cs, db_stencil_info);    /* DB_STENCIL_INFO */
      radeon_emit(cmd_buffer->cs, ds->db_z_read_base); /* DB_Z_READ_BASE */
      radeon_emit(cmd_buffer->cs,
                  S_028044_BASE_HI(ds->db_z_read_base >> 32)); /* DB_Z_READ_BASE_HI */
      radeon_emit(cmd_buffer->cs, ds->db_stencil_read_base);   /* DB_STENCIL_READ_BASE */
      radeon_emit(cmd_buffer->cs,
                  S_02804C_BASE_HI(ds->db_stencil_read_base >> 32)); /* DB_STENCIL_READ_BASE_HI */
      radeon_emit(cmd_buffer->cs, ds->db_z_write_base);              /* DB_Z_WRITE_BASE */
      radeon_emit(cmd_buffer->cs,
                  S_028054_BASE_HI(ds->db_z_write_base >> 32)); /* DB_Z_WRITE_BASE_HI */
      radeon_emit(cmd_buffer->cs, ds->db_stencil_write_base);   /* DB_STENCIL_WRITE_BASE */
      radeon_emit(cmd_buffer->cs,
                  S_02805C_BASE_HI(ds->db_stencil_write_base >> 32)); /* DB_STENCIL_WRITE_BASE_HI */

      radeon_set_context_reg_seq(cmd_buffer->cs, R_028068_DB_Z_INFO2, 2);
      radeon_emit(cmd_buffer->cs, ds->db_z_info2);
      radeon_emit(cmd_buffer->cs, ds->db_stencil_info2);
   } else {
      radeon_set_context_reg(cmd_buffer->cs, R_028014_DB_HTILE_DATA_BASE, ds->db_htile_data_base);

      radeon_set_context_reg_seq(cmd_buffer->cs, R_02803C_DB_DEPTH_INFO, 9);
      radeon_emit(cmd_buffer->cs, ds->db_depth_info);         /* R_02803C_DB_DEPTH_INFO */
      radeon_emit(cmd_buffer->cs, db_z_info);                 /* R_028040_DB_Z_INFO */
      radeon_emit(cmd_buffer->cs, db_stencil_info);           /* R_028044_DB_STENCIL_INFO */
      radeon_emit(cmd_buffer->cs, ds->db_z_read_base);        /* R_028048_DB_Z_READ_BASE */
      radeon_emit(cmd_buffer->cs, ds->db_stencil_read_base);  /* R_02804C_DB_STENCIL_READ_BASE */
      radeon_emit(cmd_buffer->cs, ds->db_z_write_base);       /* R_028050_DB_Z_WRITE_BASE */
      radeon_emit(cmd_buffer->cs, ds->db_stencil_write_base); /* R_028054_DB_STENCIL_WRITE_BASE */
      radeon_emit(cmd_buffer->cs, ds->db_depth_size);         /* R_028058_DB_DEPTH_SIZE */
      radeon_emit(cmd_buffer->cs, ds->db_depth_slice);        /* R_02805C_DB_DEPTH_SLICE */
   }

   /* Update the ZRANGE_PRECISION value for the TC-compat bug. */
   radv_update_zrange_precision(cmd_buffer, ds, iview, layout, true);

   radeon_set_context_reg(cmd_buffer->cs, R_028B78_PA_SU_POLY_OFFSET_DB_FMT_CNTL,
                          ds->pa_su_poly_offset_db_fmt_cntl);
}

/**
 * Update the fast clear depth/stencil values if the image is bound as a
 * depth/stencil buffer.
 */
static void
radv_update_bound_fast_clear_ds(struct radv_cmd_buffer *cmd_buffer,
                                const struct radv_image_view *iview,
                                VkClearDepthStencilValue ds_clear_value, VkImageAspectFlags aspects)
{
   const struct radv_image *image = iview->image;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   if (cmd_buffer->state.render.ds_att.iview == NULL ||
       cmd_buffer->state.render.ds_att.iview->image != image)
      return;

   if (aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      radeon_set_context_reg_seq(cs, R_028028_DB_STENCIL_CLEAR, 2);
      radeon_emit(cs, ds_clear_value.stencil);
      radeon_emit(cs, fui(ds_clear_value.depth));
   } else if (aspects == VK_IMAGE_ASPECT_DEPTH_BIT) {
      radeon_set_context_reg(cs, R_02802C_DB_DEPTH_CLEAR, fui(ds_clear_value.depth));
   } else {
      assert(aspects == VK_IMAGE_ASPECT_STENCIL_BIT);
      radeon_set_context_reg(cs, R_028028_DB_STENCIL_CLEAR, ds_clear_value.stencil);
   }

   /* Update the ZRANGE_PRECISION value for the TC-compat bug. This is
    * only needed when clearing Z to 0.0.
    */
   if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) && ds_clear_value.depth == 0.0) {
      radv_update_zrange_precision(cmd_buffer, &cmd_buffer->state.render.ds_att.ds, iview,
                                   cmd_buffer->state.render.ds_att.layout, false);
   }

   cmd_buffer->state.context_roll_without_scissor_emitted = true;
}

/**
 * Set the clear depth/stencil values to the image's metadata.
 */
static void
radv_set_ds_clear_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                           const VkImageSubresourceRange *range,
                           VkClearDepthStencilValue ds_clear_value, VkImageAspectFlags aspects)
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint32_t level_count = radv_get_levelCount(image, range);

   if (aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      uint64_t va = radv_get_ds_clear_value_va(image, range->baseMipLevel);

      /* Use the fastest way when both aspects are used. */
      radeon_emit(cs, PKT3(PKT3_WRITE_DATA, 2 + 2 * level_count, cmd_buffer->state.predicating));
      radeon_emit(cs, S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(V_370_PFP));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);

      for (uint32_t l = 0; l < level_count; l++) {
         radeon_emit(cs, ds_clear_value.stencil);
         radeon_emit(cs, fui(ds_clear_value.depth));
      }
   } else {
      /* Otherwise we need one WRITE_DATA packet per level. */
      for (uint32_t l = 0; l < level_count; l++) {
         uint64_t va = radv_get_ds_clear_value_va(image, range->baseMipLevel + l);
         unsigned value;

         if (aspects == VK_IMAGE_ASPECT_DEPTH_BIT) {
            value = fui(ds_clear_value.depth);
            va += 4;
         } else {
            assert(aspects == VK_IMAGE_ASPECT_STENCIL_BIT);
            value = ds_clear_value.stencil;
         }

         radeon_emit(cs, PKT3(PKT3_WRITE_DATA, 3, cmd_buffer->state.predicating));
         radeon_emit(cs,
                     S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(V_370_PFP));
         radeon_emit(cs, va);
         radeon_emit(cs, va >> 32);
         radeon_emit(cs, value);
      }
   }
}

/**
 * Update the TC-compat metadata value for this image.
 */
static void
radv_set_tc_compat_zrange_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                   const VkImageSubresourceRange *range, uint32_t value)
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   if (!cmd_buffer->device->physical_device->rad_info.has_tc_compat_zrange_bug)
      return;

   uint64_t va = radv_get_tc_compat_zrange_va(image, range->baseMipLevel);
   uint32_t level_count = radv_get_levelCount(image, range);

   radeon_emit(cs, PKT3(PKT3_WRITE_DATA, 2 + level_count, cmd_buffer->state.predicating));
   radeon_emit(cs, S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(V_370_PFP));
   radeon_emit(cs, va);
   radeon_emit(cs, va >> 32);

   for (uint32_t l = 0; l < level_count; l++)
      radeon_emit(cs, value);
}

static void
radv_update_tc_compat_zrange_metadata(struct radv_cmd_buffer *cmd_buffer,
                                      const struct radv_image_view *iview,
                                      VkClearDepthStencilValue ds_clear_value)
{
   VkImageSubresourceRange range = {
      .aspectMask = iview->vk.aspects,
      .baseMipLevel = iview->vk.base_mip_level,
      .levelCount = iview->vk.level_count,
      .baseArrayLayer = iview->vk.base_array_layer,
      .layerCount = iview->vk.layer_count,
   };
   uint32_t cond_val;

   /* Conditionally set DB_Z_INFO.ZRANGE_PRECISION to 0 when the last
    * depth clear value is 0.0f.
    */
   cond_val = ds_clear_value.depth == 0.0f ? UINT_MAX : 0;

   radv_set_tc_compat_zrange_metadata(cmd_buffer, iview->image, &range, cond_val);
}

/**
 * Update the clear depth/stencil values for this image.
 */
void
radv_update_ds_clear_metadata(struct radv_cmd_buffer *cmd_buffer,
                              const struct radv_image_view *iview,
                              VkClearDepthStencilValue ds_clear_value, VkImageAspectFlags aspects)
{
   VkImageSubresourceRange range = {
      .aspectMask = iview->vk.aspects,
      .baseMipLevel = iview->vk.base_mip_level,
      .levelCount = iview->vk.level_count,
      .baseArrayLayer = iview->vk.base_array_layer,
      .layerCount = iview->vk.layer_count,
   };
   struct radv_image *image = iview->image;

   assert(radv_htile_enabled(image, range.baseMipLevel));

   radv_set_ds_clear_metadata(cmd_buffer, iview->image, &range, ds_clear_value, aspects);

   if (radv_image_is_tc_compat_htile(image) && (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) {
      radv_update_tc_compat_zrange_metadata(cmd_buffer, iview, ds_clear_value);
   }

   radv_update_bound_fast_clear_ds(cmd_buffer, iview, ds_clear_value, aspects);
}

/**
 * Load the clear depth/stencil values from the image's metadata.
 */
static void
radv_load_ds_clear_metadata(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview)
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const struct radv_image *image = iview->image;
   VkImageAspectFlags aspects = vk_format_aspects(image->vk.format);
   uint64_t va = radv_get_ds_clear_value_va(image, iview->vk.base_mip_level);
   unsigned reg_offset = 0, reg_count = 0;

   assert(radv_image_has_htile(image));

   if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      ++reg_count;
   } else {
      ++reg_offset;
      va += 4;
   }
   if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
      ++reg_count;

   uint32_t reg = R_028028_DB_STENCIL_CLEAR + 4 * reg_offset;

   if (cmd_buffer->device->physical_device->rad_info.has_load_ctx_reg_pkt) {
      radeon_emit(cs, PKT3(PKT3_LOAD_CONTEXT_REG_INDEX, 3, 0));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, (reg - SI_CONTEXT_REG_OFFSET) >> 2);
      radeon_emit(cs, reg_count);
   } else {
      radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
      radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_REG) |
                         (reg_count == 2 ? COPY_DATA_COUNT_SEL : 0));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, reg >> 2);
      radeon_emit(cs, 0);

      radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
      radeon_emit(cs, 0);
   }
}

/*
 * With DCC some colors don't require CMASK elimination before being
 * used as a texture. This sets a predicate value to determine if the
 * cmask eliminate is required.
 */
void
radv_update_fce_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                         const VkImageSubresourceRange *range, bool value)
{
   if (!image->fce_pred_offset)
      return;

   uint64_t pred_val = value;
   uint64_t va = radv_image_get_fce_pred_va(image, range->baseMipLevel);
   uint32_t level_count = radv_get_levelCount(image, range);
   uint32_t count = 2 * level_count;

   radeon_emit(cmd_buffer->cs, PKT3(PKT3_WRITE_DATA, 2 + count, 0));
   radeon_emit(cmd_buffer->cs,
               S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(V_370_PFP));
   radeon_emit(cmd_buffer->cs, va);
   radeon_emit(cmd_buffer->cs, va >> 32);

   for (uint32_t l = 0; l < level_count; l++) {
      radeon_emit(cmd_buffer->cs, pred_val);
      radeon_emit(cmd_buffer->cs, pred_val >> 32);
   }
}

/**
 * Update the DCC predicate to reflect the compression state.
 */
void
radv_update_dcc_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                         const VkImageSubresourceRange *range, bool value)
{
   if (image->dcc_pred_offset == 0)
      return;

   uint64_t pred_val = value;
   uint64_t va = radv_image_get_dcc_pred_va(image, range->baseMipLevel);
   uint32_t level_count = radv_get_levelCount(image, range);
   uint32_t count = 2 * level_count;

   assert(radv_dcc_enabled(image, range->baseMipLevel));

   radeon_emit(cmd_buffer->cs, PKT3(PKT3_WRITE_DATA, 2 + count, 0));
   radeon_emit(cmd_buffer->cs,
               S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(V_370_PFP));
   radeon_emit(cmd_buffer->cs, va);
   radeon_emit(cmd_buffer->cs, va >> 32);

   for (uint32_t l = 0; l < level_count; l++) {
      radeon_emit(cmd_buffer->cs, pred_val);
      radeon_emit(cmd_buffer->cs, pred_val >> 32);
   }
}

/**
 * Update the fast clear color values if the image is bound as a color buffer.
 */
static void
radv_update_bound_fast_clear_color(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                   int cb_idx, uint32_t color_values[2])
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   if (cb_idx >= cmd_buffer->state.render.color_att_count ||
       cmd_buffer->state.render.color_att[cb_idx].iview == NULL ||
       cmd_buffer->state.render.color_att[cb_idx].iview->image != image)
      return;

   radeon_set_context_reg_seq(cs, R_028C8C_CB_COLOR0_CLEAR_WORD0 + cb_idx * 0x3c, 2);
   radeon_emit(cs, color_values[0]);
   radeon_emit(cs, color_values[1]);

   cmd_buffer->state.context_roll_without_scissor_emitted = true;
}

/**
 * Set the clear color values to the image's metadata.
 */
static void
radv_set_color_clear_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                              const VkImageSubresourceRange *range, uint32_t color_values[2])
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint32_t level_count = radv_get_levelCount(image, range);
   uint32_t count = 2 * level_count;

   assert(radv_image_has_cmask(image) || radv_dcc_enabled(image, range->baseMipLevel));

   if (radv_image_has_clear_value(image)) {
      uint64_t va = radv_image_get_fast_clear_va(image, range->baseMipLevel);

      radeon_emit(cs, PKT3(PKT3_WRITE_DATA, 2 + count, cmd_buffer->state.predicating));
      radeon_emit(cs, S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(V_370_PFP));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);

      for (uint32_t l = 0; l < level_count; l++) {
         radeon_emit(cs, color_values[0]);
         radeon_emit(cs, color_values[1]);
      }
   } else {
      /* Some default value we can set in the update. */
      assert(color_values[0] == 0 && color_values[1] == 0);
   }
}

/**
 * Update the clear color values for this image.
 */
void
radv_update_color_clear_metadata(struct radv_cmd_buffer *cmd_buffer,
                                 const struct radv_image_view *iview, int cb_idx,
                                 uint32_t color_values[2])
{
   struct radv_image *image = iview->image;
   VkImageSubresourceRange range = {
      .aspectMask = iview->vk.aspects,
      .baseMipLevel = iview->vk.base_mip_level,
      .levelCount = iview->vk.level_count,
      .baseArrayLayer = iview->vk.base_array_layer,
      .layerCount = iview->vk.layer_count,
   };

   assert(radv_image_has_cmask(image) || radv_dcc_enabled(image, iview->vk.base_mip_level));

   /* Do not need to update the clear value for images that are fast cleared with the comp-to-single
    * mode because the hardware gets the value from the image directly.
    */
   if (iview->image->support_comp_to_single)
      return;

   radv_set_color_clear_metadata(cmd_buffer, image, &range, color_values);

   radv_update_bound_fast_clear_color(cmd_buffer, image, cb_idx, color_values);
}

/**
 * Load the clear color values from the image's metadata.
 */
static void
radv_load_color_clear_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image_view *iview,
                               int cb_idx)
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   struct radv_image *image = iview->image;

   if (!radv_image_has_cmask(image) && !radv_dcc_enabled(image, iview->vk.base_mip_level))
      return;

   if (iview->image->support_comp_to_single)
      return;

   if (!radv_image_has_clear_value(image)) {
      uint32_t color_values[2] = {0, 0};
      radv_update_bound_fast_clear_color(cmd_buffer, image, cb_idx, color_values);
      return;
   }

   uint64_t va = radv_image_get_fast_clear_va(image, iview->vk.base_mip_level);
   uint32_t reg = R_028C8C_CB_COLOR0_CLEAR_WORD0 + cb_idx * 0x3c;

   if (cmd_buffer->device->physical_device->rad_info.has_load_ctx_reg_pkt) {
      radeon_emit(cs, PKT3(PKT3_LOAD_CONTEXT_REG_INDEX, 3, cmd_buffer->state.predicating));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, (reg - SI_CONTEXT_REG_OFFSET) >> 2);
      radeon_emit(cs, 2);
   } else {
      radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, cmd_buffer->state.predicating));
      radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_REG) |
                         COPY_DATA_COUNT_SEL);
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, reg >> 2);
      radeon_emit(cs, 0);

      radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, cmd_buffer->state.predicating));
      radeon_emit(cs, 0);
   }
}

/* GFX9+ metadata cache flushing workaround. metadata cache coherency is
 * broken if the CB caches data of multiple mips of the same image at the
 * same time.
 *
 * Insert some flushes to avoid this.
 */
static void
radv_emit_fb_mip_change_flush(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_rendering_state *render = &cmd_buffer->state.render;
   bool color_mip_changed = false;

   /* Entire workaround is not applicable before GFX9 */
   if (cmd_buffer->device->physical_device->rad_info.gfx_level < GFX9)
      return;

   for (int i = 0; i < render->color_att_count; ++i) {
      struct radv_image_view *iview = render->color_att[i].iview;
      if (!iview)
         continue;

      if ((radv_image_has_CB_metadata(iview->image) ||
           radv_dcc_enabled(iview->image, iview->vk.base_mip_level) ||
           radv_dcc_enabled(iview->image, cmd_buffer->state.cb_mip[i])) &&
          cmd_buffer->state.cb_mip[i] != iview->vk.base_mip_level)
         color_mip_changed = true;

      cmd_buffer->state.cb_mip[i] = iview->vk.base_mip_level;
   }

   if (color_mip_changed) {
      cmd_buffer->state.flush_bits |=
         RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
   }
}

/* This function does the flushes for mip changes if the levels are not zero for
 * all render targets. This way we can assume at the start of the next cmd_buffer
 * that rendering to mip 0 doesn't need any flushes. As that is the most common
 * case that saves some flushes. */
static void
radv_emit_mip_change_flush_default(struct radv_cmd_buffer *cmd_buffer)
{
   /* Entire workaround is not applicable before GFX9 */
   if (cmd_buffer->device->physical_device->rad_info.gfx_level < GFX9)
      return;

   bool need_color_mip_flush = false;
   for (unsigned i = 0; i < 8; ++i) {
      if (cmd_buffer->state.cb_mip[i]) {
         need_color_mip_flush = true;
         break;
      }
   }

   if (need_color_mip_flush) {
      cmd_buffer->state.flush_bits |=
         RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
   }

   memset(cmd_buffer->state.cb_mip, 0, sizeof(cmd_buffer->state.cb_mip));
}

static struct radv_image *
radv_cmd_buffer_get_vrs_image(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = cmd_buffer->device;

   if (!device->vrs.image) {
      VkResult result;

      /* The global VRS state is initialized on-demand to avoid wasting VRAM. */
      result = radv_device_init_vrs_state(device);
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd_buffer->vk, result);
         return NULL;
      }
   }

   return device->vrs.image;
}

static void
radv_emit_framebuffer_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_rendering_state *render = &cmd_buffer->state.render;
   int i;
   bool disable_constant_encode_ac01 = false;
   unsigned color_invalid = cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX11
                            ? S_028C70_FORMAT_GFX11(V_028C70_COLOR_INVALID)
                            : S_028C70_FORMAT_GFX6(V_028C70_COLOR_INVALID);

   for (i = 0; i < render->color_att_count; ++i) {
      struct radv_image_view *iview = render->color_att[i].iview;
      if (!iview) {
         radeon_set_context_reg(cmd_buffer->cs, R_028C70_CB_COLOR0_INFO + i * 0x3C, color_invalid);
         continue;
      }

      VkImageLayout layout = render->color_att[i].layout;

      radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, iview->image->bindings[0].bo);

      assert(iview->vk.aspects & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_PLANE_0_BIT |
                                   VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT));

      if (iview->image->disjoint && iview->vk.aspects == VK_IMAGE_ASPECT_COLOR_BIT) {
         for (uint32_t plane_id = 0; plane_id < iview->image->plane_count; plane_id++) {
            radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs,
                  iview->image->bindings[plane_id].bo);
         }
      } else {
         uint32_t plane_id = iview->image->disjoint ? iview->plane_id : 0;
         radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs,
               iview->image->bindings[plane_id].bo);
      }

      radv_emit_fb_color_state(cmd_buffer, i, &render->color_att[i].cb, iview, layout);

      radv_load_color_clear_metadata(cmd_buffer, iview, i);

      if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX9 &&
          iview->image->dcc_sign_reinterpret) {
         /* Disable constant encoding with the clear value of "1" with different DCC signedness
          * because the hardware will fill "1" instead of the clear value.
          */
         disable_constant_encode_ac01 = true;
      }
   }
   for (; i < cmd_buffer->state.last_subpass_color_count; i++) {
      radeon_set_context_reg(cmd_buffer->cs, R_028C70_CB_COLOR0_INFO + i * 0x3C, color_invalid);
   }
   cmd_buffer->state.last_subpass_color_count = render->color_att_count;

   if (render->ds_att.iview) {
      VkImageLayout layout = render->ds_att.layout;
      struct radv_image_view *iview = render->ds_att.iview;
      radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, iview->image->bindings[0].bo);

      radv_emit_fb_ds_state(cmd_buffer, &render->ds_att.ds, iview, layout);

      if (radv_layout_is_htile_compressed(
             cmd_buffer->device, iview->image, layout,
             radv_image_queue_family_mask(iview->image, cmd_buffer->qf,
                                          cmd_buffer->qf))) {
         /* Only load the depth/stencil fast clear values when
          * compressed rendering is enabled.
          */
         radv_load_ds_clear_metadata(cmd_buffer, iview);
      }
   } else if (cmd_buffer->device->physical_device->rad_info.gfx_level == GFX10_3 &&
              render->vrs_att.iview && radv_cmd_buffer_get_vrs_image(cmd_buffer)) {
      /* When a subpass uses a VRS attachment without binding a depth/stencil attachment, we have to
       * bind our internal depth buffer that contains the VRS data as part of HTILE.
       */
      VkImageLayout layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      struct radv_buffer *htile_buffer = cmd_buffer->device->vrs.buffer;
      struct radv_image *image = cmd_buffer->device->vrs.image;
      struct radv_ds_buffer_info ds;
      struct radv_image_view iview;

      radv_image_view_init(&iview, cmd_buffer->device,
                           &(VkImageViewCreateInfo){
                              .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                              .image = radv_image_to_handle(image),
                              .viewType = radv_meta_get_view_type(image),
                              .format = image->vk.format,
                              .subresourceRange =
                                 {
                                    .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                    .baseMipLevel = 0,
                                    .levelCount = 1,
                                    .baseArrayLayer = 0,
                                    .layerCount = 1,
                                 },
                           },
                           0, NULL);

      radv_initialise_vrs_surface(image, htile_buffer, &ds);

      radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, htile_buffer->bo);

      radv_emit_fb_ds_state(cmd_buffer, &ds, &iview, layout);

      radv_image_view_finish(&iview);
   } else {
      unsigned num_samples = 0;

      /* On GFX11, DB_Z_INFO.NUM_SAMPLES should always match the framebuffer samples. It affects
       * VRS and occlusion queries if depth and stencil are not bound.
       */
      if (cmd_buffer->device->physical_device->rad_info.gfx_level == GFX11)
         num_samples = util_logbase2(render->max_samples);

      if (cmd_buffer->device->physical_device->rad_info.gfx_level == GFX9)
         radeon_set_context_reg_seq(cmd_buffer->cs, R_028038_DB_Z_INFO, 2);
      else
         radeon_set_context_reg_seq(cmd_buffer->cs, R_028040_DB_Z_INFO, 2);

      radeon_emit(cmd_buffer->cs, S_028040_FORMAT(V_028040_Z_INVALID) |       /* DB_Z_INFO */
                                  S_028040_NUM_SAMPLES(num_samples));
      radeon_emit(cmd_buffer->cs, S_028044_FORMAT(V_028044_STENCIL_INVALID)); /* DB_STENCIL_INFO */
   }

   if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX8) {
      bool disable_constant_encode =
         cmd_buffer->device->physical_device->rad_info.has_dcc_constant_encode;
      enum amd_gfx_level gfx_level = cmd_buffer->device->physical_device->rad_info.gfx_level;

      if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX11) {
         radeon_set_context_reg(cmd_buffer->cs, R_028424_CB_FDCC_CONTROL,
                                S_028424_SAMPLE_MASK_TRACKER_WATERMARK(0));
      } else {
        uint8_t watermark = gfx_level >= GFX10 ? 6 : 4;

         radeon_set_context_reg(cmd_buffer->cs, R_028424_CB_DCC_CONTROL,
                                S_028424_OVERWRITE_COMBINER_MRT_SHARING_DISABLE(gfx_level <= GFX9) |
                                S_028424_OVERWRITE_COMBINER_WATERMARK(watermark) |
                                S_028424_DISABLE_CONSTANT_ENCODE_AC01(disable_constant_encode_ac01) |
                                S_028424_DISABLE_CONSTANT_ENCODE_REG(disable_constant_encode));
      }
   }

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_FRAMEBUFFER;
}

static void
radv_emit_guardband_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned rast_prim;

   if (!(pipeline->dynamic_states & RADV_DYNAMIC_PRIMITIVE_TOPOLOGY) ||
       (pipeline->active_stages & (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                   VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
                                   VK_SHADER_STAGE_GEOMETRY_BIT |
                                   VK_SHADER_STAGE_MESH_BIT_EXT))) {
      /* Ignore dynamic primitive topology for TES/GS/MS stages. */
      rast_prim = pipeline->rast_prim;
   } else {
      rast_prim = si_conv_prim_to_gs_out(d->vk.ia.primitive_topology);
   }

   si_write_guardband(cmd_buffer->cs, d->vk.vp.viewport_count, d->vk.vp.viewports, rast_prim,
                      d->vk.rs.polygon_mode, d->vk.rs.line.width);

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_GUARDBAND;
}

static void
radv_emit_index_buffer(struct radv_cmd_buffer *cmd_buffer, bool indirect)
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   struct radv_cmd_state *state = &cmd_buffer->state;

   /* With indirect generated commands the index buffer bind may be part of the
    * indirect command buffer, in which case the app may not have bound any yet. */
   if (state->index_type < 0)
      return;

   /* For the direct indexed draws we use DRAW_INDEX_2, which includes
    * the index_va and max_index_count already. */
   if (!indirect)
      return;

   if (state->max_index_count ||
       !cmd_buffer->device->physical_device->rad_info.has_zero_index_buffer_bug) {
      radeon_emit(cs, PKT3(PKT3_INDEX_BASE, 1, 0));
      radeon_emit(cs, state->index_va);
      radeon_emit(cs, state->index_va >> 32);

      radeon_emit(cs, PKT3(PKT3_INDEX_BUFFER_SIZE, 0, 0));
      radeon_emit(cs, state->max_index_count);
   }

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_INDEX_BUFFER;
}

void
radv_set_db_count_control(struct radv_cmd_buffer *cmd_buffer, bool enable_occlusion_queries)
{
   uint32_t db_count_control;

   if (!enable_occlusion_queries) {
      db_count_control =
         S_028004_ZPASS_INCREMENT_DISABLE(cmd_buffer->device->physical_device->rad_info.gfx_level < GFX11);
   } else {
      uint32_t sample_rate = util_logbase2(cmd_buffer->state.render.max_samples);
      bool gfx10_perfect =
         cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX10 &&
         cmd_buffer->state.perfect_occlusion_queries_enabled;

      if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX7) {
         /* Always enable PERFECT_ZPASS_COUNTS due to issues with partially
          * covered tiles, discards, and early depth testing. For more details,
          * see https://gitlab.freedesktop.org/mesa/mesa/-/issues/3218 */
         db_count_control = S_028004_PERFECT_ZPASS_COUNTS(1) |
                            S_028004_DISABLE_CONSERVATIVE_ZPASS_COUNTS(gfx10_perfect) |
                            S_028004_SAMPLE_RATE(sample_rate) | S_028004_ZPASS_ENABLE(1) |
                            S_028004_SLICE_EVEN_ENABLE(1) | S_028004_SLICE_ODD_ENABLE(1);
      } else {
         db_count_control = S_028004_PERFECT_ZPASS_COUNTS(1) | S_028004_SAMPLE_RATE(sample_rate);
      }
   }

   radeon_set_context_reg(cmd_buffer->cs, R_028004_DB_COUNT_CONTROL, db_count_control);

   cmd_buffer->state.context_roll_without_scissor_emitted = true;
}

unsigned
radv_instance_rate_prolog_index(unsigned num_attributes, uint32_t instance_rate_inputs)
{
   /* instance_rate_vs_prologs is a flattened array of array of arrays of different sizes, or a
    * single array sorted in ascending order using:
    * - total number of attributes
    * - number of instanced attributes
    * - index of first instanced attribute
    */

   /* From total number of attributes to offset. */
   static const uint16_t total_to_offset[16] = {0,   1,   4,   10,  20,  35,  56,  84,
                                                120, 165, 220, 286, 364, 455, 560, 680};
   unsigned start_index = total_to_offset[num_attributes - 1];

   /* From number of instanced attributes to offset. This would require a different LUT depending on
    * the total number of attributes, but we can exploit a pattern to use just the LUT for 16 total
    * attributes.
    */
   static const uint8_t count_to_offset_total16[16] = {0,   16,  31,  45,  58,  70,  81,  91,
                                                       100, 108, 115, 121, 126, 130, 133, 135};
   unsigned count = util_bitcount(instance_rate_inputs);
   unsigned offset_from_start_index =
      count_to_offset_total16[count - 1] - ((16 - num_attributes) * (count - 1));

   unsigned first = ffs(instance_rate_inputs) - 1;
   return start_index + offset_from_start_index + first;
}

union vs_prolog_key_header {
   struct {
      uint32_t key_size : 8;
      uint32_t num_attributes : 6;
      uint32_t as_ls : 1;
      uint32_t is_ngg : 1;
      uint32_t wave32 : 1;
      uint32_t next_stage : 3;
      uint32_t instance_rate_inputs : 1;
      uint32_t alpha_adjust_lo : 1;
      uint32_t alpha_adjust_hi : 1;
      uint32_t misaligned_mask : 1;
      uint32_t post_shuffle : 1;
      uint32_t nontrivial_divisors : 1;
      uint32_t zero_divisors : 1;
      /* We need this to ensure the padding is zero. It's useful even if it's unused. */
      uint32_t padding0 : 5;
   };
   uint32_t v;
};

uint32_t
radv_hash_vs_prolog(const void *key_)
{
   const uint32_t *key = key_;
   union vs_prolog_key_header header;
   header.v = key[0];
   return _mesa_hash_data(key, header.key_size);
}

bool
radv_cmp_vs_prolog(const void *a_, const void *b_)
{
   const uint32_t *a = a_;
   const uint32_t *b = b_;
   if (a[0] != b[0])
      return false;

   union vs_prolog_key_header header;
   header.v = a[0];
   return memcmp(a, b, header.key_size) == 0;
}

static struct radv_shader_part *
lookup_vs_prolog(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs_shader,
                 uint32_t *nontrivial_divisors)
{
   STATIC_ASSERT(sizeof(union vs_prolog_key_header) == 4);
   assert(vs_shader->info.vs.dynamic_inputs);

   const struct radv_vs_input_state *state = &cmd_buffer->state.dynamic_vs_input;
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radv_device *device = cmd_buffer->device;

   unsigned num_attributes = pipeline->last_vertex_attrib_bit;
   uint32_t attribute_mask = BITFIELD_MASK(num_attributes);

   uint32_t instance_rate_inputs = state->instance_rate_inputs & attribute_mask;
   uint32_t zero_divisors = state->zero_divisors & attribute_mask;
   *nontrivial_divisors = state->nontrivial_divisors & attribute_mask;
   uint32_t misaligned_mask = cmd_buffer->state.vbo_misaligned_mask;
   if (cmd_buffer->state.vbo_misaligned_mask_invalid) {
      assert(device->physical_device->rad_info.gfx_level == GFX6 ||
             device->physical_device->rad_info.gfx_level >= GFX10);

      u_foreach_bit (index, cmd_buffer->state.vbo_misaligned_mask_invalid & attribute_mask) {
         uint8_t binding = state->bindings[index];
         if (!(cmd_buffer->state.vbo_bound_mask & BITFIELD_BIT(binding)))
            continue;
         uint8_t req = state->format_align_req_minus_1[index];
         struct radv_vertex_binding *vb = &cmd_buffer->vertex_bindings[binding];
         VkDeviceSize offset = vb->offset + state->offsets[index];
         if ((offset & req) || (vb->stride & req))
            misaligned_mask |= BITFIELD_BIT(index);
      }
      cmd_buffer->state.vbo_misaligned_mask = misaligned_mask;
      cmd_buffer->state.vbo_misaligned_mask_invalid &= ~attribute_mask;
   }
   misaligned_mask |= state->nontrivial_formats;

   /* try to use a pre-compiled prolog first */
   struct radv_shader_part *prolog = NULL;
   if (pipeline->can_use_simple_input &&
       (!vs_shader->info.vs.as_ls || !instance_rate_inputs) &&
       !misaligned_mask && !state->alpha_adjust_lo && !state->alpha_adjust_hi) {
      if (!instance_rate_inputs) {
         prolog = device->simple_vs_prologs[num_attributes - 1];
      } else if (num_attributes <= 16 && !*nontrivial_divisors && !zero_divisors &&
                 util_bitcount(instance_rate_inputs) ==
                    (util_last_bit(instance_rate_inputs) - ffs(instance_rate_inputs) + 1)) {
         unsigned index = radv_instance_rate_prolog_index(num_attributes, instance_rate_inputs);
         prolog = device->instance_rate_vs_prologs[index];
      }
   }
   if (prolog)
      return prolog;

   /* if we couldn't use a pre-compiled prolog, find one in the cache or create one */
   uint32_t key_words[17];
   unsigned key_size = 1;

   struct radv_vs_prolog_key key;
   key.state = state;
   key.num_attributes = num_attributes;
   key.misaligned_mask = misaligned_mask;
   /* The instance ID input VGPR is placed differently when as_ls=true. */
   key.as_ls = vs_shader->info.vs.as_ls && instance_rate_inputs;
   key.is_ngg = vs_shader->info.is_ngg;
   key.wave32 = vs_shader->info.wave_size == 32;
   key.next_stage = pipeline->next_vertex_stage;

   union vs_prolog_key_header header;
   header.v = 0;
   header.num_attributes = num_attributes;
   header.as_ls = key.as_ls;
   header.is_ngg = key.is_ngg;
   header.wave32 = key.wave32;
   header.next_stage = key.next_stage;

   if (instance_rate_inputs & ~*nontrivial_divisors) {
      header.instance_rate_inputs = true;
      key_words[key_size++] = instance_rate_inputs;
   }
   if (*nontrivial_divisors) {
      header.nontrivial_divisors = true;
      key_words[key_size++] = *nontrivial_divisors;
   }
   if (zero_divisors) {
      header.zero_divisors = true;
      key_words[key_size++] = zero_divisors;
   }
   if (misaligned_mask) {
      header.misaligned_mask = true;
      key_words[key_size++] = misaligned_mask;

      uint8_t *formats = (uint8_t *)&key_words[key_size];
      unsigned num_formats = 0;
      u_foreach_bit(index, misaligned_mask) formats[num_formats++] = state->formats[index];
      while (num_formats & 0x3)
         formats[num_formats++] = 0;
      key_size += num_formats / 4u;

      if (state->post_shuffle & attribute_mask) {
         header.post_shuffle = true;
         key_words[key_size++] = state->post_shuffle & attribute_mask;
      }
   }
   if (state->alpha_adjust_lo & attribute_mask) {
      header.alpha_adjust_lo = true;
      key_words[key_size++] = state->alpha_adjust_lo & attribute_mask;
   }
   if (state->alpha_adjust_hi & attribute_mask) {
      header.alpha_adjust_hi = true;
      key_words[key_size++] = state->alpha_adjust_hi & attribute_mask;
   }

   header.key_size = key_size * sizeof(key_words[0]);
   key_words[0] = header.v;

   uint32_t hash = radv_hash_vs_prolog(key_words);

   if (cmd_buffer->state.emitted_vs_prolog &&
       cmd_buffer->state.emitted_vs_prolog_key_hash == hash &&
       radv_cmp_vs_prolog(key_words, cmd_buffer->state.emitted_vs_prolog_key))
      return cmd_buffer->state.emitted_vs_prolog;

   u_rwlock_rdlock(&device->vs_prologs_lock);
   struct hash_entry *prolog_entry =
      _mesa_hash_table_search_pre_hashed(device->vs_prologs, hash, key_words);
   u_rwlock_rdunlock(&device->vs_prologs_lock);

   if (!prolog_entry) {
      u_rwlock_wrlock(&device->vs_prologs_lock);
      prolog_entry = _mesa_hash_table_search_pre_hashed(device->vs_prologs, hash, key_words);
      if (prolog_entry) {
         u_rwlock_wrunlock(&device->vs_prologs_lock);
         return prolog_entry->data;
      }

      prolog = radv_create_vs_prolog(device, &key);
      uint32_t *key2 = malloc(key_size * 4);
      if (!prolog || !key2) {
         radv_shader_part_unref(device, prolog);
         free(key2);
         u_rwlock_wrunlock(&device->vs_prologs_lock);
         return NULL;
      }
      memcpy(key2, key_words, key_size * 4);
      _mesa_hash_table_insert_pre_hashed(device->vs_prologs, hash, key2, prolog);

      u_rwlock_wrunlock(&device->vs_prologs_lock);
      return prolog;
   }

   return prolog_entry->data;
}

static void
emit_prolog_regs(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs_shader,
                 const struct radv_shader_part *prolog, bool pipeline_is_dirty)
{
   /* no need to re-emit anything in this case */
   if (cmd_buffer->state.emitted_vs_prolog == prolog && !pipeline_is_dirty)
      return;

   enum amd_gfx_level chip = cmd_buffer->device->physical_device->rad_info.gfx_level;
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;

   assert(cmd_buffer->state.emitted_graphics_pipeline == cmd_buffer->state.graphics_pipeline);

   uint32_t rsrc1 = vs_shader->config.rsrc1;
   if (chip < GFX10 && G_00B228_SGPRS(prolog->rsrc1) > G_00B228_SGPRS(vs_shader->config.rsrc1))
      rsrc1 = (rsrc1 & C_00B228_SGPRS) | (prolog->rsrc1 & ~C_00B228_SGPRS);

   /* The main shader must not use less VGPRs than the prolog, otherwise shared vgprs might not
    * work.
    */
   assert(G_00B848_VGPRS(vs_shader->config.rsrc1) >= G_00B848_VGPRS(prolog->rsrc1));

   unsigned pgm_lo_reg = R_00B120_SPI_SHADER_PGM_LO_VS;
   unsigned rsrc1_reg = R_00B128_SPI_SHADER_PGM_RSRC1_VS;
   if (vs_shader->info.is_ngg || pipeline->base.shaders[MESA_SHADER_GEOMETRY] == vs_shader) {
      pgm_lo_reg = chip >= GFX10 ? R_00B320_SPI_SHADER_PGM_LO_ES : R_00B210_SPI_SHADER_PGM_LO_ES;
      rsrc1_reg = R_00B228_SPI_SHADER_PGM_RSRC1_GS;
   } else if (pipeline->base.shaders[MESA_SHADER_TESS_CTRL] == vs_shader) {
      pgm_lo_reg = chip >= GFX10 ? R_00B520_SPI_SHADER_PGM_LO_LS : R_00B410_SPI_SHADER_PGM_LO_LS;
      rsrc1_reg = R_00B428_SPI_SHADER_PGM_RSRC1_HS;
   } else if (vs_shader->info.vs.as_ls) {
      pgm_lo_reg = R_00B520_SPI_SHADER_PGM_LO_LS;
      rsrc1_reg = R_00B528_SPI_SHADER_PGM_RSRC1_LS;
   } else if (vs_shader->info.vs.as_es) {
      pgm_lo_reg = R_00B320_SPI_SHADER_PGM_LO_ES;
      rsrc1_reg = R_00B328_SPI_SHADER_PGM_RSRC1_ES;
   }

   radeon_set_sh_reg(cmd_buffer->cs, pgm_lo_reg, prolog->va >> 8);

   if (chip < GFX10)
      radeon_set_sh_reg(cmd_buffer->cs, rsrc1_reg, rsrc1);
   else
      assert(rsrc1 == vs_shader->config.rsrc1);

   radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, prolog->bo);
}

static void
emit_prolog_inputs(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs_shader,
                   uint32_t nontrivial_divisors, bool pipeline_is_dirty)
{
   /* no need to re-emit anything in this case */
   if (!nontrivial_divisors && !pipeline_is_dirty && cmd_buffer->state.emitted_vs_prolog &&
       !cmd_buffer->state.emitted_vs_prolog->nontrivial_divisors)
      return;

   const struct radv_vs_input_state *state = &cmd_buffer->state.dynamic_vs_input;
   uint64_t input_va = radv_shader_get_va(vs_shader);

   if (nontrivial_divisors) {
      unsigned inputs_offset;
      uint32_t *inputs;
      unsigned size = 8 + util_bitcount(nontrivial_divisors) * 8;
      if (!radv_cmd_buffer_upload_alloc(cmd_buffer, size, &inputs_offset, (void **)&inputs))
         return;

      *(inputs++) = input_va;
      *(inputs++) = input_va >> 32;

      u_foreach_bit(index, nontrivial_divisors)
      {
         uint32_t div = state->divisors[index];
         if (div == 0) {
            *(inputs++) = 0;
            *(inputs++) = 1;
         } else if (util_is_power_of_two_or_zero(div)) {
            *(inputs++) = util_logbase2(div) | (1 << 8);
            *(inputs++) = 0xffffffffu;
         } else {
            struct util_fast_udiv_info info = util_compute_fast_udiv_info(div, 32, 32);
            *(inputs++) = info.pre_shift | (info.increment << 8) | (info.post_shift << 16);
            *(inputs++) = info.multiplier;
         }
      }

      input_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + inputs_offset;
   }

   const struct radv_userdata_info *loc =
      &vs_shader->info.user_sgprs_locs.shader_data[AC_UD_VS_PROLOG_INPUTS];
   uint32_t base_reg = cmd_buffer->state.graphics_pipeline->base.user_data_0[MESA_SHADER_VERTEX];
   assert(loc->sgpr_idx != -1);
   assert(loc->num_sgprs == 2);
   radv_emit_shader_pointer(cmd_buffer->device, cmd_buffer->cs, base_reg + loc->sgpr_idx * 4,
                            input_va, true);
}

static void
radv_emit_vertex_input(struct radv_cmd_buffer *cmd_buffer, bool pipeline_is_dirty)
{
   const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const struct radv_shader *vs_shader = radv_get_shader(&pipeline->base, MESA_SHADER_VERTEX);

   assert(!cmd_buffer->state.mesh_shading);

   if (!vs_shader->info.vs.has_prolog)
      return;

   uint32_t nontrivial_divisors;
   struct radv_shader_part *prolog =
      lookup_vs_prolog(cmd_buffer, vs_shader, &nontrivial_divisors);
   if (!prolog) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   emit_prolog_regs(cmd_buffer, vs_shader, prolog, pipeline_is_dirty);
   emit_prolog_inputs(cmd_buffer, vs_shader, nontrivial_divisors, pipeline_is_dirty);

   cmd_buffer->state.emitted_vs_prolog = prolog;

   if (unlikely(cmd_buffer->device->trace_bo))
      radv_save_vs_prolog(cmd_buffer, prolog);
}

static void
radv_emit_tess_domain_origin(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_physical_device *pdevice = cmd_buffer->device->physical_device;
   const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const struct radv_shader *tes = radv_get_shader(&pipeline->base, MESA_SHADER_TESS_EVAL);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned type = 0, partitioning = 0, distribution_mode = 0;
   unsigned topology;

   switch (tes->info.tes._primitive_mode) {
   case TESS_PRIMITIVE_TRIANGLES:
      type = V_028B6C_TESS_TRIANGLE;
      break;
   case TESS_PRIMITIVE_QUADS:
      type = V_028B6C_TESS_QUAD;
      break;
   case TESS_PRIMITIVE_ISOLINES:
      type = V_028B6C_TESS_ISOLINE;
      break;
   default:
      unreachable("Invalid tess primitive type");
   }

   switch (tes->info.tes.spacing) {
   case TESS_SPACING_EQUAL:
      partitioning = V_028B6C_PART_INTEGER;
      break;
   case TESS_SPACING_FRACTIONAL_ODD:
      partitioning = V_028B6C_PART_FRAC_ODD;
      break;
   case TESS_SPACING_FRACTIONAL_EVEN:
      partitioning = V_028B6C_PART_FRAC_EVEN;
      break;
   default:
      unreachable("Invalid tess spacing type");
   }

   if (pdevice->rad_info.has_distributed_tess) {
      if (pdevice->rad_info.family == CHIP_FIJI || pdevice->rad_info.family >= CHIP_POLARIS10)
         distribution_mode = V_028B6C_TRAPEZOIDS;
      else
         distribution_mode = V_028B6C_DONUTS;
   } else {
      distribution_mode = V_028B6C_NO_DIST;
   }

   if (tes->info.tes.point_mode) {
      topology = V_028B6C_OUTPUT_POINT;
   } else if (tes->info.tes._primitive_mode == TESS_PRIMITIVE_ISOLINES) {
      topology = V_028B6C_OUTPUT_LINE;
   } else {
      bool ccw = tes->info.tes.ccw;

      if (d->vk.ts.domain_origin != VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT) {
         ccw = !ccw;
      }

      topology = ccw ? V_028B6C_OUTPUT_TRIANGLE_CCW : V_028B6C_OUTPUT_TRIANGLE_CW;
   }

   radeon_set_context_reg(cmd_buffer->cs, R_028B6C_VGT_TF_PARAM,
                          S_028B6C_TYPE(type) | S_028B6C_PARTITIONING(partitioning) |
                             S_028B6C_TOPOLOGY(topology) |
                             S_028B6C_DISTRIBUTION_MODE(distribution_mode));
}

static void
radv_emit_alpha_to_coverage_enable(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned db_alpha_to_mask = 0;

   if (cmd_buffer->device->instance->debug_flags & RADV_DEBUG_NO_ATOC_DITHERING) {
      db_alpha_to_mask = S_028B70_ALPHA_TO_MASK_OFFSET0(2) | S_028B70_ALPHA_TO_MASK_OFFSET1(2) |
                         S_028B70_ALPHA_TO_MASK_OFFSET2(2) | S_028B70_ALPHA_TO_MASK_OFFSET3(2) |
                         S_028B70_OFFSET_ROUND(0);
   } else {
      db_alpha_to_mask = S_028B70_ALPHA_TO_MASK_OFFSET0(3) | S_028B70_ALPHA_TO_MASK_OFFSET1(1) |
                         S_028B70_ALPHA_TO_MASK_OFFSET2(0) | S_028B70_ALPHA_TO_MASK_OFFSET3(2) |
                         S_028B70_OFFSET_ROUND(1);
   }

   db_alpha_to_mask |= S_028B70_ALPHA_TO_MASK_ENABLE(d->vk.ms.alpha_to_coverage_enable);

   radeon_set_context_reg(cmd_buffer->cs, R_028B70_DB_ALPHA_TO_MASK, db_alpha_to_mask);
}

static void
radv_emit_sample_mask(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   radeon_set_context_reg_seq(cmd_buffer->cs, R_028C38_PA_SC_AA_MASK_X0Y0_X1Y0, 2);
   radeon_emit(cmd_buffer->cs, d->vk.ms.sample_mask | ((uint32_t)d->vk.ms.sample_mask << 16));
   radeon_emit(cmd_buffer->cs, d->vk.ms.sample_mask | ((uint32_t)d->vk.ms.sample_mask << 16));
}

static void
radv_emit_color_blend(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_physical_device *pdevice = cmd_buffer->device->physical_device;
   const enum amd_gfx_level gfx_level = pdevice->rad_info.gfx_level;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned cb_blend_control[MAX_RTS], sx_mrt_blend_opt[MAX_RTS];
   bool mrt0_is_dual_src = radv_is_mrt0_dual_src(cmd_buffer);

   for (unsigned i = 0; i < MAX_RTS; i++) {
      VkBlendOp eqRGB = d->vk.cb.attachments[i].color_blend_op;
      VkBlendFactor srcRGB = d->vk.cb.attachments[i].src_color_blend_factor;
      VkBlendFactor dstRGB = d->vk.cb.attachments[i].dst_color_blend_factor;
      VkBlendOp eqA = d->vk.cb.attachments[i].alpha_blend_op;
      VkBlendFactor srcA = d->vk.cb.attachments[i].src_alpha_blend_factor;
      VkBlendFactor dstA = d->vk.cb.attachments[i].dst_alpha_blend_factor;
      unsigned srcRGB_opt, dstRGB_opt, srcA_opt, dstA_opt;
      unsigned blend_cntl = 0;

      cb_blend_control[i] = sx_mrt_blend_opt[i] = 0;

      /* Ignore other blend targets if dual-source blending is enabled to prevent wrong behaviour.
       */
      if (i > 0 && mrt0_is_dual_src)
         continue;

      if (!d->vk.cb.attachments[i].blend_enable) {
         sx_mrt_blend_opt[i] |= S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED) |
                                S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED);
         continue;
      }

      radv_normalize_blend_factor(eqRGB, &srcRGB, &dstRGB);
      radv_normalize_blend_factor(eqA, &srcA, &dstA);

      /* Blending optimizations for RB+.
       * These transformations don't change the behavior.
       *
       * First, get rid of DST in the blend factors:
       *    func(src * DST, dst * 0) ---> func(src * 0, dst * SRC)
       */
      si_blend_remove_dst(&eqRGB, &srcRGB, &dstRGB, VK_BLEND_FACTOR_DST_COLOR,
                          VK_BLEND_FACTOR_SRC_COLOR);

      si_blend_remove_dst(&eqA, &srcA, &dstA, VK_BLEND_FACTOR_DST_COLOR,
                          VK_BLEND_FACTOR_SRC_COLOR);

      si_blend_remove_dst(&eqA, &srcA, &dstA, VK_BLEND_FACTOR_DST_ALPHA,
                          VK_BLEND_FACTOR_SRC_ALPHA);

      /* Look up the ideal settings from tables. */
      srcRGB_opt = si_translate_blend_opt_factor(srcRGB, false);
      dstRGB_opt = si_translate_blend_opt_factor(dstRGB, false);
      srcA_opt = si_translate_blend_opt_factor(srcA, true);
      dstA_opt = si_translate_blend_opt_factor(dstA, true);

      /* Handle interdependencies. */
      if (si_blend_factor_uses_dst(srcRGB))
         dstRGB_opt = V_028760_BLEND_OPT_PRESERVE_NONE_IGNORE_NONE;
      if (si_blend_factor_uses_dst(srcA))
         dstA_opt = V_028760_BLEND_OPT_PRESERVE_NONE_IGNORE_NONE;

      if (srcRGB == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE &&
          (dstRGB == VK_BLEND_FACTOR_ZERO || dstRGB == VK_BLEND_FACTOR_SRC_ALPHA ||
           dstRGB == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE))
         dstRGB_opt = V_028760_BLEND_OPT_PRESERVE_NONE_IGNORE_A0;

      /* Set the final value. */
      sx_mrt_blend_opt[i] =
         S_028760_COLOR_SRC_OPT(srcRGB_opt) | S_028760_COLOR_DST_OPT(dstRGB_opt) |
         S_028760_COLOR_COMB_FCN(si_translate_blend_opt_function(eqRGB)) |
         S_028760_ALPHA_SRC_OPT(srcA_opt) | S_028760_ALPHA_DST_OPT(dstA_opt) |
         S_028760_ALPHA_COMB_FCN(si_translate_blend_opt_function(eqA));

      blend_cntl |= S_028780_ENABLE(1);
      blend_cntl |= S_028780_COLOR_COMB_FCN(si_translate_blend_function(eqRGB));
      blend_cntl |= S_028780_COLOR_SRCBLEND(si_translate_blend_factor(gfx_level, srcRGB));
      blend_cntl |= S_028780_COLOR_DESTBLEND(si_translate_blend_factor(gfx_level, dstRGB));
      if (srcA != srcRGB || dstA != dstRGB || eqA != eqRGB) {
         blend_cntl |= S_028780_SEPARATE_ALPHA_BLEND(1);
         blend_cntl |= S_028780_ALPHA_COMB_FCN(si_translate_blend_function(eqA));
         blend_cntl |= S_028780_ALPHA_SRCBLEND(si_translate_blend_factor(gfx_level, srcA));
         blend_cntl |= S_028780_ALPHA_DESTBLEND(si_translate_blend_factor(gfx_level, dstA));
      }
      cb_blend_control[i] = blend_cntl;
   }

   if (pdevice->rad_info.has_rbplus) {
      /* Disable RB+ blend optimizations for dual source blending. */
      if (mrt0_is_dual_src) {
         for (unsigned i = 0; i < MAX_RTS; i++) {
            sx_mrt_blend_opt[i] = S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_NONE) |
                                  S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_NONE);
         }
      }
   }

   radeon_set_context_reg_seq(cmd_buffer->cs, R_028780_CB_BLEND0_CONTROL, MAX_RTS);
   radeon_emit_array(cmd_buffer->cs, cb_blend_control, MAX_RTS);

   if (pdevice->rad_info.has_rbplus) {
      radeon_set_context_reg_seq(cmd_buffer->cs, R_028760_SX_MRT0_BLEND_OPT, MAX_RTS);
      radeon_emit_array(cmd_buffer->cs, sx_mrt_blend_opt, MAX_RTS);
   }
}

uint32_t
radv_hash_ps_epilog(const void *key_)
{
   const struct radv_ps_epilog_key *key = key_;
   return _mesa_hash_data(key, sizeof(*key));
}

bool
radv_cmp_ps_epilog(const void *a_, const void *b_)
{
   const struct radv_ps_epilog_key *a = a_;
   const struct radv_ps_epilog_key *b = b_;
   return memcmp(a, b, sizeof(*a)) == 0;
}

static struct radv_shader_part *
lookup_ps_epilog(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_device *device = cmd_buffer->device;
   struct radv_shader_part *epilog = NULL;
   struct radv_ps_epilog_state state = {0};

   state.color_attachment_count = render->color_att_count;
   for (unsigned i = 0; i < render->color_att_count; ++i) {
      state.color_attachment_formats[i] = render->color_att[i].format;
   }

   for (unsigned i = 0; i < MAX_RTS; i++) {
      VkBlendOp eqRGB = d->vk.cb.attachments[i].color_blend_op;
      VkBlendFactor srcRGB = d->vk.cb.attachments[i].src_color_blend_factor;
      VkBlendFactor dstRGB = d->vk.cb.attachments[i].dst_color_blend_factor;

      state.color_write_mask |= d->vk.cb.attachments[i].write_mask << (4 * i);
      state.color_blend_enable |= d->vk.cb.attachments[i].blend_enable << (4 * i);

      radv_normalize_blend_factor(eqRGB, &srcRGB, &dstRGB);

      if (srcRGB == VK_BLEND_FACTOR_SRC_ALPHA || dstRGB == VK_BLEND_FACTOR_SRC_ALPHA ||
          srcRGB == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE ||
          dstRGB == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE ||
          srcRGB == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ||
          dstRGB == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA)
         state.need_src_alpha |= 1 << i;
   }

   state.mrt0_is_dual_src = radv_is_mrt0_dual_src(cmd_buffer);

   if (d->vk.ms.alpha_to_coverage_enable) {
      /* Select a color export format with alpha when alpha to coverage is enabled. */
      state.need_src_alpha |= 0x1;
   }

   struct radv_ps_epilog_key key = radv_generate_ps_epilog_key(pipeline, &state, true);
   uint32_t hash = radv_hash_ps_epilog(&key);

   u_rwlock_rdlock(&device->ps_epilogs_lock);
   struct hash_entry *epilog_entry =
      _mesa_hash_table_search_pre_hashed(device->ps_epilogs, hash, &key);
   u_rwlock_rdunlock(&device->ps_epilogs_lock);

   if (!epilog_entry) {
      u_rwlock_wrlock(&device->ps_epilogs_lock);
      epilog_entry = _mesa_hash_table_search_pre_hashed(device->ps_epilogs, hash, &key);
      if (epilog_entry) {
         u_rwlock_wrunlock(&device->ps_epilogs_lock);
         return epilog_entry->data;
      }

      epilog = radv_create_ps_epilog(device, &key);
      struct radv_ps_epilog_key *key2 = malloc(sizeof(*key2));
      if (!epilog || !key2) {
         radv_shader_part_unref(device, epilog);
         free(key2);
         u_rwlock_wrunlock(&device->ps_epilogs_lock);
         return NULL;
      }
      memcpy(key2, &key, sizeof(*key2));
      _mesa_hash_table_insert_pre_hashed(device->ps_epilogs, hash, key2, epilog);

      u_rwlock_wrunlock(&device->ps_epilogs_lock);
      return epilog;
   }

   return epilog_entry->data;
}

static void
radv_emit_msaa_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_physical_device *pdevice = cmd_buffer->device->physical_device;
   unsigned rasterization_samples = radv_get_rasterization_samples(cmd_buffer);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned log_samples = util_logbase2(rasterization_samples);
   unsigned pa_sc_aa_config = 0;
   unsigned max_sample_dist = 0;
   unsigned db_eqaa;

   db_eqaa = S_028804_HIGH_QUALITY_INTERSECTIONS(1) | S_028804_INCOHERENT_EQAA_READS(1) |
             S_028804_STATIC_ANCHOR_ASSOCIATIONS(1);

   if (pdevice->rad_info.gfx_level >= GFX9 &&
       d->vk.rs.conservative_mode != VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT) {
      /* Adjust MSAA state if conservative rasterization is enabled. */
      db_eqaa |= S_028804_ENABLE_POSTZ_OVERRASTERIZATION(1) | S_028804_OVERRASTERIZATION_AMOUNT(4);
      pa_sc_aa_config |= S_028BE0_AA_MASK_CENTROID_DTMN(1);
   }

   if (!d->sample_location.count) {
      max_sample_dist = radv_get_default_max_sample_dist(log_samples);
   } else {
      uint32_t num_samples = (uint32_t)d->sample_location.per_pixel;
      VkOffset2D sample_locs[4][8]; /* 8 is the max. sample count supported */

      /* Convert the user sample locations to hardware sample locations. */
      radv_convert_user_sample_locs(&d->sample_location, 0, 0, sample_locs[0]);
      radv_convert_user_sample_locs(&d->sample_location, 1, 0, sample_locs[1]);
      radv_convert_user_sample_locs(&d->sample_location, 0, 1, sample_locs[2]);
      radv_convert_user_sample_locs(&d->sample_location, 1, 1, sample_locs[3]);

      /* Compute the maximum sample distance from the specified locations. */
      for (unsigned i = 0; i < 4; ++i) {
         for (uint32_t j = 0; j < num_samples; j++) {
            VkOffset2D offset = sample_locs[i][j];
            max_sample_dist = MAX2(max_sample_dist, MAX2(abs(offset.x), abs(offset.y)));
         }
      }
   }

   if (rasterization_samples > 1) {
      unsigned z_samples = MAX2(render->ds_samples, rasterization_samples);
      unsigned ps_iter_samples = radv_get_ps_iter_samples(cmd_buffer);
      unsigned log_z_samples = util_logbase2(z_samples);
      unsigned log_ps_iter_samples = util_logbase2(ps_iter_samples);

      db_eqaa |= S_028804_MAX_ANCHOR_SAMPLES(log_z_samples) |
                 S_028804_PS_ITER_SAMPLES(log_ps_iter_samples) |
                 S_028804_MASK_EXPORT_NUM_SAMPLES(log_samples) |
                 S_028804_ALPHA_TO_MASK_NUM_SAMPLES(log_samples);

      pa_sc_aa_config |= S_028BE0_MSAA_NUM_SAMPLES(log_samples) |
                         S_028BE0_MAX_SAMPLE_DIST(max_sample_dist) |
                         S_028BE0_MSAA_EXPOSED_SAMPLES(log_samples) | /* CM_R_028BE0_PA_SC_AA_CONFIG */
                         S_028BE0_COVERED_CENTROID_IS_CENTER(pdevice->rad_info.gfx_level >= GFX10_3);
   }

   radeon_set_context_reg(cmd_buffer->cs, R_028804_DB_EQAA, db_eqaa);
   radeon_set_context_reg(cmd_buffer->cs, R_028BE0_PA_SC_AA_CONFIG, pa_sc_aa_config);
   radeon_set_context_reg(cmd_buffer->cs, R_028A48_PA_SC_MODE_CNTL_0,
                          S_028A48_ALTERNATE_RBS_PER_TILE(pdevice->rad_info.gfx_level >= GFX9) |
                             S_028A48_VPORT_SCISSOR_ENABLE(1) |
                             S_028A48_LINE_STIPPLE_ENABLE(d->vk.rs.line.stipple.enable) |
                             S_028A48_MSAA_ENABLE(rasterization_samples > 1));
}

static void
radv_cmd_buffer_flush_dynamic_state(struct radv_cmd_buffer *cmd_buffer, bool pipeline_is_dirty)
{
   const uint64_t states =
      cmd_buffer->state.dirty & cmd_buffer->state.emitted_graphics_pipeline->needed_dynamic_state;

   if (!states)
      return;

   if (states & (RADV_CMD_DIRTY_DYNAMIC_VIEWPORT |
                 RADV_CMD_DIRTY_DYNAMIC_DEPTH_CLIP_ENABLE |
                 RADV_CMD_DIRTY_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE |
                 RADV_CMD_DIRTY_DYNAMIC_DEPTH_CLAMP_ENABLE))
      radv_emit_viewport(cmd_buffer);

   if (states & (RADV_CMD_DIRTY_DYNAMIC_SCISSOR | RADV_CMD_DIRTY_DYNAMIC_VIEWPORT) &&
       !cmd_buffer->device->physical_device->rad_info.has_gfx9_scissor_bug)
      radv_emit_scissor(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_LINE_WIDTH)
      radv_emit_line_width(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_BLEND_CONSTANTS)
      radv_emit_blend_constants(cmd_buffer);

   if (states &
       (RADV_CMD_DIRTY_DYNAMIC_STENCIL_REFERENCE | RADV_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK |
        RADV_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK))
      radv_emit_stencil(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_DEPTH_BOUNDS)
      radv_emit_depth_bounds(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_DEPTH_BIAS)
      radv_emit_depth_bias(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_DISCARD_RECTANGLE)
      radv_emit_discard_rectangle(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_CONSERVATIVE_RAST_MODE)
      radv_emit_conservative_rast_mode(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_SAMPLE_LOCATIONS)
      radv_emit_sample_locations(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_LINE_STIPPLE)
      radv_emit_line_stipple(cmd_buffer);

   if (states & (RADV_CMD_DIRTY_DYNAMIC_CULL_MODE | RADV_CMD_DIRTY_DYNAMIC_FRONT_FACE |
                 RADV_CMD_DIRTY_DYNAMIC_DEPTH_BIAS_ENABLE | RADV_CMD_DIRTY_DYNAMIC_POLYGON_MODE |
                 RADV_CMD_DIRTY_DYNAMIC_PROVOKING_VERTEX_MODE))
      radv_emit_culling(cmd_buffer);

   if (states & (RADV_CMD_DIRTY_DYNAMIC_PROVOKING_VERTEX_MODE |
                 RADV_CMD_DIRTY_DYNAMIC_PRIMITIVE_TOPOLOGY))
      radv_emit_provoking_vertex_mode(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_PRIMITIVE_TOPOLOGY)
      radv_emit_primitive_topology(cmd_buffer);

   if (states &
       (RADV_CMD_DIRTY_DYNAMIC_DEPTH_TEST_ENABLE | RADV_CMD_DIRTY_DYNAMIC_DEPTH_WRITE_ENABLE |
        RADV_CMD_DIRTY_DYNAMIC_DEPTH_COMPARE_OP | RADV_CMD_DIRTY_DYNAMIC_DEPTH_BOUNDS_TEST_ENABLE |
        RADV_CMD_DIRTY_DYNAMIC_STENCIL_TEST_ENABLE | RADV_CMD_DIRTY_DYNAMIC_STENCIL_OP))
      radv_emit_depth_control(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_STENCIL_OP)
      radv_emit_stencil_control(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_FRAGMENT_SHADING_RATE)
      radv_emit_fragment_shading_rate(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_PRIMITIVE_RESTART_ENABLE)
      radv_emit_primitive_restart_enable(cmd_buffer);

   if (states & (RADV_CMD_DIRTY_DYNAMIC_RASTERIZER_DISCARD_ENABLE |
                 RADV_CMD_DIRTY_DYNAMIC_DEPTH_CLIP_ENABLE |
                 RADV_CMD_DIRTY_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE))
      radv_emit_clipping(cmd_buffer);

   if (states & (RADV_CMD_DIRTY_DYNAMIC_LOGIC_OP | RADV_CMD_DIRTY_DYNAMIC_LOGIC_OP_ENABLE |
                 RADV_CMD_DIRTY_DYNAMIC_COLOR_WRITE_MASK |
                 RADV_CMD_DIRTY_DYNAMIC_COLOR_BLEND_ENABLE |
                 RADV_CMD_DIRTY_DYNAMIC_COLOR_BLEND_EQUATION))
      radv_emit_logic_op(cmd_buffer);

   if (states & (RADV_CMD_DIRTY_DYNAMIC_COLOR_WRITE_ENABLE |
                 RADV_CMD_DIRTY_DYNAMIC_COLOR_WRITE_MASK))
      radv_emit_color_write(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_VERTEX_INPUT)
      radv_emit_vertex_input(cmd_buffer, pipeline_is_dirty);

   if (states & RADV_CMD_DIRTY_DYNAMIC_PATCH_CONTROL_POINTS)
      radv_emit_patch_control_points(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_TESS_DOMAIN_ORIGIN)
      radv_emit_tess_domain_origin(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE)
      radv_emit_alpha_to_coverage_enable(cmd_buffer);

   if (states & RADV_CMD_DIRTY_DYNAMIC_SAMPLE_MASK)
      radv_emit_sample_mask(cmd_buffer);

   if (states & (RADV_CMD_DIRTY_DYNAMIC_DEPTH_CLAMP_ENABLE |
                 RADV_CMD_DIRTY_DYNAMIC_DEPTH_CLIP_ENABLE))
      radv_emit_depth_clamp_enable(cmd_buffer);

   if (states & (RADV_CMD_DIRTY_DYNAMIC_COLOR_BLEND_ENABLE |
                 RADV_CMD_DIRTY_DYNAMIC_COLOR_WRITE_MASK |
                 RADV_CMD_DIRTY_DYNAMIC_COLOR_BLEND_EQUATION))
      radv_emit_color_blend(cmd_buffer);

   if (states & (RADV_CMD_DIRTY_DYNAMIC_RASTERIZATION_SAMPLES |
                 RADV_CMD_DIRTY_DYNAMIC_LINE_RASTERIZATION_MODE))
      radv_emit_rasterization_samples(cmd_buffer);

   if (states & (RADV_CMD_DIRTY_DYNAMIC_LINE_STIPPLE_ENABLE |
                 RADV_CMD_DIRTY_DYNAMIC_CONSERVATIVE_RAST_MODE |
                 RADV_CMD_DIRTY_DYNAMIC_SAMPLE_LOCATIONS |
                 RADV_CMD_DIRTY_DYNAMIC_RASTERIZATION_SAMPLES |
                 RADV_CMD_DIRTY_DYNAMIC_LINE_RASTERIZATION_MODE))
      radv_emit_msaa_state(cmd_buffer);

   cmd_buffer->state.dirty &= ~states;
}

static void
radv_flush_push_descriptors(struct radv_cmd_buffer *cmd_buffer,
                            struct radv_descriptor_state *descriptors_state)
{
   struct radv_descriptor_set *set = (struct radv_descriptor_set *)&descriptors_state->push_set.set;
   unsigned bo_offset;

   if (!radv_cmd_buffer_upload_data(cmd_buffer, set->header.size, set->header.mapped_ptr,
                                    &bo_offset))
      return;

   set->header.va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
   set->header.va += bo_offset;
}

static void
radv_flush_indirect_descriptor_sets(struct radv_cmd_buffer *cmd_buffer,
                                    struct radv_pipeline *pipeline, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, bind_point);
   uint32_t size = MAX_SETS * 4;
   uint32_t offset;
   void *ptr;

   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, size, &offset, &ptr))
      return;

   for (unsigned i = 0; i < MAX_SETS; i++) {
      uint32_t *uptr = ((uint32_t *)ptr) + i;
      uint64_t set_va = 0;
      if (descriptors_state->valid & (1u << i))
         set_va = radv_descriptor_get_va(descriptors_state, i);

      uptr[0] = set_va & 0xffffffff;
   }

   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   struct radv_device *device = cmd_buffer->device;
   uint64_t va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
   va += offset;

   if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
      struct radv_graphics_pipeline *graphics_pipeline = radv_pipeline_to_graphics(pipeline);

      for (unsigned s = MESA_SHADER_VERTEX; s <= MESA_SHADER_FRAGMENT; s++)
         if (radv_pipeline_has_stage(graphics_pipeline, s))
            radv_emit_userdata_address(device, cs, pipeline, s, AC_UD_INDIRECT_DESCRIPTOR_SETS, va);

      if (radv_pipeline_has_stage(graphics_pipeline, MESA_SHADER_MESH))
         radv_emit_userdata_address(device, cs, pipeline, MESA_SHADER_MESH,
                                    AC_UD_INDIRECT_DESCRIPTOR_SETS, va);

      if (radv_pipeline_has_stage(graphics_pipeline, MESA_SHADER_TASK))
         radv_emit_userdata_address(device, cmd_buffer->ace_internal.cs, pipeline, MESA_SHADER_TASK,
                                    AC_UD_INDIRECT_DESCRIPTOR_SETS, va);
   } else {
      radv_emit_userdata_address(device, cs, pipeline, MESA_SHADER_COMPUTE,
                                 AC_UD_INDIRECT_DESCRIPTOR_SETS, va);
   }
}

ALWAYS_INLINE static void
radv_flush_descriptors(struct radv_cmd_buffer *cmd_buffer, VkShaderStageFlags stages,
                       struct radv_pipeline *pipeline, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, bind_point);
   struct radv_device *device = cmd_buffer->device;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   bool flush_indirect_descriptors;

   if (!descriptors_state->dirty)
      return;

   flush_indirect_descriptors = pipeline->need_indirect_descriptor_sets;

   if (flush_indirect_descriptors)
      radv_flush_indirect_descriptor_sets(cmd_buffer, pipeline, bind_point);

   ASSERTED unsigned cdw_max =
      radeon_check_space(device->ws, cs, MAX_SETS * MESA_VULKAN_SHADER_STAGES * 4);

   if (stages & VK_SHADER_STAGE_COMPUTE_BIT) {
      radv_emit_descriptor_pointers(device, cs, pipeline, descriptors_state, MESA_SHADER_COMPUTE);
   } else {
      radv_foreach_stage(stage, stages & ~VK_SHADER_STAGE_TASK_BIT_EXT)
      {
         if (!cmd_buffer->state.graphics_pipeline->base.shaders[stage])
            continue;

         radv_emit_descriptor_pointers(device, cs, pipeline, descriptors_state, stage);
      }

      if (stages & VK_SHADER_STAGE_TASK_BIT_EXT) {
         radv_emit_descriptor_pointers(device, cmd_buffer->ace_internal.cs, pipeline,
                                       descriptors_state, MESA_SHADER_TASK);
      }
   }

   descriptors_state->dirty = 0;

   assert(cmd_buffer->cs->cdw <= cdw_max);

   if (unlikely(cmd_buffer->device->trace_bo))
      radv_save_descriptors(cmd_buffer, bind_point);
}

static bool
radv_shader_loads_push_constants(struct radv_pipeline *pipeline, gl_shader_stage stage)
{
   struct radv_userdata_info *loc =
      radv_lookup_user_sgpr(pipeline, stage, AC_UD_PUSH_CONSTANTS);
   return loc->sgpr_idx != -1;
}

static void
radv_emit_all_inline_push_consts(struct radv_device *device, struct radeon_cmdbuf *cs,
                                 struct radv_pipeline *pipeline, gl_shader_stage stage,
                                 uint32_t *values, bool *need_push_constants)
{
   const struct radv_shader *shader = radv_get_shader(pipeline, stage);
   if (!shader)
      return;

   *need_push_constants |= radv_shader_loads_push_constants(pipeline, stage);

   const uint64_t mask = shader->info.inline_push_constant_mask;
   if (!mask)
      return;

   const uint8_t base = ffs(mask) - 1;
   if (mask == u_bit_consecutive64(base, util_last_bit64(mask) - base)) {
      /* consecutive inline push constants */
      radv_emit_inline_push_consts(device, cs, pipeline, stage, AC_UD_INLINE_PUSH_CONSTANTS,
                                   values + base);
   } else {
      /* sparse inline push constants */
      uint32_t consts[AC_MAX_INLINE_PUSH_CONSTS];
      unsigned num_consts = 0;
      u_foreach_bit64 (idx, mask)
         consts[num_consts++] = values[idx];
      radv_emit_inline_push_consts(device, cs, pipeline, stage, AC_UD_INLINE_PUSH_CONSTANTS,
                                   consts);
   }
}

ALWAYS_INLINE static VkShaderStageFlags
radv_must_flush_constants(const struct radv_cmd_buffer *cmd_buffer,
                          const struct radv_pipeline *pipeline, VkShaderStageFlags stages)
{
   if (pipeline->push_constant_size || pipeline->dynamic_offset_count)
      return stages & cmd_buffer->push_constant_stages;

   return 0;
}

static void
radv_flush_constants(struct radv_cmd_buffer *cmd_buffer, VkShaderStageFlags stages,
                     struct radv_pipeline *pipeline, VkPipelineBindPoint bind_point)
{
   struct radv_device *device = cmd_buffer->device;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, bind_point);
   struct radv_shader *shader, *prev_shader;
   bool need_push_constants = false;
   unsigned offset;
   void *ptr;
   uint64_t va;
   uint32_t internal_stages = stages;
   uint32_t dirty_stages = 0;

   switch (bind_point) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      break;
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      dirty_stages = RADV_RT_STAGE_BITS;
      break;
   case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR:
      internal_stages = VK_SHADER_STAGE_COMPUTE_BIT;
      dirty_stages = VK_SHADER_STAGE_COMPUTE_BIT;
      break;
   default:
      unreachable("Unhandled bind point");
   }

   radv_foreach_stage(stage, internal_stages & ~VK_SHADER_STAGE_TASK_BIT_EXT)
   {
      radv_emit_all_inline_push_consts(
         device, cs, pipeline, stage, (uint32_t *)cmd_buffer->push_constants, &need_push_constants);
   }

   if (internal_stages & VK_SHADER_STAGE_TASK_BIT_EXT) {
      radv_emit_all_inline_push_consts(device, cmd_buffer->ace_internal.cs, pipeline,
                                       MESA_SHADER_TASK, (uint32_t *)cmd_buffer->push_constants,
                                       &need_push_constants);
   }

   if (need_push_constants) {
      if (!radv_cmd_buffer_upload_alloc(
             cmd_buffer, pipeline->push_constant_size + 16 * pipeline->dynamic_offset_count, &offset,
             &ptr))
         return;

      memcpy(ptr, cmd_buffer->push_constants, pipeline->push_constant_size);
      memcpy((char *)ptr + pipeline->push_constant_size, descriptors_state->dynamic_buffers,
             16 * pipeline->dynamic_offset_count);

      va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
      va += offset;

      ASSERTED unsigned cdw_max =
         radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, MESA_VULKAN_SHADER_STAGES * 4);

      prev_shader = NULL;
      radv_foreach_stage(stage, internal_stages & ~VK_SHADER_STAGE_TASK_BIT_EXT)
      {
         shader = radv_get_shader(pipeline, stage);

         /* Avoid redundantly emitting the address for merged stages. */
         if (shader && shader != prev_shader) {
            radv_emit_userdata_address(device, cs, pipeline, stage, AC_UD_PUSH_CONSTANTS, va);

            prev_shader = shader;
         }
      }

      if (internal_stages & VK_SHADER_STAGE_TASK_BIT_EXT) {
         radv_emit_userdata_address(device, cmd_buffer->ace_internal.cs, pipeline, MESA_SHADER_TASK,
                                    AC_UD_PUSH_CONSTANTS, va);
      }

      assert(cmd_buffer->cs->cdw <= cdw_max);
   }

   cmd_buffer->push_constant_stages &= ~stages;
   cmd_buffer->push_constant_stages |= dirty_stages;
}

void
radv_write_vertex_descriptors(const struct radv_cmd_buffer *cmd_buffer,
                              const struct radv_graphics_pipeline *pipeline,
                              bool full_null_descriptors, void *vb_ptr)
{
   struct radv_shader *vs_shader = radv_get_shader(&pipeline->base, MESA_SHADER_VERTEX);
   enum amd_gfx_level chip = cmd_buffer->device->physical_device->rad_info.gfx_level;
   enum radeon_family family = cmd_buffer->device->physical_device->rad_info.family;
   unsigned desc_index = 0;
   uint32_t mask = pipeline->vb_desc_usage_mask;
   uint64_t va;
   const struct radv_vs_input_state *vs_state =
      vs_shader->info.vs.dynamic_inputs ? &cmd_buffer->state.dynamic_vs_input : NULL;
   assert(!vs_state || pipeline->use_per_attribute_vb_descs);

   const struct ac_vtx_format_info *vtx_info_table =
      vs_state ? ac_get_vtx_format_info_table(chip, family) : NULL;

   while (mask) {
      unsigned i = u_bit_scan(&mask);
      uint32_t *desc = &((uint32_t *)vb_ptr)[desc_index++ * 4];
      uint32_t offset, rsrc_word3;

      if (vs_state && !(vs_state->attribute_mask & BITFIELD_BIT(i))) {
         /* No vertex attribute description given: assume that the shader doesn't use this
          * location (vb_desc_usage_mask can be larger than attribute usage) and use a null
          * descriptor to avoid hangs (prologs load all attributes, even if there are holes).
          */
         memset(desc, 0, 4 * 4);
         continue;
      }

      unsigned binding =
         vs_state ? cmd_buffer->state.dynamic_vs_input.bindings[i]
                  : (pipeline->use_per_attribute_vb_descs ? pipeline->attrib_bindings[i] : i);
      struct radv_buffer *buffer = cmd_buffer->vertex_binding_buffers[binding];
      unsigned num_records;
      unsigned stride;

      if (vs_state && !(vs_state->nontrivial_formats & BITFIELD_BIT(i))) {
         const struct ac_vtx_format_info *vtx_info = &vtx_info_table[vs_state->formats[i]];
         unsigned hw_format = vtx_info->hw_format[vtx_info->num_channels - 1];

         if (chip >= GFX10) {
            rsrc_word3 = vtx_info->dst_sel | S_008F0C_FORMAT(hw_format);
         } else {
            rsrc_word3 = vtx_info->dst_sel | S_008F0C_NUM_FORMAT((hw_format >> 4) & 0x7) |
                         S_008F0C_DATA_FORMAT(hw_format & 0xf);
         }
      } else {
         rsrc_word3 = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
                      S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
                      S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) | S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W);
         if (chip >= GFX10)
            rsrc_word3 |= S_008F0C_FORMAT(V_008F0C_GFX10_FORMAT_32_UINT);
         else
            rsrc_word3 |= S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_UINT) |
                          S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32);
      }

      if (pipeline->dynamic_states & (RADV_DYNAMIC_VERTEX_INPUT_BINDING_STRIDE |
                                      RADV_DYNAMIC_VERTEX_INPUT)) {
         stride = cmd_buffer->vertex_bindings[binding].stride;
      } else {
         stride = pipeline->binding_stride[binding];
      }

      if (!buffer) {
         if (full_null_descriptors) {
            /* Put all the info in for the DGC generation shader in case the VBO gets overridden. */
            desc[0] = 0;
            desc[1] = S_008F04_STRIDE(stride);
            desc[2] = 0;
            desc[3] = rsrc_word3;
         } else if (vs_state) {
            /* Stride needs to be non-zero on GFX9, or else bounds checking is disabled. We need
             * to include the format/word3 so that the alpha channel is 1 for formats without an
             * alpha channel.
             */
            desc[0] = 0;
            desc[1] = S_008F04_STRIDE(16);
            desc[2] = 0;
            desc[3] = rsrc_word3;
         } else {
            memset(desc, 0, 4 * 4);
         }

         continue;
      }

      va = radv_buffer_get_va(buffer->bo);

      offset = cmd_buffer->vertex_bindings[binding].offset;
      va += offset + buffer->offset;
      if (vs_state)
         va += vs_state->offsets[i];

      if (cmd_buffer->vertex_bindings[binding].size) {
         num_records = cmd_buffer->vertex_bindings[binding].size;
      } else {
         num_records = vk_buffer_range(&buffer->vk, offset, VK_WHOLE_SIZE);
      }

      if (pipeline->use_per_attribute_vb_descs) {
         uint32_t attrib_end =
            vs_state ? vs_state->offsets[i] + vs_state->format_sizes[i] : pipeline->attrib_ends[i];

         if (num_records < attrib_end) {
            num_records = 0; /* not enough space for one vertex */
         } else if (stride == 0) {
            num_records = 1; /* only one vertex */
         } else {
            num_records = (num_records - attrib_end) / stride + 1;
            /* If attrib_offset>stride, then the compiler will increase the vertex index by
             * attrib_offset/stride and decrease the offset by attrib_offset%stride. This is
             * only allowed with static strides.
             */
            num_records += pipeline->attrib_index_offset[i];
         }

         /* GFX10 uses OOB_SELECT_RAW if stride==0, so convert num_records from elements into
          * into bytes in that case. GFX8 always uses bytes.
          */
         if (num_records && (chip == GFX8 || (chip != GFX9 && !stride))) {
            num_records = (num_records - 1) * stride + attrib_end;
         } else if (!num_records) {
            /* On GFX9, it seems bounds checking is disabled if both
             * num_records and stride are zero. This doesn't seem necessary on GFX8, GFX10 and
             * GFX10.3 but it doesn't hurt.
             */
            if (full_null_descriptors) {
               /* Put all the info in for the DGC generation shader in case the VBO gets overridden.
                */
               desc[0] = 0;
               desc[1] = S_008F04_STRIDE(stride);
               desc[2] = 0;
               desc[3] = rsrc_word3;
            } else if (vs_state) {
               desc[0] = 0;
               desc[1] = S_008F04_STRIDE(16);
               desc[2] = 0;
               desc[3] = rsrc_word3;
            } else {
               memset(desc, 0, 16);
            }

            continue;
         }
      } else {
         if (chip != GFX8 && stride)
            num_records = DIV_ROUND_UP(num_records, stride);
      }

      if (chip >= GFX10) {
         /* OOB_SELECT chooses the out-of-bounds check:
          * - 1: index >= NUM_RECORDS (Structured)
          * - 3: offset >= NUM_RECORDS (Raw)
          */
         int oob_select = stride ? V_008F0C_OOB_SELECT_STRUCTURED : V_008F0C_OOB_SELECT_RAW;
         rsrc_word3 |= S_008F0C_OOB_SELECT(oob_select) | S_008F0C_RESOURCE_LEVEL(chip < GFX11);
      }

      desc[0] = va;
      desc[1] = S_008F04_BASE_ADDRESS_HI(va >> 32) | S_008F04_STRIDE(stride);
      desc[2] = num_records;
      desc[3] = rsrc_word3;
   }
}

static void
radv_flush_vertex_descriptors(struct radv_cmd_buffer *cmd_buffer)
{
   if (!cmd_buffer->state.graphics_pipeline->vb_desc_usage_mask)
      return;

   /* Mesh shaders don't have vertex descriptors. */
   assert(!cmd_buffer->state.mesh_shading);

   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   unsigned vb_offset;
   void *vb_ptr;
   uint64_t va;

   /* allocate some descriptor state for vertex buffers */
   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, pipeline->vb_desc_alloc_size, &vb_offset,
                                     &vb_ptr))
      return;

   radv_write_vertex_descriptors(cmd_buffer, pipeline, false, vb_ptr);

   va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
   va += vb_offset;

   radv_emit_userdata_address(cmd_buffer->device, cmd_buffer->cs, &pipeline->base,
                              MESA_SHADER_VERTEX, AC_UD_VS_VERTEX_BUFFERS, va);

   cmd_buffer->state.vb_va = va;
   cmd_buffer->state.prefetch_L2_mask |= RADV_PREFETCH_VBO_DESCRIPTORS;

   if (unlikely(cmd_buffer->device->trace_bo))
      radv_save_vertex_descriptors(cmd_buffer, (uintptr_t)vb_ptr);

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_VERTEX_BUFFER;
}

static void
radv_emit_streamout_buffers(struct radv_cmd_buffer *cmd_buffer, uint64_t va)
{
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const struct radv_userdata_info *loc = &pipeline->last_vgt_api_stage_locs[AC_UD_STREAMOUT_BUFFERS];
   const unsigned stage = pipeline->last_vgt_api_stage;
   uint32_t base_reg;

   if (loc->sgpr_idx == -1)
      return;

   base_reg = pipeline->base.user_data_0[stage];

   radv_emit_shader_pointer(cmd_buffer->device, cmd_buffer->cs, base_reg + loc->sgpr_idx * 4, va,
                            false);

   if (radv_pipeline_has_gs_copy_shader(&pipeline->base)) {
      loc = &pipeline->base.gs_copy_shader->info.user_sgprs_locs.shader_data[AC_UD_STREAMOUT_BUFFERS];
      if (loc->sgpr_idx != -1) {
         base_reg = R_00B130_SPI_SHADER_USER_DATA_VS_0;

         radv_emit_shader_pointer(cmd_buffer->device, cmd_buffer->cs, base_reg + loc->sgpr_idx * 4,
                                  va, false);
      }
   }
}

static void
radv_flush_streamout_descriptors(struct radv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_STREAMOUT_BUFFER) {
      struct radv_streamout_binding *sb = cmd_buffer->streamout_bindings;
      struct radv_streamout_state *so = &cmd_buffer->state.streamout;
      unsigned so_offset;
      uint64_t desc_va;
      void *so_ptr;

      /* Allocate some descriptor state for streamout buffers. */
      if (!radv_cmd_buffer_upload_alloc(cmd_buffer, MAX_SO_BUFFERS * 16, &so_offset, &so_ptr))
         return;

      for (uint32_t i = 0; i < MAX_SO_BUFFERS; i++) {
         struct radv_buffer *buffer = sb[i].buffer;
         uint32_t *desc = &((uint32_t *)so_ptr)[i * 4];
         uint32_t size = 0;
         uint64_t va = 0;

         if (so->enabled_mask & (1 << i)) {
            va = radv_buffer_get_va(buffer->bo) + buffer->offset;

            va += sb[i].offset;

            /* Set the descriptor.
             *
             * On GFX8, the format must be non-INVALID, otherwise
             * the buffer will be considered not bound and store
             * instructions will be no-ops.
             */
            size = 0xffffffff;

            if (cmd_buffer->device->physical_device->use_ngg_streamout) {
               /* With NGG streamout, the buffer size is used to determine the max emit per buffer
                * and also acts as a disable bit when it's 0.
                */
               size = radv_is_streamout_enabled(cmd_buffer) ? sb[i].size : 0;
            }
         }

         uint32_t rsrc_word3 =
            S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) | S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
            S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) | S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W);

         if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX11) {
            rsrc_word3 |= S_008F0C_FORMAT(V_008F0C_GFX11_FORMAT_32_FLOAT) |
                          S_008F0C_OOB_SELECT(V_008F0C_OOB_SELECT_RAW);
         } else if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX10) {
            rsrc_word3 |= S_008F0C_FORMAT(V_008F0C_GFX10_FORMAT_32_FLOAT) |
                          S_008F0C_OOB_SELECT(V_008F0C_OOB_SELECT_RAW) | S_008F0C_RESOURCE_LEVEL(1);
         } else {
            rsrc_word3 |= S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32);
         }

         desc[0] = va;
         desc[1] = S_008F04_BASE_ADDRESS_HI(va >> 32);
         desc[2] = size;
         desc[3] = rsrc_word3;
      }

      desc_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
      desc_va += so_offset;

      radv_emit_streamout_buffers(cmd_buffer, desc_va);
   }

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_STREAMOUT_BUFFER;
}

static void
radv_flush_ngg_query_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const unsigned stage = pipeline->last_vgt_api_stage;
   const struct radv_userdata_info *loc = &pipeline->last_vgt_api_stage_locs[AC_UD_NGG_QUERY_STATE];
   enum radv_ngg_query_state ngg_query_state = radv_ngg_query_none;
   uint32_t base_reg;

   if (loc->sgpr_idx == -1)
      return;

   assert(pipeline->is_ngg);

   /* By default NGG queries are disabled but they are enabled if the command buffer has active GDS
    * queries or if it's a secondary command buffer that inherits the number of generated
    * primitives.
    */
   if (cmd_buffer->state.active_pipeline_gds_queries ||
       (cmd_buffer->state.inherited_pipeline_statistics &
        VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT))
      ngg_query_state |= radv_ngg_query_pipeline_stat;

   if (cmd_buffer->state.active_prims_gen_gds_queries)
      ngg_query_state |= radv_ngg_query_prim_gen;

   if (cmd_buffer->state.active_prims_xfb_gds_queries && radv_is_streamout_enabled(cmd_buffer)) {
      ngg_query_state |= radv_ngg_query_prim_xfb | radv_ngg_query_prim_gen;
   }

   base_reg = pipeline->base.user_data_0[stage];
   assert(loc->sgpr_idx != -1);

   radeon_set_sh_reg(cmd_buffer->cs, base_reg + loc->sgpr_idx * 4, ngg_query_state);
}

static void
radv_flush_force_vrs_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;

   if (!pipeline->force_vrs_per_vertex) {
      /* Un-set the SGPR index so we know to re-emit it later. */
      cmd_buffer->state.last_vrs_rates_sgpr_idx = -1;
      return;
   }

   struct radv_userdata_info *loc;
   uint32_t base_reg;

   if (radv_pipeline_has_gs_copy_shader(&pipeline->base)) {
      loc = &pipeline->base.gs_copy_shader->info.user_sgprs_locs.shader_data[AC_UD_FORCE_VRS_RATES];
      base_reg = R_00B130_SPI_SHADER_USER_DATA_VS_0;
   } else {
      loc = &pipeline->last_vgt_api_stage_locs[AC_UD_FORCE_VRS_RATES];
      base_reg = pipeline->base.user_data_0[pipeline->last_vgt_api_stage];
   }

   assert(loc->sgpr_idx != -1);

   enum amd_gfx_level gfx_level = pipeline->base.device->physical_device->rad_info.gfx_level;
   uint32_t vrs_rates = 0;

   switch (cmd_buffer->device->force_vrs) {
   case RADV_FORCE_VRS_2x2:
      vrs_rates = gfx_level >= GFX11 ? V_0283D0_VRS_SHADING_RATE_2X2 : (1u << 2) | (1u << 4);
      break;
   case RADV_FORCE_VRS_2x1:
      vrs_rates = gfx_level >= GFX11 ? V_0283D0_VRS_SHADING_RATE_2X1 : (1u << 2) | (0u << 4);
      break;
   case RADV_FORCE_VRS_1x2:
      vrs_rates = gfx_level >= GFX11 ? V_0283D0_VRS_SHADING_RATE_1X2 : (0u << 2) | (1u << 4);
      break;
   default:
      break;
   }

   if (cmd_buffer->state.last_vrs_rates != vrs_rates ||
       cmd_buffer->state.last_vrs_rates_sgpr_idx != loc->sgpr_idx) {
      radeon_set_sh_reg(cmd_buffer->cs, base_reg + loc->sgpr_idx * 4, vrs_rates);
   }

   cmd_buffer->state.last_vrs_rates = vrs_rates;
   cmd_buffer->state.last_vrs_rates_sgpr_idx = loc->sgpr_idx;
}

static void
radv_upload_graphics_shader_descriptors(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_VERTEX_BUFFER)
      radv_flush_vertex_descriptors(cmd_buffer);

   radv_flush_streamout_descriptors(cmd_buffer);

   VkShaderStageFlags stages = VK_SHADER_STAGE_ALL_GRAPHICS;
   radv_flush_descriptors(cmd_buffer, stages, &pipeline->base, VK_PIPELINE_BIND_POINT_GRAPHICS);

   const VkShaderStageFlags pc_stages =
      radv_must_flush_constants(cmd_buffer, &pipeline->base, stages);
   if (pc_stages)
      radv_flush_constants(cmd_buffer, pc_stages, &pipeline->base, VK_PIPELINE_BIND_POINT_GRAPHICS);

   radv_flush_force_vrs_state(cmd_buffer);
}

struct radv_draw_info {
   /**
    * Number of vertices.
    */
   uint32_t count;

   /**
    * First instance id.
    */
   uint32_t first_instance;

   /**
    * Number of instances.
    */
   uint32_t instance_count;

   /**
    * Whether it's an indexed draw.
    */
   bool indexed;

   /**
    * Indirect draw parameters resource.
    */
   struct radv_buffer *indirect;
   uint64_t indirect_offset;
   uint32_t stride;

   /**
    * Draw count parameters resource.
    */
   struct radv_buffer *count_buffer;
   uint64_t count_buffer_offset;

   /**
    * Stream output parameters resource.
    */
   struct radv_buffer *strmout_buffer;
   uint64_t strmout_buffer_offset;
};

static uint32_t
radv_get_primitive_reset_index(struct radv_cmd_buffer *cmd_buffer)
{
   uint32_t index_type = G_028A7C_INDEX_TYPE(cmd_buffer->state.index_type);
   switch (index_type) {
   case V_028A7C_VGT_INDEX_8:
      return 0xffu;
   case V_028A7C_VGT_INDEX_16:
      return 0xffffu;
   case V_028A7C_VGT_INDEX_32:
      return 0xffffffffu;
   default:
      unreachable("invalid index type");
   }
}

static void
si_emit_ia_multi_vgt_param(struct radv_cmd_buffer *cmd_buffer, bool instanced_draw,
                           bool indirect_draw, bool count_from_stream_output,
                           uint32_t draw_vertex_count)
{
   struct radeon_info *info = &cmd_buffer->device->physical_device->rad_info;
   struct radv_cmd_state *state = &cmd_buffer->state;
   unsigned patch_control_points = state->dynamic.vk.ts.patch_control_points;
   unsigned topology = state->dynamic.vk.ia.primitive_topology;
   bool prim_restart_enable = state->dynamic.vk.ia.primitive_restart_enable;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   unsigned ia_multi_vgt_param;

   ia_multi_vgt_param =
      si_get_ia_multi_vgt_param(cmd_buffer, instanced_draw, indirect_draw, count_from_stream_output,
                                draw_vertex_count, topology, prim_restart_enable,
                                patch_control_points, state->tess_num_patches);

   if (state->last_ia_multi_vgt_param != ia_multi_vgt_param) {
      if (info->gfx_level == GFX9) {
         radeon_set_uconfig_reg_idx(cmd_buffer->device->physical_device, cs,
                                    R_030960_IA_MULTI_VGT_PARAM, 4, ia_multi_vgt_param);
      } else if (info->gfx_level >= GFX7) {
         radeon_set_context_reg_idx(cs, R_028AA8_IA_MULTI_VGT_PARAM, 1, ia_multi_vgt_param);
      } else {
         radeon_set_context_reg(cs, R_028AA8_IA_MULTI_VGT_PARAM, ia_multi_vgt_param);
      }
      state->last_ia_multi_vgt_param = ia_multi_vgt_param;
   }
}

static void
gfx10_emit_ge_cntl(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radv_cmd_state *state = &cmd_buffer->state;
   bool break_wave_at_eoi = false;
   unsigned primgroup_size;
   unsigned ge_cntl;

   if (pipeline->is_ngg)
      return;

   if (radv_pipeline_has_stage(pipeline, MESA_SHADER_TESS_CTRL)) {
      primgroup_size = state->tess_num_patches;

      if (pipeline->base.shaders[MESA_SHADER_TESS_CTRL]->info.uses_prim_id ||
          radv_get_shader(&pipeline->base, MESA_SHADER_TESS_EVAL)->info.uses_prim_id) {
         break_wave_at_eoi = true;
      }
   } else if (radv_pipeline_has_stage(pipeline, MESA_SHADER_GEOMETRY)) {
      const struct gfx9_gs_info *gs_state =
         &pipeline->base.shaders[MESA_SHADER_GEOMETRY]->info.gs_ring_info;
      primgroup_size = G_028A44_GS_PRIMS_PER_SUBGRP(gs_state->vgt_gs_onchip_cntl);
   } else {
      primgroup_size = 128; /* recommended without a GS and tess */
   }

   ge_cntl = S_03096C_PRIM_GRP_SIZE_GFX10(primgroup_size) |
             S_03096C_VERT_GRP_SIZE(256) | /* disable vertex grouping */
             S_03096C_PACKET_TO_ONE_PA(0) /* line stipple */ |
             S_03096C_BREAK_WAVE_AT_EOI(break_wave_at_eoi);

   if (state->last_ge_cntl != ge_cntl) {
      radeon_set_uconfig_reg(cmd_buffer->cs, R_03096C_GE_CNTL, ge_cntl);
      state->last_ge_cntl = ge_cntl;
   }
}

static void
radv_emit_draw_registers(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *draw_info)
{
   struct radeon_info *info = &cmd_buffer->device->physical_device->rad_info;
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint32_t topology = state->dynamic.vk.ia.primitive_topology;
   bool disable_instance_packing = false;

   /* Draw state. */
   if (info->gfx_level >= GFX10) {
      gfx10_emit_ge_cntl(cmd_buffer);
   } else {
      si_emit_ia_multi_vgt_param(cmd_buffer, draw_info->instance_count > 1, draw_info->indirect,
                                 !!draw_info->strmout_buffer,
                                 draw_info->indirect ? 0 : draw_info->count);
   }

   if (state->dynamic.vk.ia.primitive_restart_enable) {
      uint32_t primitive_reset_index = radv_get_primitive_reset_index(cmd_buffer);

      if (primitive_reset_index != state->last_primitive_reset_index) {
         radeon_set_context_reg(cs, R_02840C_VGT_MULTI_PRIM_IB_RESET_INDX, primitive_reset_index);
         state->last_primitive_reset_index = primitive_reset_index;
      }
   }

   /* RDNA2 is affected by a hardware bug when instance packing is enabled for adjacent primitive
    * topologies and instance_count > 1, pipeline stats generated by GE are incorrect. It needs to
    * be applied for indexed and non-indexed draws.
    */
   if (info->gfx_level == GFX10_3 && state->active_pipeline_queries > 0 &&
       (draw_info->instance_count > 1 || draw_info->indirect) &&
       (topology == V_008958_DI_PT_LINELIST_ADJ || topology == V_008958_DI_PT_LINESTRIP_ADJ ||
        topology == V_008958_DI_PT_TRILIST_ADJ || topology == V_008958_DI_PT_TRISTRIP_ADJ)) {
      disable_instance_packing = true;
   }

   if ((draw_info->indexed &&
        (state->index_type != state->last_index_type || cmd_buffer->device->uses_shadow_regs)) ||
       (info->gfx_level == GFX10_3 &&
        (state->last_index_type == -1 ||
         disable_instance_packing != G_028A7C_DISABLE_INSTANCE_PACKING(state->last_index_type)))) {
      uint32_t index_type = state->index_type | S_028A7C_DISABLE_INSTANCE_PACKING(disable_instance_packing);

      if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX9) {
         radeon_set_uconfig_reg_idx(cmd_buffer->device->physical_device, cs,
                                    R_03090C_VGT_INDEX_TYPE, 2, index_type);
      } else {
         radeon_emit(cs, PKT3(PKT3_INDEX_TYPE, 0, 0));
         radeon_emit(cs, index_type);
      }

      state->last_index_type = index_type;
   }
}

static void
radv_stage_flush(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 src_stage_mask)
{
   /* For simplicity, if the barrier wants to wait for the task shader,
    * just make it wait for the mesh shader too.
    */
   if (src_stage_mask & VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
      src_stage_mask |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;

   if (src_stage_mask & (VK_PIPELINE_STAGE_2_COPY_BIT |
                         VK_PIPELINE_STAGE_2_RESOLVE_BIT |
                         VK_PIPELINE_STAGE_2_BLIT_BIT |
                         VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
      /* Be conservative for now. */
      src_stage_mask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
   }

   if (src_stage_mask &
       (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR |
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH;
   }

   if (src_stage_mask &
       (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
        VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_PS_PARTIAL_FLUSH;
   } else if (src_stage_mask &
              (VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
               VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
               VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
               VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
               VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
               VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
               VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT |
               VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VS_PARTIAL_FLUSH;
   }
}

static bool
can_skip_buffer_l2_flushes(struct radv_device *device)
{
   return device->physical_device->rad_info.gfx_level == GFX9 ||
          (device->physical_device->rad_info.gfx_level >= GFX10 &&
           !device->physical_device->rad_info.tcc_rb_non_coherent);
}

/*
 * In vulkan barriers have two kinds of operations:
 *
 * - visibility (implemented with radv_src_access_flush)
 * - availability (implemented with radv_dst_access_flush)
 *
 * for a memory operation to observe the result of a previous memory operation
 * one needs to do a visibility operation from the source memory and then an
 * availability operation to the target memory.
 *
 * The complication is the availability and visibility operations do not need to
 * be in the same barrier.
 *
 * The cleanest way to implement this is to define the visibility operation to
 * bring the caches to a "state of rest", which none of the caches below that
 * level dirty.
 *
 * For GFX8 and earlier this would be VRAM/GTT with none of the caches dirty.
 *
 * For GFX9+ we can define the state at rest to be L2 instead of VRAM for all
 * buffers and for images marked as coherent, and VRAM/GTT for non-coherent
 * images. However, given the existence of memory barriers which do not specify
 * the image/buffer it often devolves to just VRAM/GTT anyway.
 *
 * To help reducing the invalidations for GPUs that have L2 coherency between the
 * RB and the shader caches, we always invalidate L2 on the src side, as we can
 * use our knowledge of past usage to optimize flushes away.
 */

enum radv_cmd_flush_bits
radv_src_access_flush(struct radv_cmd_buffer *cmd_buffer, VkAccessFlags2 src_flags,
                      const struct radv_image *image)
{
   bool has_CB_meta = true, has_DB_meta = true;
   bool image_is_coherent = image ? image->l2_coherent : false;
   enum radv_cmd_flush_bits flush_bits = 0;

   if (image) {
      if (!radv_image_has_CB_metadata(image))
         has_CB_meta = false;
      if (!radv_image_has_htile(image))
         has_DB_meta = false;
   }

   u_foreach_bit64(b, src_flags)
   {
      switch ((VkAccessFlags2)(1 << b)) {
      case VK_ACCESS_2_SHADER_WRITE_BIT:
      case VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT:
         /* since the STORAGE bit isn't set we know that this is a meta operation.
          * on the dst flush side we skip CB/DB flushes without the STORAGE bit, so
          * set it here. */
         if (image && !(image->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
            if (vk_format_is_depth_or_stencil(image->vk.format)) {
               flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB;
            } else {
               flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB;
            }
         }

         if (!image_is_coherent)
            flush_bits |= RADV_CMD_FLAG_INV_L2;
         break;
      case VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR:
      case VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT:
      case VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT:
         if (!image_is_coherent)
            flush_bits |= RADV_CMD_FLAG_WB_L2;
         break;
      case VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT:
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB;
         if (has_CB_meta)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
         break;
      case VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB;
         if (has_DB_meta)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
         break;
      case VK_ACCESS_2_TRANSFER_WRITE_BIT:
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB;

         if (!image_is_coherent)
            flush_bits |= RADV_CMD_FLAG_INV_L2;
         if (has_CB_meta)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
         if (has_DB_meta)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
         break;
      case VK_ACCESS_2_MEMORY_WRITE_BIT:
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB;

         if (!image_is_coherent)
            flush_bits |= RADV_CMD_FLAG_INV_L2;
         if (has_CB_meta)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
         if (has_DB_meta)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
         break;
      default:
         break;
      }
   }
   return flush_bits;
}

enum radv_cmd_flush_bits
radv_dst_access_flush(struct radv_cmd_buffer *cmd_buffer, VkAccessFlags2 dst_flags,
                      const struct radv_image *image)
{
   bool has_CB_meta = true, has_DB_meta = true;
   enum radv_cmd_flush_bits flush_bits = 0;
   bool flush_CB = true, flush_DB = true;
   bool image_is_coherent = image ? image->l2_coherent : false;

   if (image) {
      if (!(image->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
         flush_CB = false;
         flush_DB = false;
      }

      if (!radv_image_has_CB_metadata(image))
         has_CB_meta = false;
      if (!radv_image_has_htile(image))
         has_DB_meta = false;
   }

   /* All the L2 invalidations below are not the CB/DB. So if there are no incoherent images
    * in the L2 cache in CB/DB mode then they are already usable from all the other L2 clients. */
   image_is_coherent |=
      can_skip_buffer_l2_flushes(cmd_buffer->device) && !cmd_buffer->state.rb_noncoherent_dirty;

   u_foreach_bit64(b, dst_flags)
   {
      switch ((VkAccessFlags2)(1 << b)) {
      case VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT:
         /* SMEM loads are used to read compute dispatch size in shaders */
         if (!cmd_buffer->device->load_grid_size_from_user_sgpr)
            flush_bits |= RADV_CMD_FLAG_INV_SCACHE;

         /* Ensure the DGC meta shader can read the commands. */
         if (cmd_buffer->device->uses_device_generated_commands) {
            flush_bits |= RADV_CMD_FLAG_INV_SCACHE | RADV_CMD_FLAG_INV_VCACHE;

            if (cmd_buffer->device->physical_device->rad_info.gfx_level < GFX9)
               flush_bits |= RADV_CMD_FLAG_INV_L2;
         }

         break;
      case VK_ACCESS_2_INDEX_READ_BIT:
      case VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT:
         break;
      case VK_ACCESS_2_UNIFORM_READ_BIT:
         flush_bits |= RADV_CMD_FLAG_INV_VCACHE | RADV_CMD_FLAG_INV_SCACHE;
         break;
      case VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT:
      case VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT:
      case VK_ACCESS_2_TRANSFER_READ_BIT:
      case VK_ACCESS_2_TRANSFER_WRITE_BIT:
         flush_bits |= RADV_CMD_FLAG_INV_VCACHE;

         if (has_CB_meta || has_DB_meta)
            flush_bits |= RADV_CMD_FLAG_INV_L2_METADATA;
         if (!image_is_coherent)
            flush_bits |= RADV_CMD_FLAG_INV_L2;
         break;
      case VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR:
      case VK_ACCESS_2_SHADER_READ_BIT:
      case VK_ACCESS_2_SHADER_STORAGE_READ_BIT:
         flush_bits |= RADV_CMD_FLAG_INV_VCACHE;
         /* Unlike LLVM, ACO uses SMEM for SSBOs and we have to
          * invalidate the scalar cache. */
         if (!cmd_buffer->device->physical_device->use_llvm && !image)
            flush_bits |= RADV_CMD_FLAG_INV_SCACHE;

         if (has_CB_meta || has_DB_meta)
            flush_bits |= RADV_CMD_FLAG_INV_L2_METADATA;
         if (!image_is_coherent)
            flush_bits |= RADV_CMD_FLAG_INV_L2;
         break;
      case VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR:
         flush_bits |= RADV_CMD_FLAG_INV_VCACHE;
         if (cmd_buffer->device->physical_device->rad_info.gfx_level < GFX9)
            flush_bits |= RADV_CMD_FLAG_INV_L2;
         break;
      case VK_ACCESS_2_SHADER_WRITE_BIT:
      case VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT:
      case VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR:
         break;
      case VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT:
      case VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT:
         if (flush_CB)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB;
         if (has_CB_meta)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
         break;
      case VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT:
      case VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:
         if (flush_DB)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB;
         if (has_DB_meta)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
         break;
      case VK_ACCESS_2_MEMORY_READ_BIT:
      case VK_ACCESS_2_MEMORY_WRITE_BIT:
         flush_bits |= RADV_CMD_FLAG_INV_VCACHE | RADV_CMD_FLAG_INV_SCACHE;
         if (!image_is_coherent)
            flush_bits |= RADV_CMD_FLAG_INV_L2;
         if (flush_CB)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB;
         if (has_CB_meta)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
         if (flush_DB)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB;
         if (has_DB_meta)
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
         break;
      default:
         break;
      }
   }
   return flush_bits;
}

void
radv_emit_resolve_barrier(struct radv_cmd_buffer *cmd_buffer,
                          const struct radv_resolve_barrier *barrier)
{
   struct radv_rendering_state *render = &cmd_buffer->state.render;

   for (uint32_t i = 0; i < render->color_att_count; i++) {
      struct radv_image_view *iview = render->color_att[i].iview;
      if (!iview)
         continue;

      cmd_buffer->state.flush_bits |=
         radv_src_access_flush(cmd_buffer, barrier->src_access_mask, iview->image);
   }
   if (render->ds_att.iview) {
      cmd_buffer->state.flush_bits |=
         radv_src_access_flush(cmd_buffer, barrier->src_access_mask, render->ds_att.iview->image);
   }

   radv_stage_flush(cmd_buffer, barrier->src_stage_mask);

   for (uint32_t i = 0; i < render->color_att_count; i++) {
      struct radv_image_view *iview = render->color_att[i].iview;
      if (!iview)
         continue;

      cmd_buffer->state.flush_bits |=
         radv_dst_access_flush(cmd_buffer, barrier->dst_access_mask, iview->image);
   }
   if (render->ds_att.iview) {
      cmd_buffer->state.flush_bits |=
         radv_dst_access_flush(cmd_buffer, barrier->dst_access_mask, render->ds_att.iview->image);
   }

   radv_ace_internal_barrier(cmd_buffer, barrier->src_stage_mask, barrier->dst_stage_mask);
}

static void
radv_handle_image_transition_separate(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                      VkImageLayout src_layout, VkImageLayout dst_layout,
                                      VkImageLayout src_stencil_layout,
                                      VkImageLayout dst_stencil_layout,
                                      uint32_t src_family_index, uint32_t dst_family_index,
                                      const VkImageSubresourceRange *range,
                                      struct radv_sample_locations_state *sample_locs)
{
   /* If we have a stencil layout that's different from depth, we need to
    * perform the stencil transition separately.
    */
   if ((range->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) &&
       (src_layout != src_stencil_layout || dst_layout != dst_stencil_layout)) {
      VkImageSubresourceRange aspect_range = *range;
      /* Depth-only transitions. */
      if (range->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         aspect_range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
         radv_handle_image_transition(cmd_buffer, image, src_layout, dst_layout,
                                      src_family_index, dst_family_index, &aspect_range, sample_locs);
      }

      /* Stencil-only transitions. */
      aspect_range.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
      radv_handle_image_transition(cmd_buffer, image, src_stencil_layout, dst_stencil_layout,
                                   src_family_index, dst_family_index, &aspect_range, sample_locs);
   } else {
      radv_handle_image_transition(cmd_buffer, image, src_layout, dst_layout,
                                   src_family_index, dst_family_index, range, sample_locs);
   }
}

static void
radv_handle_rendering_image_transition(struct radv_cmd_buffer *cmd_buffer,
                                       struct radv_image_view *view,
                                       uint32_t layer_count,
                                       uint32_t view_mask,
                                       VkImageLayout initial_layout,
                                       VkImageLayout initial_stencil_layout,
                                       VkImageLayout final_layout,
                                       VkImageLayout final_stencil_layout,
                                       struct radv_sample_locations_state *sample_locs)
{
   VkImageSubresourceRange range;
   range.aspectMask = view->image->vk.aspects;
   range.baseMipLevel = view->vk.base_mip_level;
   range.levelCount = 1;

   if (view_mask) {
      while (view_mask) {
         int start, count;
         u_bit_scan_consecutive_range(&view_mask, &start, &count);

         range.baseArrayLayer = view->vk.base_array_layer + start;
         range.layerCount = count;

         radv_handle_image_transition_separate(cmd_buffer, view->image, initial_layout, final_layout,
                                               initial_stencil_layout, final_stencil_layout,
                                               0, 0, &range, sample_locs);
      }
   } else {
      range.baseArrayLayer = view->vk.base_array_layer;
      range.layerCount = layer_count;
      radv_handle_image_transition_separate(cmd_buffer, view->image, initial_layout, final_layout,
                                            initial_stencil_layout, final_stencil_layout,
                                            0, 0, &range, sample_locs);
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VkResult result = VK_SUCCESS;

   vk_command_buffer_begin(&cmd_buffer->vk, pBeginInfo);

   memset(&cmd_buffer->state, 0, sizeof(cmd_buffer->state));
   cmd_buffer->state.last_index_type = -1;
   cmd_buffer->state.last_num_instances = -1;
   cmd_buffer->state.last_vertex_offset = -1;
   cmd_buffer->state.last_first_instance = -1;
   cmd_buffer->state.last_drawid = -1;
   cmd_buffer->state.last_subpass_color_count = MAX_RTS;
   cmd_buffer->state.predication_type = -1;
   cmd_buffer->state.last_sx_ps_downconvert = -1;
   cmd_buffer->state.last_sx_blend_opt_epsilon = -1;
   cmd_buffer->state.last_sx_blend_opt_control = -1;
   cmd_buffer->state.mesh_shading = false;
   cmd_buffer->state.last_vrs_rates = -1;
   cmd_buffer->state.last_vrs_rates_sgpr_idx = -1;
   cmd_buffer->state.last_pa_sc_binner_cntl_0 = -1;
   cmd_buffer->usage_flags = pBeginInfo->flags;

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_ALL | RADV_CMD_DIRTY_GUARDBAND;

   if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX7) {
      uint32_t pred_value = 0;
      uint32_t pred_offset;
      if (!radv_cmd_buffer_upload_data(cmd_buffer, 4, &pred_value, &pred_offset))
         vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

      cmd_buffer->mec_inv_pred_emitted = false;
      cmd_buffer->mec_inv_pred_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + pred_offset;
   }

   if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX9 &&
       cmd_buffer->qf == RADV_QUEUE_GENERAL) {
      unsigned num_db = cmd_buffer->device->physical_device->rad_info.max_render_backends;
      unsigned fence_offset, eop_bug_offset;
      void *fence_ptr;

      radv_cmd_buffer_upload_alloc(cmd_buffer, 8, &fence_offset, &fence_ptr);
      memset(fence_ptr, 0, 8);

      cmd_buffer->gfx9_fence_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
      cmd_buffer->gfx9_fence_va += fence_offset;

      radv_emit_clear_data(cmd_buffer, V_370_PFP, cmd_buffer->gfx9_fence_va, 8);

      if (cmd_buffer->device->physical_device->rad_info.gfx_level == GFX9) {
         /* Allocate a buffer for the EOP bug on GFX9. */
         radv_cmd_buffer_upload_alloc(cmd_buffer, 16 * num_db, &eop_bug_offset, &fence_ptr);
         memset(fence_ptr, 0, 16 * num_db);
         cmd_buffer->gfx9_eop_bug_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
         cmd_buffer->gfx9_eop_bug_va += eop_bug_offset;

         radv_emit_clear_data(cmd_buffer, V_370_PFP, cmd_buffer->gfx9_eop_bug_va, 16 * num_db);
      }
   }

   if (cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY &&
       (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {

      char gcbiar_data[VK_GCBIARR_DATA_SIZE(MAX_RTS)];
      const VkRenderingInfo *resume_info =
         vk_get_command_buffer_inheritance_as_rendering_resume(cmd_buffer->vk.level, pBeginInfo,
                                                               gcbiar_data);
      if (resume_info) {
         radv_CmdBeginRendering(commandBuffer, resume_info);
      } else {
         const VkCommandBufferInheritanceRenderingInfo *inheritance_info =
            vk_get_command_buffer_inheritance_rendering_info(cmd_buffer->vk.level, pBeginInfo);

         radv_cmd_buffer_reset_rendering(cmd_buffer);
         struct radv_rendering_state *render = &cmd_buffer->state.render;
         render->active = true;
         render->view_mask = inheritance_info->viewMask;
         render->max_samples = inheritance_info->rasterizationSamples;
         render->color_att_count = inheritance_info->colorAttachmentCount;
         for (uint32_t i = 0; i < render->color_att_count; i++) {
            render->color_att[i] = (struct radv_attachment) {
               .format = inheritance_info->pColorAttachmentFormats[i],
            };
         }
         assert(inheritance_info->depthAttachmentFormat == VK_FORMAT_UNDEFINED ||
                inheritance_info->stencilAttachmentFormat == VK_FORMAT_UNDEFINED ||
                inheritance_info->depthAttachmentFormat ==
                inheritance_info->stencilAttachmentFormat);
         render->ds_att = (struct radv_attachment) { .iview = NULL };
         if (inheritance_info->depthAttachmentFormat != VK_FORMAT_UNDEFINED)
            render->ds_att.format = inheritance_info->depthAttachmentFormat;
         if (inheritance_info->stencilAttachmentFormat != VK_FORMAT_UNDEFINED)
            render->ds_att.format = inheritance_info->stencilAttachmentFormat;
      }

      cmd_buffer->state.inherited_pipeline_statistics =
         pBeginInfo->pInheritanceInfo->pipelineStatistics;

      if (cmd_buffer->state.inherited_pipeline_statistics &
          VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_NGG_QUERY;
   }

   if (unlikely(cmd_buffer->device->trace_bo))
      radv_cmd_buffer_trace_emit(cmd_buffer);

   radv_describe_begin_cmd_buffer(cmd_buffer);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindVertexBuffers2(VkCommandBuffer commandBuffer, uint32_t firstBinding,
                           uint32_t bindingCount, const VkBuffer *pBuffers,
                           const VkDeviceSize *pOffsets, const VkDeviceSize *pSizes,
                           const VkDeviceSize *pStrides)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_vertex_binding *vb = cmd_buffer->vertex_bindings;
   const struct radv_vs_input_state *state = &cmd_buffer->state.dynamic_vs_input;

   /* We have to defer setting up vertex buffer since we need the buffer
    * stride from the pipeline. */

   assert(firstBinding + bindingCount <= MAX_VBS);
   enum amd_gfx_level chip = cmd_buffer->device->physical_device->rad_info.gfx_level;

   if (firstBinding + bindingCount > cmd_buffer->used_vertex_bindings)
      cmd_buffer->used_vertex_bindings = firstBinding + bindingCount;

   uint32_t misaligned_mask_invalid = 0;

   for (uint32_t i = 0; i < bindingCount; i++) {
      RADV_FROM_HANDLE(radv_buffer, buffer, pBuffers[i]);
      uint32_t idx = firstBinding + i;
      VkDeviceSize size = pSizes ? pSizes[i] : 0;
      /* if pStrides=NULL, it shouldn't overwrite the strides specified by CmdSetVertexInputEXT */
      VkDeviceSize stride = pStrides ? pStrides[i] : vb[idx].stride;

      if (!!cmd_buffer->vertex_binding_buffers[idx] != !!buffer ||
          (buffer && ((vb[idx].offset & 0x3) != (pOffsets[i] & 0x3) ||
                      (vb[idx].stride & 0x3) != (stride & 0x3)))) {
         misaligned_mask_invalid |= state->bindings_match_attrib ? BITFIELD_BIT(idx) : 0xffffffff;
      }

      cmd_buffer->vertex_binding_buffers[idx] = buffer;
      vb[idx].offset = pOffsets[i];
      vb[idx].size = size;
      vb[idx].stride = stride;

      uint32_t bit = BITFIELD_BIT(idx);
      if (buffer) {
         radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, cmd_buffer->vertex_binding_buffers[idx]->bo);
         cmd_buffer->state.vbo_bound_mask |= bit;
      } else {
         cmd_buffer->state.vbo_bound_mask &= ~bit;
      }
   }

   if ((chip == GFX6 || chip >= GFX10) && misaligned_mask_invalid) {
      cmd_buffer->state.vbo_misaligned_mask_invalid = misaligned_mask_invalid;
      cmd_buffer->state.vbo_misaligned_mask &= ~misaligned_mask_invalid;
   }

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VERTEX_BUFFER |
                              RADV_CMD_DIRTY_DYNAMIC_VERTEX_INPUT;
}

static uint32_t
vk_to_index_type(VkIndexType type)
{
   switch (type) {
   case VK_INDEX_TYPE_UINT8_EXT:
      return V_028A7C_VGT_INDEX_8;
   case VK_INDEX_TYPE_UINT16:
      return V_028A7C_VGT_INDEX_16;
   case VK_INDEX_TYPE_UINT32:
      return V_028A7C_VGT_INDEX_32;
   default:
      unreachable("invalid index type");
   }
}

uint32_t
radv_get_vgt_index_size(uint32_t type)
{
   uint32_t index_type = G_028A7C_INDEX_TYPE(type);
   switch (index_type) {
   case V_028A7C_VGT_INDEX_8:
      return 1;
   case V_028A7C_VGT_INDEX_16:
      return 2;
   case V_028A7C_VGT_INDEX_32:
      return 4;
   default:
      unreachable("invalid index type");
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                        VkIndexType indexType)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, index_buffer, buffer);

   cmd_buffer->state.index_type = vk_to_index_type(indexType);
   cmd_buffer->state.index_va = radv_buffer_get_va(index_buffer->bo);
   cmd_buffer->state.index_va += index_buffer->offset + offset;

   int index_size = radv_get_vgt_index_size(vk_to_index_type(indexType));
   cmd_buffer->state.max_index_count =
      (vk_buffer_range(&index_buffer->vk, offset, VK_WHOLE_SIZE)) / index_size;
   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_INDEX_BUFFER;
   radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, index_buffer->bo);
}

static void
radv_bind_descriptor_set(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point,
                         struct radv_descriptor_set *set, unsigned idx)
{
   struct radeon_winsys *ws = cmd_buffer->device->ws;

   radv_set_descriptor_set(cmd_buffer, bind_point, set, idx);

   assert(set);
   assert(!(set->header.layout->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR));

   if (!cmd_buffer->device->use_global_bo_list) {
      for (unsigned j = 0; j < set->header.buffer_count; ++j)
         if (set->descriptors[j])
            radv_cs_add_buffer(ws, cmd_buffer->cs, set->descriptors[j]);
   }

   if (set->header.bo)
      radv_cs_add_buffer(ws, cmd_buffer->cs, set->header.bo);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                           VkPipelineLayout _layout, uint32_t firstSet, uint32_t descriptorSetCount,
                           const VkDescriptorSet *pDescriptorSets, uint32_t dynamicOffsetCount,
                           const uint32_t *pDynamicOffsets)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_pipeline_layout, layout, _layout);
   unsigned dyn_idx = 0;

   const bool no_dynamic_bounds =
      cmd_buffer->device->instance->debug_flags & RADV_DEBUG_NO_DYNAMIC_BOUNDS;
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, pipelineBindPoint);

   for (unsigned i = 0; i < descriptorSetCount; ++i) {
      unsigned set_idx = i + firstSet;
      RADV_FROM_HANDLE(radv_descriptor_set, set, pDescriptorSets[i]);

      if (!set)
         continue;

      /* If the set is already bound we only need to update the
       * (potentially changed) dynamic offsets. */
      if (descriptors_state->sets[set_idx] != set ||
          !(descriptors_state->valid & (1u << set_idx))) {
         radv_bind_descriptor_set(cmd_buffer, pipelineBindPoint, set, set_idx);
      }

      for (unsigned j = 0; j < set->header.layout->dynamic_offset_count; ++j, ++dyn_idx) {
         unsigned idx = j + layout->set[i + firstSet].dynamic_offset_start;
         uint32_t *dst = descriptors_state->dynamic_buffers + idx * 4;
         assert(dyn_idx < dynamicOffsetCount);

         struct radv_descriptor_range *range = set->header.dynamic_descriptors + j;

         if (!range->va) {
            memset(dst, 0, 4 * 4);
         } else {
            uint64_t va = range->va + pDynamicOffsets[dyn_idx];
            dst[0] = va;
            dst[1] = S_008F04_BASE_ADDRESS_HI(va >> 32);
            dst[2] = no_dynamic_bounds ? 0xffffffffu : range->size;
            dst[3] = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) | S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
                     S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) | S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W);

            if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX11) {
               dst[3] |= S_008F0C_FORMAT(V_008F0C_GFX11_FORMAT_32_FLOAT) |
                         S_008F0C_OOB_SELECT(V_008F0C_OOB_SELECT_RAW);
            } else if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX10) {
               dst[3] |= S_008F0C_FORMAT(V_008F0C_GFX10_FORMAT_32_FLOAT) |
                         S_008F0C_OOB_SELECT(V_008F0C_OOB_SELECT_RAW) | S_008F0C_RESOURCE_LEVEL(1);
            } else {
               dst[3] |= S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_FLOAT) |
                         S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32);
            }
         }

         cmd_buffer->push_constant_stages |= set->header.layout->dynamic_shader_stages;
      }
   }
}

static bool
radv_init_push_descriptor_set(struct radv_cmd_buffer *cmd_buffer, struct radv_descriptor_set *set,
                              struct radv_descriptor_set_layout *layout,
                              VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, bind_point);
   set->header.size = layout->size;

   if (set->header.layout != layout) {
      if (set->header.layout)
         vk_descriptor_set_layout_unref(&cmd_buffer->device->vk, &set->header.layout->vk);
      vk_descriptor_set_layout_ref(&layout->vk);
      set->header.layout = layout;
   }

   if (descriptors_state->push_set.capacity < set->header.size) {
      size_t new_size = MAX2(set->header.size, 1024);
      new_size = MAX2(new_size, 2 * descriptors_state->push_set.capacity);
      new_size = MIN2(new_size, 96 * MAX_PUSH_DESCRIPTORS);

      free(set->header.mapped_ptr);
      set->header.mapped_ptr = malloc(new_size);

      if (!set->header.mapped_ptr) {
         descriptors_state->push_set.capacity = 0;
         vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return false;
      }

      descriptors_state->push_set.capacity = new_size;
   }

   return true;
}

void
radv_meta_push_descriptor_set(struct radv_cmd_buffer *cmd_buffer,
                              VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout _layout,
                              uint32_t set, uint32_t descriptorWriteCount,
                              const VkWriteDescriptorSet *pDescriptorWrites)
{
   RADV_FROM_HANDLE(radv_pipeline_layout, layout, _layout);
   struct radv_descriptor_set *push_set =
      (struct radv_descriptor_set *)&cmd_buffer->meta_push_descriptors;
   unsigned bo_offset;

   assert(set == 0);
   assert(layout->set[set].layout->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);

   push_set->header.size = layout->set[set].layout->size;
   push_set->header.layout = layout->set[set].layout;

   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, push_set->header.size, &bo_offset,
                                     (void **)&push_set->header.mapped_ptr))
      return;

   push_set->header.va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
   push_set->header.va += bo_offset;

   radv_cmd_update_descriptor_sets(cmd_buffer->device, cmd_buffer,
                                   radv_descriptor_set_to_handle(push_set), descriptorWriteCount,
                                   pDescriptorWrites, 0, NULL);

   radv_set_descriptor_set(cmd_buffer, pipelineBindPoint, push_set, set);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdPushDescriptorSetKHR(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                             VkPipelineLayout _layout, uint32_t set, uint32_t descriptorWriteCount,
                             const VkWriteDescriptorSet *pDescriptorWrites)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_pipeline_layout, layout, _layout);
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, pipelineBindPoint);
   struct radv_descriptor_set *push_set =
      (struct radv_descriptor_set *)&descriptors_state->push_set.set;

   assert(layout->set[set].layout->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);

   if (!radv_init_push_descriptor_set(cmd_buffer, push_set, layout->set[set].layout,
                                      pipelineBindPoint))
      return;

   /* Check that there are no inline uniform block updates when calling vkCmdPushDescriptorSetKHR()
    * because it is invalid, according to Vulkan spec.
    */
   for (int i = 0; i < descriptorWriteCount; i++) {
      ASSERTED const VkWriteDescriptorSet *writeset = &pDescriptorWrites[i];
      assert(writeset->descriptorType != VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK);
   }

   radv_cmd_update_descriptor_sets(cmd_buffer->device, cmd_buffer,
                                   radv_descriptor_set_to_handle(push_set), descriptorWriteCount,
                                   pDescriptorWrites, 0, NULL);

   radv_set_descriptor_set(cmd_buffer, pipelineBindPoint, push_set, set);

   radv_flush_push_descriptors(cmd_buffer, descriptors_state);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer commandBuffer,
                                         VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                         VkPipelineLayout _layout, uint32_t set, const void *pData)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_pipeline_layout, layout, _layout);
   RADV_FROM_HANDLE(radv_descriptor_update_template, templ, descriptorUpdateTemplate);
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, templ->bind_point);
   struct radv_descriptor_set *push_set =
      (struct radv_descriptor_set *)&descriptors_state->push_set.set;

   assert(layout->set[set].layout->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);

   if (!radv_init_push_descriptor_set(cmd_buffer, push_set, layout->set[set].layout,
                                      templ->bind_point))
      return;

   radv_cmd_update_descriptor_set_with_template(cmd_buffer->device, cmd_buffer, push_set,
                                                descriptorUpdateTemplate, pData);

   radv_set_descriptor_set(cmd_buffer, templ->bind_point, push_set, set);

   radv_flush_push_descriptors(cmd_buffer, descriptors_state);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
                      VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size,
                      const void *pValues)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   memcpy(cmd_buffer->push_constants + offset, pValues, size);
   cmd_buffer->push_constant_stages |= stageFlags;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   radv_emit_mip_change_flush_default(cmd_buffer);

   if (cmd_buffer->qf == RADV_QUEUE_GENERAL ||
       cmd_buffer->qf == RADV_QUEUE_COMPUTE) {
      if (cmd_buffer->device->physical_device->rad_info.gfx_level == GFX6)
         cmd_buffer->state.flush_bits |=
            RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_PS_PARTIAL_FLUSH | RADV_CMD_FLAG_WB_L2;

      /* Make sure to sync all pending active queries at the end of
       * command buffer.
       */
      cmd_buffer->state.flush_bits |= cmd_buffer->active_query_flush_bits;

      /* Flush noncoherent images on GFX9+ so we can assume they're clean on the start of a
       * command buffer.
       */
      if (cmd_buffer->state.rb_noncoherent_dirty && can_skip_buffer_l2_flushes(cmd_buffer->device))
         cmd_buffer->state.flush_bits |= radv_src_access_flush(
            cmd_buffer,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            NULL);

      /* Since NGG streamout uses GDS, we need to make GDS idle when
       * we leave the IB, otherwise another process might overwrite
       * it while our shaders are busy.
       */
      if (cmd_buffer->gds_needed)
         cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_PS_PARTIAL_FLUSH;

      /* Finalize the internal compute command stream, if it exists. */
      if (cmd_buffer->ace_internal.cs) {
         VkResult result = radv_ace_internal_finalize(cmd_buffer);
         if (result != VK_SUCCESS)
            return vk_error(cmd_buffer, result);
      }

      si_emit_cache_flush(cmd_buffer);
   }

   /* Make sure CP DMA is idle at the end of IBs because the kernel
    * doesn't wait for it.
    */
   if (cmd_buffer->qf != RADV_QUEUE_VIDEO_DEC)
      si_cp_dma_wait_for_idle(cmd_buffer);

   radv_describe_end_cmd_buffer(cmd_buffer);

   VkResult result = cmd_buffer->device->ws->cs_finalize(cmd_buffer->cs);
   if (result != VK_SUCCESS)
      return vk_error(cmd_buffer, result);

   return vk_command_buffer_end(&cmd_buffer->vk);
}

static void
radv_emit_compute_pipeline(struct radv_cmd_buffer *cmd_buffer,
                           struct radv_compute_pipeline *pipeline)
{
   if (pipeline == cmd_buffer->state.emitted_compute_pipeline)
      return;

   assert(!pipeline->base.ctx_cs.cdw);

   cmd_buffer->state.emitted_compute_pipeline = pipeline;

   radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, pipeline->base.cs.cdw);
   radeon_emit_array(cmd_buffer->cs, pipeline->base.cs.buf, pipeline->base.cs.cdw);

   cmd_buffer->compute_scratch_size_per_wave_needed =
      MAX2(cmd_buffer->compute_scratch_size_per_wave_needed, pipeline->base.scratch_bytes_per_wave);
   cmd_buffer->compute_scratch_waves_wanted =
      MAX2(cmd_buffer->compute_scratch_waves_wanted, pipeline->base.max_waves);

   radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs,
                      pipeline->base.shaders[MESA_SHADER_COMPUTE]->bo);

   if (unlikely(cmd_buffer->device->trace_bo))
      radv_save_pipeline(cmd_buffer, &pipeline->base);
}

static void
radv_mark_descriptor_sets_dirty(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, bind_point);

   descriptors_state->dirty |= descriptors_state->valid;
}

static void
radv_bind_vs_input_state(struct radv_cmd_buffer *cmd_buffer,
                         const struct radv_graphics_pipeline *pipeline)
{
   const struct radv_shader *vs_shader = radv_get_shader(&pipeline->base, MESA_SHADER_VERTEX);
   const struct radv_vs_input_state *src = &pipeline->vs_input_state;

   /* Bind the vertex input state from the pipeline when the VS has a prolog and the state isn't
    * dynamic. This can happen when the pre-rasterization stages and the vertex input state are from
    * two different libraries. Otherwise, if the VS has a prolog, the state is dynamic and there is
    * nothing to bind.
    */
   if (!vs_shader || !vs_shader->info.vs.has_prolog ||
       (pipeline->dynamic_states & RADV_DYNAMIC_VERTEX_INPUT))
      return;

   cmd_buffer->state.dynamic_vs_input = *src;

   if (cmd_buffer->device->physical_device->rad_info.gfx_level == GFX6 ||
       cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX10) {
      cmd_buffer->state.vbo_misaligned_mask = 0;
      cmd_buffer->state.vbo_misaligned_mask_invalid = src->attribute_mask;
   }

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_VERTEX_INPUT;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                     VkPipeline _pipeline)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_pipeline, pipeline, _pipeline);

   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_COMPUTE: {
      struct radv_compute_pipeline *compute_pipeline = radv_pipeline_to_compute(pipeline);

      if (cmd_buffer->state.compute_pipeline == compute_pipeline)
         return;
      radv_mark_descriptor_sets_dirty(cmd_buffer, pipelineBindPoint);

      cmd_buffer->state.compute_pipeline = compute_pipeline;
      cmd_buffer->push_constant_stages |= VK_SHADER_STAGE_COMPUTE_BIT;
      break;
   }
   case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR: {
      struct radv_ray_tracing_pipeline *rt_pipeline = radv_pipeline_to_ray_tracing(pipeline);

      if (cmd_buffer->state.rt_pipeline == rt_pipeline)
         return;
      radv_mark_descriptor_sets_dirty(cmd_buffer, pipelineBindPoint);

      cmd_buffer->state.rt_pipeline = rt_pipeline;
      cmd_buffer->push_constant_stages |= RADV_RT_STAGE_BITS;
      if (rt_pipeline->dynamic_stack_size)
         radv_set_rt_stack_size(cmd_buffer, cmd_buffer->state.rt_stack_size);
      break;
   }
   case VK_PIPELINE_BIND_POINT_GRAPHICS: {
      struct radv_graphics_pipeline *graphics_pipeline = radv_pipeline_to_graphics(pipeline);

      if (cmd_buffer->state.graphics_pipeline == graphics_pipeline)
         return;
      radv_mark_descriptor_sets_dirty(cmd_buffer, pipelineBindPoint);

      bool vtx_emit_count_changed =
         !cmd_buffer->state.graphics_pipeline ||
         cmd_buffer->state.graphics_pipeline->vtx_emit_num != graphics_pipeline->vtx_emit_num ||
         cmd_buffer->state.graphics_pipeline->vtx_base_sgpr != graphics_pipeline->vtx_base_sgpr;
      cmd_buffer->state.graphics_pipeline = graphics_pipeline;

      bool mesh_shading = (graphics_pipeline->active_stages & VK_SHADER_STAGE_MESH_BIT_EXT) > 0;
      if (mesh_shading != cmd_buffer->state.mesh_shading) {
         /* Re-emit VRS state because the combiner is different (vertex vs primitive).
          * Re-emit primitive topology because the mesh shading pipeline clobbered it.
          */
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_FRAGMENT_SHADING_RATE |
                                    RADV_CMD_DIRTY_DYNAMIC_PRIMITIVE_TOPOLOGY;
      }

      cmd_buffer->state.mesh_shading = mesh_shading;
      cmd_buffer->state.has_nggc = graphics_pipeline->has_ngg_culling;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_PIPELINE | RADV_CMD_DIRTY_DYNAMIC_VERTEX_INPUT;
      cmd_buffer->push_constant_stages |= graphics_pipeline->active_stages;

      /* the new vertex shader might not have the same user regs */
      if (vtx_emit_count_changed) {
         cmd_buffer->state.last_first_instance = -1;
         cmd_buffer->state.last_vertex_offset = -1;
         cmd_buffer->state.last_drawid = -1;
      }

      /* Prefetch all pipeline shaders at first draw time. */
      cmd_buffer->state.prefetch_L2_mask |= RADV_PREFETCH_SHADERS;

      if (cmd_buffer->device->physical_device->rad_info.has_vgt_flush_ngg_legacy_bug &&
          cmd_buffer->state.emitted_graphics_pipeline &&
          cmd_buffer->state.emitted_graphics_pipeline->is_ngg &&
          !cmd_buffer->state.graphics_pipeline->is_ngg) {
         /* Transitioning from NGG to legacy GS requires
          * VGT_FLUSH on GFX10 and Navi21. VGT_FLUSH
          * is also emitted at the beginning of IBs when legacy
          * GS ring pointers are set.
          */
         cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VGT_FLUSH;
      }

      if (graphics_pipeline->active_stages & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) {
         if (!(graphics_pipeline->dynamic_states & RADV_DYNAMIC_PATCH_CONTROL_POINTS)) {
            /* Bind the tessellation state from the pipeline when it's not dynamic. */
            struct radv_shader *tcs = graphics_pipeline->base.shaders[MESA_SHADER_TESS_CTRL];

            cmd_buffer->state.tess_num_patches = tcs->info.num_tess_patches;
            cmd_buffer->state.tess_lds_size = tcs->info.tcs.num_lds_blocks;
         }

         /* Always re-emit patch control points/domain origin when a new pipeline with tessellation
          * is bound because a bunch of parameters (user SGPRs, TCS vertices out, ccw, etc) can be
          * different.
          */
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_PATCH_CONTROL_POINTS |
                                    RADV_CMD_DIRTY_DYNAMIC_TESS_DOMAIN_ORIGIN;
      }

      /* Re-emit the vertex buffer descriptors because they are really tied to the pipeline. */
      if (graphics_pipeline->vb_desc_usage_mask) {
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VERTEX_BUFFER;
      }

      /* Re-emit the provoking vertex mode state because the SGPR idx can be different. */
      if (graphics_pipeline->has_pv_sgpr) {
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_PROVOKING_VERTEX_MODE;
      }

      if (cmd_buffer->device->physical_device->rad_info.rbplus_allowed &&
          (!cmd_buffer->state.emitted_graphics_pipeline ||
           cmd_buffer->state.col_format_non_compacted != graphics_pipeline->col_format_non_compacted)) {
         cmd_buffer->state.col_format_non_compacted = graphics_pipeline->col_format_non_compacted;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RBPLUS;
      }

      /* Re-emit the streamout buffers because the SGPR idx can be different and with NGG streamout
       * they always need to be emitted because a buffer size of 0 is used to disable streamout.
       */
      if (graphics_pipeline->has_streamout) {
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_BUFFER;

         if (cmd_buffer->device->physical_device->use_ngg_streamout) {
            cmd_buffer->gds_needed = true;
            cmd_buffer->gds_oa_needed = true;
         }
      }

      /* Re-emit NGG query state when SGPR exists but location potentially changed. */
      if (graphics_pipeline->last_vgt_api_stage_locs[AC_UD_NGG_QUERY_STATE].sgpr_idx != -1)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_NGG_QUERY;

      /* Re-emit the rasterization samples state because the SGPR idx can be different. */
      if (graphics_pipeline->has_dynamic_samples) {
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_RASTERIZATION_SAMPLES;
      }

      radv_bind_dynamic_state(cmd_buffer, &graphics_pipeline->dynamic_state);

      radv_bind_vs_input_state(cmd_buffer, graphics_pipeline);

      if (graphics_pipeline->has_sample_positions)
         cmd_buffer->sample_positions_needed = true;

      if (graphics_pipeline->esgs_ring_size > cmd_buffer->esgs_ring_size_needed)
         cmd_buffer->esgs_ring_size_needed = graphics_pipeline->esgs_ring_size;
      if (graphics_pipeline->gsvs_ring_size > cmd_buffer->gsvs_ring_size_needed)
         cmd_buffer->gsvs_ring_size_needed = graphics_pipeline->gsvs_ring_size;

      cmd_buffer->scratch_size_per_wave_needed =
         MAX2(cmd_buffer->scratch_size_per_wave_needed, pipeline->scratch_bytes_per_wave);
      cmd_buffer->scratch_waves_wanted = MAX2(cmd_buffer->scratch_waves_wanted, pipeline->max_waves);

      if (graphics_pipeline->active_stages & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT)
         cmd_buffer->tess_rings_needed = true;
      if (mesh_shading)
         cmd_buffer->mesh_scratch_ring_needed |=
            pipeline->shaders[MESA_SHADER_MESH]->info.ms.needs_ms_scratch_ring;

      if (graphics_pipeline->active_stages & VK_SHADER_STAGE_TASK_BIT_EXT) {
         if (!cmd_buffer->ace_internal.cs) {
            cmd_buffer->ace_internal.cs = radv_ace_internal_create(cmd_buffer);
            if (!cmd_buffer->ace_internal.cs)
               return;
         }

         cmd_buffer->task_rings_needed = true;
      }
      break;
   }
   default:
      assert(!"invalid bind point");
      break;
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount,
                    const VkViewport *pViewports)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   ASSERTED const uint32_t total_count = firstViewport + viewportCount;

   assert(firstViewport < MAX_VIEWPORTS);
   assert(total_count >= 1 && total_count <= MAX_VIEWPORTS);

   if (state->dynamic.vk.vp.viewport_count < total_count)
      state->dynamic.vk.vp.viewport_count = total_count;

   memcpy(state->dynamic.vk.vp.viewports + firstViewport, pViewports,
          viewportCount * sizeof(*pViewports));
   for (unsigned i = 0; i < viewportCount; i++) {
      radv_get_viewport_xform(&pViewports[i],
                              state->dynamic.hw_vp.xform[i + firstViewport].scale,
                              state->dynamic.hw_vp.xform[i + firstViewport].translate);
   }

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_VIEWPORT | RADV_CMD_DIRTY_GUARDBAND;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount,
                   const VkRect2D *pScissors)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   ASSERTED const uint32_t total_count = firstScissor + scissorCount;

   assert(firstScissor < MAX_SCISSORS);
   assert(total_count >= 1 && total_count <= MAX_SCISSORS);

   if (state->dynamic.vk.vp.scissor_count < total_count)
      state->dynamic.vk.vp.scissor_count = total_count;

   memcpy(state->dynamic.vk.vp.scissors + firstScissor, pScissors,
          scissorCount * sizeof(*pScissors));

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_SCISSOR;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.line.width = lineWidth;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_LINE_WIDTH | RADV_CMD_DIRTY_GUARDBAND;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthBias(VkCommandBuffer commandBuffer, float depthBiasConstantFactor,
                     float depthBiasClamp, float depthBiasSlopeFactor)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.depth_bias.constant = depthBiasConstantFactor;
   state->dynamic.vk.rs.depth_bias.clamp = depthBiasClamp;
   state->dynamic.vk.rs.depth_bias.slope = depthBiasSlopeFactor;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_DEPTH_BIAS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4])
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   memcpy(state->dynamic.vk.cb.blend_constants, blendConstants, sizeof(float) * 4);

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_BLEND_CONSTANTS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthBounds(VkCommandBuffer commandBuffer, float minDepthBounds, float maxDepthBounds)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.bounds_test.min = minDepthBounds;
   state->dynamic.vk.ds.depth.bounds_test.max = maxDepthBounds;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_DEPTH_BOUNDS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilCompareMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
                              uint32_t compareMask)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      state->dynamic.vk.ds.stencil.front.compare_mask = compareMask;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      state->dynamic.vk.ds.stencil.back.compare_mask = compareMask;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilWriteMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
                            uint32_t writeMask)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      state->dynamic.vk.ds.stencil.front.write_mask = writeMask;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      state->dynamic.vk.ds.stencil.back.write_mask = writeMask;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilReference(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
                            uint32_t reference)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
      state->dynamic.vk.ds.stencil.front.reference = reference;
   if (faceMask & VK_STENCIL_FACE_BACK_BIT)
      state->dynamic.vk.ds.stencil.back.reference = reference;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_STENCIL_REFERENCE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDiscardRectangleEXT(VkCommandBuffer commandBuffer, uint32_t firstDiscardRectangle,
                               uint32_t discardRectangleCount, const VkRect2D *pDiscardRectangles)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   ASSERTED const uint32_t total_count = firstDiscardRectangle + discardRectangleCount;

   assert(firstDiscardRectangle < MAX_DISCARD_RECTANGLES);
   assert(total_count >= 1 && total_count <= MAX_DISCARD_RECTANGLES);

   typed_memcpy(&state->dynamic.vk.dr.rectangles[firstDiscardRectangle], pDiscardRectangles,
                discardRectangleCount);

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_DISCARD_RECTANGLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetSampleLocationsEXT(VkCommandBuffer commandBuffer,
                              const VkSampleLocationsInfoEXT *pSampleLocationsInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   assert(pSampleLocationsInfo->sampleLocationsCount <= MAX_SAMPLE_LOCATIONS);

   state->dynamic.sample_location.per_pixel = pSampleLocationsInfo->sampleLocationsPerPixel;
   state->dynamic.sample_location.grid_size = pSampleLocationsInfo->sampleLocationGridSize;
   state->dynamic.sample_location.count = pSampleLocationsInfo->sampleLocationsCount;
   typed_memcpy(&state->dynamic.sample_location.locations[0],
                pSampleLocationsInfo->pSampleLocations, pSampleLocationsInfo->sampleLocationsCount);

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_SAMPLE_LOCATIONS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLineStippleEXT(VkCommandBuffer commandBuffer, uint32_t lineStippleFactor,
                          uint16_t lineStipplePattern)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.line.stipple.factor = lineStippleFactor;
   state->dynamic.vk.rs.line.stipple.pattern = lineStipplePattern;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_LINE_STIPPLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCullMode(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.cull_mode = cullMode;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_CULL_MODE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetFrontFace(VkCommandBuffer commandBuffer, VkFrontFace frontFace)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.front_face = frontFace;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_FRONT_FACE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetPrimitiveTopology(VkCommandBuffer commandBuffer, VkPrimitiveTopology primitiveTopology)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   unsigned primitive_topology = si_translate_prim(primitiveTopology);

   if ((state->dynamic.vk.ia.primitive_topology == V_008958_DI_PT_LINESTRIP) !=
       (primitive_topology == V_008958_DI_PT_LINESTRIP))
      state->dirty |= RADV_CMD_DIRTY_DYNAMIC_LINE_STIPPLE;

   if (radv_prim_is_points_or_lines(state->dynamic.vk.ia.primitive_topology) !=
       radv_prim_is_points_or_lines(primitive_topology))
      state->dirty |= RADV_CMD_DIRTY_GUARDBAND;

   state->dynamic.vk.ia.primitive_topology = primitive_topology;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_PRIMITIVE_TOPOLOGY;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetViewportWithCount(VkCommandBuffer commandBuffer, uint32_t viewportCount,
                             const VkViewport *pViewports)
{
   radv_CmdSetViewport(commandBuffer, 0, viewportCount, pViewports);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetScissorWithCount(VkCommandBuffer commandBuffer, uint32_t scissorCount,
                            const VkRect2D *pScissors)
{
   radv_CmdSetScissor(commandBuffer, 0, scissorCount, pScissors);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable)

{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.test_enable = depthTestEnable;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_DEPTH_TEST_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthWriteEnable(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.write_enable = depthWriteEnable;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_DEPTH_WRITE_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthCompareOp(VkCommandBuffer commandBuffer, VkCompareOp depthCompareOp)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.compare_op = depthCompareOp;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_DEPTH_COMPARE_OP;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthBoundsTestEnable)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.bounds_test.enable = depthBoundsTestEnable;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_DEPTH_BOUNDS_TEST_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilTestEnable(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.stencil.test_enable = stencilTestEnable;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_STENCIL_TEST_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilOp(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
                     VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp,
                     VkCompareOp compareOp)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (faceMask & VK_STENCIL_FACE_FRONT_BIT) {
      state->dynamic.vk.ds.stencil.front.op.fail = failOp;
      state->dynamic.vk.ds.stencil.front.op.pass = passOp;
      state->dynamic.vk.ds.stencil.front.op.depth_fail = depthFailOp;
      state->dynamic.vk.ds.stencil.front.op.compare = compareOp;
   }

   if (faceMask & VK_STENCIL_FACE_BACK_BIT) {
      state->dynamic.vk.ds.stencil.back.op.fail = failOp;
      state->dynamic.vk.ds.stencil.back.op.pass = passOp;
      state->dynamic.vk.ds.stencil.back.op.depth_fail = depthFailOp;
      state->dynamic.vk.ds.stencil.back.op.compare = compareOp;
   }

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_STENCIL_OP;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetFragmentShadingRateKHR(VkCommandBuffer commandBuffer, const VkExtent2D *pFragmentSize,
                                  const VkFragmentShadingRateCombinerOpKHR combinerOps[2])
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.fsr.fragment_size = *pFragmentSize;
   for (unsigned i = 0; i < 2; i++)
      state->dynamic.vk.fsr.combiner_ops[i] = combinerOps[i];

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_FRAGMENT_SHADING_RATE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthBiasEnable(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.depth_bias.enable = depthBiasEnable;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_DEPTH_BIAS_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetPrimitiveRestartEnable(VkCommandBuffer commandBuffer, VkBool32 primitiveRestartEnable)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ia.primitive_restart_enable = primitiveRestartEnable;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_PRIMITIVE_RESTART_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRasterizerDiscardEnable(VkCommandBuffer commandBuffer, VkBool32 rasterizerDiscardEnable)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.rasterizer_discard_enable = rasterizerDiscardEnable;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_RASTERIZER_DISCARD_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetPatchControlPointsEXT(VkCommandBuffer commandBuffer, uint32_t patchControlPoints)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ts.patch_control_points = patchControlPoints;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_PATCH_CONTROL_POINTS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLogicOpEXT(VkCommandBuffer commandBuffer, VkLogicOp logicOp)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   unsigned logic_op = si_translate_blend_logic_op(logicOp);

   state->dynamic.vk.cb.logic_op = logic_op;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_LOGIC_OP;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetColorWriteEnableEXT(VkCommandBuffer commandBuffer, uint32_t attachmentCount,
                               const VkBool32 *pColorWriteEnables)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint8_t color_write_enable = 0;

   assert(attachmentCount <= MAX_RTS);

   for (uint32_t i = 0; i < attachmentCount; i++) {
      if (pColorWriteEnables[i]) {
         color_write_enable |= BITFIELD_BIT(i);
      }
   }

   state->dynamic.vk.cb.color_write_enables = color_write_enable;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_COLOR_WRITE_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetVertexInputEXT(VkCommandBuffer commandBuffer, uint32_t vertexBindingDescriptionCount,
                          const VkVertexInputBindingDescription2EXT *pVertexBindingDescriptions,
                          uint32_t vertexAttributeDescriptionCount,
                          const VkVertexInputAttributeDescription2EXT *pVertexAttributeDescriptions)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radv_vs_input_state *vs_state = &state->dynamic_vs_input;

   const VkVertexInputBindingDescription2EXT *bindings[MAX_VBS];
   for (unsigned i = 0; i < vertexBindingDescriptionCount; i++)
      bindings[pVertexBindingDescriptions[i].binding] = &pVertexBindingDescriptions[i];

   state->vbo_misaligned_mask = 0;
   state->vbo_misaligned_mask_invalid = 0;

   vs_state->attribute_mask = 0;
   vs_state->instance_rate_inputs = 0;
   vs_state->nontrivial_divisors = 0;
   vs_state->zero_divisors = 0;
   vs_state->post_shuffle = 0;
   vs_state->alpha_adjust_lo = 0;
   vs_state->alpha_adjust_hi = 0;
   vs_state->nontrivial_formats = 0;
   vs_state->bindings_match_attrib = true;

   enum amd_gfx_level chip = cmd_buffer->device->physical_device->rad_info.gfx_level;
   enum radeon_family family = cmd_buffer->device->physical_device->rad_info.family;
   const struct ac_vtx_format_info *vtx_info_table = ac_get_vtx_format_info_table(chip, family);

   for (unsigned i = 0; i < vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription2EXT *attrib = &pVertexAttributeDescriptions[i];
      const VkVertexInputBindingDescription2EXT *binding = bindings[attrib->binding];
      unsigned loc = attrib->location;

      vs_state->attribute_mask |= 1u << loc;
      vs_state->bindings[loc] = attrib->binding;
      if (attrib->binding != loc)
         vs_state->bindings_match_attrib = false;
      if (binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) {
         vs_state->instance_rate_inputs |= 1u << loc;
         vs_state->divisors[loc] = binding->divisor;
         if (binding->divisor == 0) {
            vs_state->zero_divisors |= 1u << loc;
         } else if (binding->divisor > 1) {
            vs_state->nontrivial_divisors |= 1u << loc;
         }
      }
      cmd_buffer->vertex_bindings[attrib->binding].stride = binding->stride;
      vs_state->offsets[loc] = attrib->offset;

      enum pipe_format format = vk_format_to_pipe_format(attrib->format);
      const struct ac_vtx_format_info *vtx_info = &vtx_info_table[format];

      vs_state->formats[loc] = format;
      uint8_t align_req_minus_1 = vtx_info->chan_byte_size >= 4 ? 3 : (vtx_info->element_size - 1);
      vs_state->format_align_req_minus_1[loc] = align_req_minus_1;
      vs_state->format_sizes[loc] = vtx_info->element_size;
      vs_state->alpha_adjust_lo |= (vtx_info->alpha_adjust & 0x1) << loc;
      vs_state->alpha_adjust_hi |= (vtx_info->alpha_adjust >> 1) << loc;
      if (G_008F0C_DST_SEL_X(vtx_info->dst_sel) == V_008F0C_SQ_SEL_Z)
         vs_state->post_shuffle |= BITFIELD_BIT(loc);

      if (!(vtx_info->has_hw_format & BITFIELD_BIT(vtx_info->num_channels - 1)))
         vs_state->nontrivial_formats |= BITFIELD_BIT(loc);

      if ((chip == GFX6 || chip >= GFX10) &&
         state->vbo_bound_mask & BITFIELD_BIT(attrib->binding)) {
         if (binding->stride & align_req_minus_1) {
            state->vbo_misaligned_mask |= BITFIELD_BIT(loc);
         } else if ((cmd_buffer->vertex_bindings[attrib->binding].offset + vs_state->offsets[loc]) &
                    align_req_minus_1) {
            state->vbo_misaligned_mask |= BITFIELD_BIT(loc);
         }
      }
   }

   state->dirty |= RADV_CMD_DIRTY_VERTEX_BUFFER | RADV_CMD_DIRTY_DYNAMIC_VERTEX_INPUT;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetPolygonModeEXT(VkCommandBuffer commandBuffer, VkPolygonMode polygonMode)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   unsigned polygon_mode = si_translate_fill(polygonMode);

   if (radv_polygon_mode_is_points_or_lines(state->dynamic.vk.rs.polygon_mode) !=
       radv_polygon_mode_is_points_or_lines(polygon_mode))
      state->dirty |= RADV_CMD_DIRTY_GUARDBAND;

   state->dynamic.vk.rs.polygon_mode = polygon_mode;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_POLYGON_MODE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetTessellationDomainOriginEXT(VkCommandBuffer commandBuffer,
                                       VkTessellationDomainOrigin domainOrigin)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ts.domain_origin = domainOrigin;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_TESS_DOMAIN_ORIGIN;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLogicOpEnableEXT(VkCommandBuffer commandBuffer, VkBool32 logicOpEnable)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.cb.logic_op_enable = logicOpEnable;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_LOGIC_OP_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLineStippleEnableEXT(VkCommandBuffer commandBuffer, VkBool32 stippledLineEnable)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.line.stipple.enable = stippledLineEnable;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_LINE_STIPPLE_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetAlphaToCoverageEnableEXT(VkCommandBuffer commandBuffer, VkBool32 alphaToCoverageEnable)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ms.alpha_to_coverage_enable = alphaToCoverageEnable;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetSampleMaskEXT(VkCommandBuffer commandBuffer, VkSampleCountFlagBits samples,
                         const VkSampleMask *pSampleMask)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ms.sample_mask = pSampleMask[0] & 0xffff;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_SAMPLE_MASK;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthClipEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthClipEnable)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.depth_clip_enable = depthClipEnable;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_DEPTH_CLIP_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetConservativeRasterizationModeEXT(VkCommandBuffer commandBuffer,
                                            VkConservativeRasterizationModeEXT conservativeRasterizationMode)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.conservative_mode = conservativeRasterizationMode;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_CONSERVATIVE_RAST_MODE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthClipNegativeOneToOneEXT(VkCommandBuffer commandBuffer, VkBool32 negativeOneToOne)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.vp.depth_clip_negative_one_to_one = negativeOneToOne;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetProvokingVertexModeEXT(VkCommandBuffer commandBuffer,
                                  VkProvokingVertexModeEXT provokingVertexMode)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.provoking_vertex = provokingVertexMode;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_PROVOKING_VERTEX_MODE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthClampEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthClampEnable)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.depth_clamp_enable = depthClampEnable;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_DEPTH_CLAMP_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetColorWriteMaskEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment,
                             uint32_t attachmentCount, const VkColorComponentFlags *pColorWriteMasks)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   assert(firstAttachment + attachmentCount <= MAX_RTS);

   for (uint32_t i = 0; i < attachmentCount; i++) {
      uint32_t idx = firstAttachment + i;

      state->dynamic.vk.cb.attachments[idx].write_mask = pColorWriteMasks[i];
   }

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_COLOR_WRITE_MASK;

   if (cmd_buffer->device->physical_device->rad_info.rbplus_allowed)
      state->dirty |= RADV_CMD_DIRTY_RBPLUS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetColorBlendEnableEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment,
                               uint32_t attachmentCount, const VkBool32* pColorBlendEnables)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   assert(firstAttachment + attachmentCount <= MAX_RTS);

   for (uint32_t i = 0; i < attachmentCount; i++) {
      uint32_t idx = firstAttachment + i;

      state->dynamic.vk.cb.attachments[idx].blend_enable = pColorBlendEnables[i];
   }

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_COLOR_BLEND_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRasterizationSamplesEXT(VkCommandBuffer commandBuffer,
                                   VkSampleCountFlagBits rasterizationSamples)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ms.rasterization_samples = rasterizationSamples;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_RASTERIZATION_SAMPLES;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLineRasterizationModeEXT(VkCommandBuffer commandBuffer,
                                    VkLineRasterizationModeEXT lineRasterizationMode)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.line.mode = lineRasterizationMode;

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_LINE_RASTERIZATION_MODE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetColorBlendEquationEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment,
                                 uint32_t attachmentCount,
                                 const VkColorBlendEquationEXT *pColorBlendEquations)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   assert(firstAttachment + attachmentCount <= MAX_RTS);
   for (uint32_t i = 0; i < attachmentCount; i++) {
      unsigned idx = firstAttachment + i;

      state->dynamic.vk.cb.attachments[idx].src_color_blend_factor =
         pColorBlendEquations[i].srcColorBlendFactor;
      state->dynamic.vk.cb.attachments[idx].dst_color_blend_factor =
         pColorBlendEquations[i].dstColorBlendFactor;
      state->dynamic.vk.cb.attachments[idx].color_blend_op =
         pColorBlendEquations[i].colorBlendOp;
      state->dynamic.vk.cb.attachments[idx].src_alpha_blend_factor =
         pColorBlendEquations[i].srcAlphaBlendFactor;
      state->dynamic.vk.cb.attachments[idx].dst_alpha_blend_factor =
         pColorBlendEquations[i].dstAlphaBlendFactor;
      state->dynamic.vk.cb.attachments[idx].alpha_blend_op =
         pColorBlendEquations[i].alphaBlendOp;
   }

   state->dirty |= RADV_CMD_DIRTY_DYNAMIC_COLOR_BLEND_EQUATION;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount,
                        const VkCommandBuffer *pCmdBuffers)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, primary, commandBuffer);

   assert(commandBufferCount > 0);

   radv_emit_mip_change_flush_default(primary);

   /* Emit pending flushes on primary prior to executing secondary */
   si_emit_cache_flush(primary);

   /* Make sure CP DMA is idle on primary prior to executing secondary. */
   si_cp_dma_wait_for_idle(primary);

   for (uint32_t i = 0; i < commandBufferCount; i++) {
      RADV_FROM_HANDLE(radv_cmd_buffer, secondary, pCmdBuffers[i]);
      bool allow_ib2 = true;

      if (secondary->device->physical_device->rad_info.gfx_level == GFX7 &&
          secondary->state.uses_draw_indirect_multi) {
         /* Do not launch an IB2 for secondary command buffers that contain
          * DRAW_{INDEX}_INDIRECT_MULTI on GFX7 because it's illegal and hang the GPU.
          */
         allow_ib2 = false;
      }

      if (secondary->qf == RADV_QUEUE_COMPUTE) {
         /* IB2 packets are not supported on compute queues according to PAL. */
         allow_ib2 = false;
      }

      primary->scratch_size_per_wave_needed =
         MAX2(primary->scratch_size_per_wave_needed, secondary->scratch_size_per_wave_needed);
      primary->scratch_waves_wanted =
         MAX2(primary->scratch_waves_wanted, secondary->scratch_waves_wanted);
      primary->compute_scratch_size_per_wave_needed =
         MAX2(primary->compute_scratch_size_per_wave_needed,
              secondary->compute_scratch_size_per_wave_needed);
      primary->compute_scratch_waves_wanted =
         MAX2(primary->compute_scratch_waves_wanted, secondary->compute_scratch_waves_wanted);

      if (secondary->esgs_ring_size_needed > primary->esgs_ring_size_needed)
         primary->esgs_ring_size_needed = secondary->esgs_ring_size_needed;
      if (secondary->gsvs_ring_size_needed > primary->gsvs_ring_size_needed)
         primary->gsvs_ring_size_needed = secondary->gsvs_ring_size_needed;
      if (secondary->tess_rings_needed)
         primary->tess_rings_needed = true;
      if (secondary->task_rings_needed)
         primary->task_rings_needed = true;
      if (secondary->mesh_scratch_ring_needed)
         primary->mesh_scratch_ring_needed = true;
      if (secondary->sample_positions_needed)
         primary->sample_positions_needed = true;
      if (secondary->gds_needed)
         primary->gds_needed = true;
      if (secondary->gds_oa_needed)
         primary->gds_oa_needed = true;

      if (!secondary->state.render.has_image_views && primary->state.render.active &&
          (primary->state.dirty & RADV_CMD_DIRTY_FRAMEBUFFER)) {
         /* Emit the framebuffer state from primary if secondary
          * has been recorded without a framebuffer, otherwise
          * fast color/depth clears can't work.
          */
         radv_emit_fb_mip_change_flush(primary);
         radv_emit_framebuffer_state(primary);
      }

      if (secondary->ace_internal.cs) {
         if (!primary->ace_internal.cs) {
            primary->ace_internal.cs = radv_ace_internal_create(primary);
            if (!primary->ace_internal.cs)
               return;
         }

         struct radeon_cmdbuf *ace_primary = primary->ace_internal.cs;
         struct radeon_cmdbuf *ace_secondary = secondary->ace_internal.cs;

         /* Emit pending flushes on primary prior to executing secondary. */
         radv_ace_internal_cache_flush(primary);

         /* Wait for primary GFX->ACE semaphore, if necessary. */
         if (radv_flush_gfx2ace_semaphore(primary))
            radv_wait_gfx2ace_semaphore(primary);

         /* Execute the secondary compute cmdbuf.
          * Don't use IB2 packets because they are not supported on compute queues.
          */
         primary->device->ws->cs_execute_secondary(ace_primary, ace_secondary, false);
      }

      /* Update pending ACE internal flush bits from the secondary cmdbuf */
      primary->ace_internal.flush_bits |= secondary->ace_internal.flush_bits;

      /* Increment primary semaphore if secondary was dirty.
       * This happens when the secondary cmdbuf has a barrier which
       * isn't consumed by a draw call.
       */
      if (radv_ace_internal_sem_dirty(secondary))
         primary->ace_internal.sem.gfx2ace_value++;

      primary->device->ws->cs_execute_secondary(primary->cs, secondary->cs, allow_ib2);

      /* When the secondary command buffer is compute only we don't
       * need to re-emit the current graphics pipeline.
       */
      if (secondary->state.emitted_graphics_pipeline) {
         primary->state.emitted_graphics_pipeline = secondary->state.emitted_graphics_pipeline;
      }

      /* When the secondary command buffer is graphics only we don't
       * need to re-emit the current compute pipeline.
       */
      if (secondary->state.emitted_compute_pipeline) {
         primary->state.emitted_compute_pipeline = secondary->state.emitted_compute_pipeline;
      }

      if (secondary->state.last_primitive_reset_index) {
         primary->state.last_primitive_reset_index = secondary->state.last_primitive_reset_index;
      }

      if (secondary->state.last_ia_multi_vgt_param) {
         primary->state.last_ia_multi_vgt_param = secondary->state.last_ia_multi_vgt_param;
      }

      if (secondary->state.last_ge_cntl) {
         primary->state.last_ge_cntl = secondary->state.last_ge_cntl;
      }

      primary->state.last_first_instance = secondary->state.last_first_instance;
      primary->state.last_num_instances = secondary->state.last_num_instances;
      primary->state.last_drawid = secondary->state.last_drawid;
      primary->state.last_subpass_color_count = secondary->state.last_subpass_color_count;
      primary->state.last_vertex_offset = secondary->state.last_vertex_offset;
      primary->state.last_sx_ps_downconvert = secondary->state.last_sx_ps_downconvert;
      primary->state.last_sx_blend_opt_epsilon = secondary->state.last_sx_blend_opt_epsilon;
      primary->state.last_sx_blend_opt_control = secondary->state.last_sx_blend_opt_control;

      if (secondary->state.last_index_type != -1) {
         primary->state.last_index_type = secondary->state.last_index_type;
      }

      primary->state.last_vrs_rates = secondary->state.last_vrs_rates;
      primary->state.last_vrs_rates_sgpr_idx = secondary->state.last_vrs_rates_sgpr_idx;

      primary->state.last_pa_sc_binner_cntl_0 = secondary->state.last_pa_sc_binner_cntl_0;
   }

   /* After executing commands from secondary buffers we have to dirty
    * some states.
    */
   primary->state.dirty |= RADV_CMD_DIRTY_PIPELINE | RADV_CMD_DIRTY_INDEX_BUFFER |
                           RADV_CMD_DIRTY_GUARDBAND | RADV_CMD_DIRTY_DYNAMIC_ALL |
                           RADV_CMD_DIRTY_NGG_QUERY;
   radv_mark_descriptor_sets_dirty(primary, VK_PIPELINE_BIND_POINT_GRAPHICS);
   radv_mark_descriptor_sets_dirty(primary, VK_PIPELINE_BIND_POINT_COMPUTE);
}

static void
radv_mark_noncoherent_rb(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_rendering_state *render = &cmd_buffer->state.render;

   /* Have to be conservative in cmdbuffers with inherited attachments. */
   if (!render->has_image_views) {
      cmd_buffer->state.rb_noncoherent_dirty = true;
      return;
   }

   for (uint32_t i = 0; i < render->color_att_count; i++) {
      if (render->color_att[i].iview && !render->color_att[i].iview->image->l2_coherent) {
         cmd_buffer->state.rb_noncoherent_dirty = true;
         return;
      }
   }
   if (render->ds_att.iview && !render->ds_att.iview->image->l2_coherent)
      cmd_buffer->state.rb_noncoherent_dirty = true;
}

static VkImageLayout
attachment_initial_layout(const VkRenderingAttachmentInfo *att)
{
   const VkRenderingAttachmentInitialLayoutInfoMESA *layout_info =
      vk_find_struct_const(att->pNext,
                           RENDERING_ATTACHMENT_INITIAL_LAYOUT_INFO_MESA);
   if (layout_info != NULL)
      return layout_info->initialLayout;

   return att->imageLayout;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo *pRenderingInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   const struct VkSampleLocationsInfoEXT *sample_locs_info =
      vk_find_struct_const(pRenderingInfo->pNext, SAMPLE_LOCATIONS_INFO_EXT);

   struct radv_sample_locations_state sample_locations = { .count = 0, };
   if (sample_locs_info) {
      sample_locations = (struct radv_sample_locations_state) {
         .per_pixel = sample_locs_info->sampleLocationsPerPixel,
         .grid_size = sample_locs_info->sampleLocationGridSize,
         .count = sample_locs_info->sampleLocationsCount,
      };
      typed_memcpy(sample_locations.locations,
                   sample_locs_info->pSampleLocations,
                   sample_locs_info->sampleLocationsCount);
   }

   uint32_t color_samples = 0, ds_samples = 0;
   struct radv_attachment color_att[MAX_RTS];
   for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; i++) {
      const VkRenderingAttachmentInfo *att_info =
         &pRenderingInfo->pColorAttachments[i];

      color_att[i] = (struct radv_attachment) { .iview = NULL };
      if (att_info->imageView == VK_NULL_HANDLE)
         continue;

      VK_FROM_HANDLE(radv_image_view, iview, att_info->imageView);
      color_att[i].format = iview->vk.format;
      color_att[i].iview = iview;
      color_att[i].layout = att_info->imageLayout;
      radv_initialise_color_surface(cmd_buffer->device, &color_att[i].cb, iview);

      if (att_info->resolveMode != VK_RESOLVE_MODE_NONE &&
          att_info->resolveImageView != VK_NULL_HANDLE) {
         color_att[i].resolve_mode = att_info->resolveMode;
         color_att[i].resolve_iview =
            radv_image_view_from_handle(att_info->resolveImageView);
         color_att[i].resolve_layout = att_info->resolveImageLayout;
      }

      color_samples = MAX2(color_samples, color_att[i].iview->vk.image->samples);

      VkImageLayout initial_layout = attachment_initial_layout(att_info);
      if (initial_layout != color_att[i].layout) {
         assert(!(pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT));
         radv_handle_rendering_image_transition(cmd_buffer, color_att[i].iview,
                                                pRenderingInfo->layerCount,
                                                pRenderingInfo->viewMask,
                                                initial_layout, VK_IMAGE_LAYOUT_UNDEFINED,
                                                color_att[i].layout, VK_IMAGE_LAYOUT_UNDEFINED,
                                                &sample_locations);
      }
   }

   struct radv_attachment ds_att = { .iview = NULL };
   const VkRenderingAttachmentInfo *d_att_info = pRenderingInfo->pDepthAttachment;
   const VkRenderingAttachmentInfo *s_att_info = pRenderingInfo->pStencilAttachment;
   if ((d_att_info != NULL && d_att_info->imageView != VK_NULL_HANDLE) ||
       (s_att_info != NULL && s_att_info->imageView != VK_NULL_HANDLE)) {
      struct radv_image_view *d_iview = NULL, *s_iview = NULL;
      struct radv_image_view *d_res_iview = NULL, *s_res_iview = NULL;
      VkImageLayout initial_depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
      VkImageLayout initial_stencil_layout = VK_IMAGE_LAYOUT_UNDEFINED;

      if (d_att_info != NULL && d_att_info->imageView != VK_NULL_HANDLE) {
         d_iview = radv_image_view_from_handle(d_att_info->imageView);
         initial_depth_layout = attachment_initial_layout(d_att_info);
         ds_att.layout = d_att_info->imageLayout;

         if (d_att_info->resolveMode != VK_RESOLVE_MODE_NONE &&
             d_att_info->resolveImageView != VK_NULL_HANDLE) {
            d_res_iview = radv_image_view_from_handle(d_att_info->resolveImageView);
            ds_att.resolve_mode = d_att_info->resolveMode;
            ds_att.resolve_layout = d_att_info->resolveImageLayout;
         }
      }

      if (s_att_info != NULL && s_att_info->imageView != VK_NULL_HANDLE) {
         s_iview = radv_image_view_from_handle(s_att_info->imageView);
         initial_stencil_layout = attachment_initial_layout(s_att_info);
         ds_att.stencil_layout = s_att_info->imageLayout;

         if (s_att_info->resolveMode != VK_RESOLVE_MODE_NONE &&
             s_att_info->resolveImageView != VK_NULL_HANDLE) {
            s_res_iview = radv_image_view_from_handle(s_att_info->resolveImageView);
            ds_att.stencil_resolve_mode = s_att_info->resolveMode;
            ds_att.stencil_resolve_layout = s_att_info->resolveImageLayout;
         }
      }

      assert(d_iview == NULL || s_iview == NULL || d_iview == s_iview);
      ds_att.iview = d_iview ? d_iview : s_iview,
      ds_att.format = ds_att.iview->vk.format;
      radv_initialise_ds_surface(cmd_buffer->device, &ds_att.ds, ds_att.iview);

      assert(d_res_iview == NULL || s_res_iview == NULL || d_res_iview == s_res_iview);
      ds_att.resolve_iview = d_res_iview ? d_res_iview : s_res_iview;

      ds_samples = ds_att.iview->vk.image->samples;

      if (initial_depth_layout != ds_att.layout ||
          initial_stencil_layout != ds_att.stencil_layout) {
         assert(!(pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT));
         radv_handle_rendering_image_transition(cmd_buffer, ds_att.iview,
                                                pRenderingInfo->layerCount,
                                                pRenderingInfo->viewMask,
                                                initial_depth_layout, initial_stencil_layout,
                                                ds_att.layout, ds_att.stencil_layout,
                                                &sample_locations);
      }
   }

   const VkRenderingFragmentShadingRateAttachmentInfoKHR *fsr_info =
      vk_find_struct_const(pRenderingInfo->pNext,
                           RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR);
   struct radv_attachment vrs_att = { .iview = NULL };
   VkExtent2D vrs_texel_size = { .width = 0 };
   if (fsr_info && fsr_info->imageView) {
      VK_FROM_HANDLE(radv_image_view, iview, fsr_info->imageView);
      vrs_att = (struct radv_attachment) {
         .format = iview->vk.format,
         .iview = iview,
         .layout = fsr_info->imageLayout,
      };
      vrs_texel_size = fsr_info->shadingRateAttachmentTexelSize;
   }

   /* Now that we've done any layout transitions which may invoke meta, we can
    * fill out the actual rendering info and set up for the client's render pass.
    */
   radv_cmd_buffer_reset_rendering(cmd_buffer);

   struct radv_rendering_state *render = &cmd_buffer->state.render;
   render->active = true;
   render->has_image_views = true;
   render->area = pRenderingInfo->renderArea;
   render->view_mask = pRenderingInfo->viewMask;
   render->layer_count = pRenderingInfo->layerCount;
   render->color_samples = color_samples;
   render->ds_samples = ds_samples;
   render->max_samples = MAX2(color_samples, ds_samples);
   render->sample_locations = sample_locations;
   render->color_att_count = pRenderingInfo->colorAttachmentCount;
   typed_memcpy(render->color_att, color_att, render->color_att_count);
   render->ds_att = ds_att;
   render->vrs_att = vrs_att;
   render->vrs_texel_size = vrs_texel_size;
   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAMEBUFFER;

   if (cmd_buffer->device->physical_device->rad_info.rbplus_allowed)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RBPLUS;

   if (render->vrs_att.iview && cmd_buffer->device->physical_device->rad_info.gfx_level == GFX10_3) {
      if (render->ds_att.iview) {
         /* When we have a VRS attachment and a depth/stencil attachment, we just need to copy the
          * VRS rates to the HTILE buffer of the attachment.
          */
         struct radv_image_view *ds_iview = render->ds_att.iview;
         struct radv_image *ds_image = ds_iview->image;
         uint32_t level = ds_iview->vk.base_mip_level;

         /* HTILE buffer */
         uint64_t htile_offset = ds_image->bindings[0].offset + ds_image->planes[0].surface.meta_offset +
                                 ds_image->planes[0].surface.u.gfx9.meta_levels[level].offset;
         uint64_t htile_size = ds_image->planes[0].surface.u.gfx9.meta_levels[level].size;
         struct radv_buffer htile_buffer;

         radv_buffer_init(&htile_buffer, cmd_buffer->device, ds_image->bindings[0].bo, htile_size, htile_offset);

         assert(render->area.offset.x + render->area.extent.width <= ds_image->info.width &&
                render->area.offset.x + render->area.extent.height <= ds_image->info.height);

         /* Copy the VRS rates to the HTILE buffer. */
         radv_copy_vrs_htile(cmd_buffer, render->vrs_att.iview->image, &render->area, ds_image,
                             &htile_buffer, true);

         radv_buffer_finish(&htile_buffer);
      } else {
         /* When a subpass uses a VRS attachment without binding a depth/stencil attachment, we have
          * to copy the VRS rates to our internal HTILE buffer.
          */
         struct radv_image *ds_image = radv_cmd_buffer_get_vrs_image(cmd_buffer);

         if (ds_image && render->area.offset.x < ds_image->info.width &&
                         render->area.offset.y < ds_image->info.height) {
            /* HTILE buffer */
            struct radv_buffer *htile_buffer = cmd_buffer->device->vrs.buffer;

            VkRect2D area = render->area;
            area.extent.width = MIN2(area.extent.width, ds_image->info.width - area.offset.x);
            area.extent.height = MIN2(area.extent.height, ds_image->info.height - area.offset.y);

            /* Copy the VRS rates to the HTILE buffer. */
            radv_copy_vrs_htile(cmd_buffer, render->vrs_att.iview->image, &area, ds_image,
                                htile_buffer, false);
         }
      }
   }

   radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 6);
   radeon_set_context_reg(cmd_buffer->cs, R_028204_PA_SC_WINDOW_SCISSOR_TL,
                          S_028204_TL_X(render->area.offset.x) |
                          S_028204_TL_Y(render->area.offset.y));
   radeon_set_context_reg(cmd_buffer->cs, R_028208_PA_SC_WINDOW_SCISSOR_BR,
                          S_028208_BR_X(render->area.offset.x + render->area.extent.width) |
                          S_028208_BR_Y(render->area.offset.y + render->area.extent.height));

   if (!(pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT))
      radv_cmd_buffer_clear_rendering(cmd_buffer, pRenderingInfo);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdEndRendering(VkCommandBuffer commandBuffer)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   radv_mark_noncoherent_rb(cmd_buffer);
   radv_cmd_buffer_resolve_rendering(cmd_buffer);
   radv_cmd_buffer_reset_rendering(cmd_buffer);
}

static void
radv_emit_view_index_per_stage(struct radeon_cmdbuf *cs, struct radv_graphics_pipeline *pipeline,
                               unsigned stage, unsigned index)
{
   struct radv_userdata_info *loc = radv_lookup_user_sgpr(&pipeline->base, stage, AC_UD_VIEW_INDEX);
   if (loc->sgpr_idx == -1)
      return;
   uint32_t base_reg = pipeline->base.user_data_0[stage];
   radeon_set_sh_reg(cs, base_reg + loc->sgpr_idx * 4, index);
}

static void
radv_emit_view_index(struct radv_cmd_buffer *cmd_buffer, unsigned index)
{
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;

   radv_foreach_stage(stage, pipeline->active_stages & ~VK_SHADER_STAGE_TASK_BIT_EXT) {
      radv_emit_view_index_per_stage(cmd_buffer->cs, pipeline, stage, index);
   }
   if (radv_pipeline_has_gs_copy_shader(&pipeline->base)) {
      struct radv_userdata_info *loc =
         &pipeline->base.gs_copy_shader->info.user_sgprs_locs.shader_data[AC_UD_VIEW_INDEX];
      if (loc->sgpr_idx != -1) {
         uint32_t base_reg = R_00B130_SPI_SHADER_USER_DATA_VS_0;
         radeon_set_sh_reg(cmd_buffer->cs, base_reg + loc->sgpr_idx * 4, index);
      }
   }
   if (pipeline->active_stages & VK_SHADER_STAGE_TASK_BIT_EXT) {
      radv_emit_view_index_per_stage(cmd_buffer->ace_internal.cs, pipeline, MESA_SHADER_TASK,
                                     index);
   }
}

/**
 * Emulates predication for MEC using COND_EXEC.
 * When the current command buffer is predicating, emit a COND_EXEC packet
 * so that the MEC skips the next few dwords worth of packets.
 *
 * To make it work with inverted conditional rendering, we allocate
 * space in the upload BO and emit some packets to invert the condition.
 */
static void
radv_cs_emit_compute_predication(struct radv_cmd_state *state, struct radeon_cmdbuf *cs,
                                 uint64_t inv_va, bool *inv_emitted, unsigned dwords)
{
   if (!state->predicating)
      return;

   uint64_t va = state->predication_va;

   if (!state->predication_type) {
      /* Invert the condition the first time it is needed. */
      if (!*inv_emitted) {
         *inv_emitted = true;

         /* Write 1 to the inverted predication VA. */
         radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
         radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_IMM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) |
                            COPY_DATA_WR_CONFIRM);
         radeon_emit(cs, 1);
         radeon_emit(cs, 0);
         radeon_emit(cs, inv_va);
         radeon_emit(cs, inv_va >> 32);

         /* If the API predication VA == 0, skip next command. */
         radeon_emit(cs, PKT3(PKT3_COND_EXEC, 3, 0));
         radeon_emit(cs, va);
         radeon_emit(cs, va >> 32);
         radeon_emit(cs, 0);
         radeon_emit(cs, 6); /* 1x COPY_DATA size */

         /* Write 0 to the new predication VA (when the API condition != 0) */
         radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
         radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_IMM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) |
                            COPY_DATA_WR_CONFIRM);
         radeon_emit(cs, 0);
         radeon_emit(cs, 0);
         radeon_emit(cs, inv_va);
         radeon_emit(cs, inv_va >> 32);
      }

      va = inv_va;
   }

   radeon_emit(cs, PKT3(PKT3_COND_EXEC, 3, 0));
   radeon_emit(cs, va);
   radeon_emit(cs, va >> 32);
   radeon_emit(cs, 0); /* Cache policy */
   radeon_emit(cs, dwords); /* Size of the predicated packet(s) in DWORDs. */
}

static void
radv_cs_emit_draw_packet(struct radv_cmd_buffer *cmd_buffer, uint32_t vertex_count,
                         uint32_t use_opaque)
{
   radeon_emit(cmd_buffer->cs, PKT3(PKT3_DRAW_INDEX_AUTO, 1, cmd_buffer->state.predicating));
   radeon_emit(cmd_buffer->cs, vertex_count);
   radeon_emit(cmd_buffer->cs, V_0287F0_DI_SRC_SEL_AUTO_INDEX | use_opaque);
}

/**
 * Emit a PKT3_DRAW_INDEX_2 packet to render "index_count` vertices.
 *
 * The starting address "index_va" may point anywhere within the index buffer. The number of
 * indexes allocated in the index buffer *past that point* is specified by "max_index_count".
 * Hardware uses this information to return 0 for out-of-bounds reads.
 */
static void
radv_cs_emit_draw_indexed_packet(struct radv_cmd_buffer *cmd_buffer, uint64_t index_va,
                                 uint32_t max_index_count, uint32_t index_count, bool not_eop)
{
   radeon_emit(cmd_buffer->cs, PKT3(PKT3_DRAW_INDEX_2, 4, cmd_buffer->state.predicating));
   radeon_emit(cmd_buffer->cs, max_index_count);
   radeon_emit(cmd_buffer->cs, index_va);
   radeon_emit(cmd_buffer->cs, index_va >> 32);
   radeon_emit(cmd_buffer->cs, index_count);
   /* NOT_EOP allows merging multiple draws into 1 wave, but only user VGPRs
    * can be changed between draws and GS fast launch must be disabled.
    * NOT_EOP doesn't work on gfx9 and older.
    */
   radeon_emit(cmd_buffer->cs, V_0287F0_DI_SRC_SEL_DMA | S_0287F0_NOT_EOP(not_eop));
}

/* MUST inline this function to avoid massive perf loss in drawoverhead */
ALWAYS_INLINE static void
radv_cs_emit_indirect_draw_packet(struct radv_cmd_buffer *cmd_buffer, bool indexed,
                                  uint32_t draw_count, uint64_t count_va, uint32_t stride)
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const unsigned di_src_sel = indexed ? V_0287F0_DI_SRC_SEL_DMA : V_0287F0_DI_SRC_SEL_AUTO_INDEX;
   bool draw_id_enable = cmd_buffer->state.graphics_pipeline->uses_drawid;
   uint32_t base_reg = cmd_buffer->state.graphics_pipeline->vtx_base_sgpr;
   uint32_t vertex_offset_reg, start_instance_reg = 0, draw_id_reg = 0;
   bool predicating = cmd_buffer->state.predicating;
   bool mesh = cmd_buffer->state.mesh_shading;
   assert(base_reg);

   /* just reset draw state for vertex data */
   cmd_buffer->state.last_first_instance = -1;
   cmd_buffer->state.last_num_instances = -1;
   cmd_buffer->state.last_drawid = -1;
   cmd_buffer->state.last_vertex_offset = -1;

   vertex_offset_reg = (base_reg - SI_SH_REG_OFFSET) >> 2;
   if (cmd_buffer->state.graphics_pipeline->uses_baseinstance)
      start_instance_reg = ((base_reg + (draw_id_enable ? 8 : 4)) - SI_SH_REG_OFFSET) >> 2;
   if (draw_id_enable)
      draw_id_reg = ((base_reg + mesh * 12 + 4) - SI_SH_REG_OFFSET) >> 2;

   if (draw_count == 1 && !count_va && !draw_id_enable) {
      radeon_emit(cs,
                  PKT3(indexed ? PKT3_DRAW_INDEX_INDIRECT : PKT3_DRAW_INDIRECT, 3, predicating));
      radeon_emit(cs, 0);
      radeon_emit(cs, vertex_offset_reg);
      radeon_emit(cs, start_instance_reg);
      radeon_emit(cs, di_src_sel);
   } else {
      radeon_emit(cs, PKT3(indexed ? PKT3_DRAW_INDEX_INDIRECT_MULTI : PKT3_DRAW_INDIRECT_MULTI, 8,
                           predicating));
      radeon_emit(cs, 0);
      radeon_emit(cs, vertex_offset_reg);
      radeon_emit(cs, start_instance_reg);
      radeon_emit(cs, draw_id_reg | S_2C3_DRAW_INDEX_ENABLE(draw_id_enable) |
                         S_2C3_COUNT_INDIRECT_ENABLE(!!count_va));
      radeon_emit(cs, draw_count); /* count */
      radeon_emit(cs, count_va);   /* count_addr */
      radeon_emit(cs, count_va >> 32);
      radeon_emit(cs, stride); /* stride */
      radeon_emit(cs, di_src_sel);

      cmd_buffer->state.uses_draw_indirect_multi = true;
   }
}

ALWAYS_INLINE static void
radv_cs_emit_indirect_mesh_draw_packet(struct radv_cmd_buffer *cmd_buffer, uint32_t draw_count,
                                       uint64_t count_va, uint32_t stride)
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   bool draw_id_enable = cmd_buffer->state.graphics_pipeline->uses_drawid;
   uint32_t base_reg = cmd_buffer->state.graphics_pipeline->vtx_base_sgpr;
   bool predicating = cmd_buffer->state.predicating;
   assert(base_reg);

   /* Reset draw state. */
   cmd_buffer->state.last_first_instance = -1;
   cmd_buffer->state.last_num_instances = -1;
   cmd_buffer->state.last_drawid = -1;
   cmd_buffer->state.last_vertex_offset = -1;

   /* Note: firstTask/firstVertex is not supported by this draw packet. */
   uint32_t xyz_dim_reg = (base_reg + 4 - SI_SH_REG_OFFSET) >> 2;
   uint32_t draw_id_reg = (base_reg + 16 - SI_SH_REG_OFFSET) >> 2;

   radeon_emit(cs, PKT3(PKT3_DISPATCH_MESH_INDIRECT_MULTI, 7, predicating));
   radeon_emit(cs, 0); /* data_offset */
   radeon_emit(cs, (xyz_dim_reg & 0xFFFF) | (draw_id_reg << 16));
   radeon_emit(cs,
               S_2C3_DRAW_INDEX_ENABLE(draw_id_enable) | S_2C3_COUNT_INDIRECT_ENABLE(!!count_va));
   radeon_emit(cs, draw_count);
   radeon_emit(cs, count_va & 0xFFFFFFFF);
   radeon_emit(cs, count_va >> 32);
   radeon_emit(cs, stride);
   radeon_emit(cs, V_0287F0_DI_SRC_SEL_AUTO_INDEX);

   cmd_buffer->state.uses_draw_indirect_multi = true;
}

ALWAYS_INLINE static void
radv_cs_emit_dispatch_taskmesh_direct_ace_packet(struct radv_cmd_buffer *cmd_buffer,
                                                 const uint32_t x, const uint32_t y,
                                                 const uint32_t z)
{
   struct radv_pipeline *pipeline = &cmd_buffer->state.graphics_pipeline->base;
   struct radv_shader *compute_shader = radv_get_shader(pipeline, MESA_SHADER_TASK);
   struct radeon_cmdbuf *cs = cmd_buffer->ace_internal.cs;
   const bool predicating = cmd_buffer->state.predicating;
   const uint32_t dispatch_initiator = cmd_buffer->device->dispatch_initiator_task |
                                       S_00B800_CS_W32_EN(compute_shader->info.wave_size == 32);

   struct radv_userdata_info *ring_entry_loc =
      radv_lookup_user_sgpr(pipeline, MESA_SHADER_TASK, AC_UD_TASK_RING_ENTRY);
   assert(ring_entry_loc && ring_entry_loc->sgpr_idx != -1 && ring_entry_loc->num_sgprs == 1);

   uint32_t ring_entry_reg =
      (R_00B900_COMPUTE_USER_DATA_0 + ring_entry_loc->sgpr_idx * 4 - SI_SH_REG_OFFSET) >> 2;

   radeon_emit(cs, PKT3(PKT3_DISPATCH_TASKMESH_DIRECT_ACE, 4, predicating) | PKT3_SHADER_TYPE_S(1));
   radeon_emit(cs, x);
   radeon_emit(cs, y);
   radeon_emit(cs, z);
   radeon_emit(cs, dispatch_initiator);
   radeon_emit(cs, ring_entry_reg & 0xFFFF);
}

ALWAYS_INLINE static void
radv_cs_emit_dispatch_taskmesh_indirect_multi_ace_packet(struct radv_cmd_buffer *cmd_buffer,
                                                         uint64_t data_va, uint32_t draw_count,
                                                         uint64_t count_va, uint32_t stride)
{
   assert((data_va & 0x03) == 0);
   assert((count_va & 0x03) == 0);

   struct radv_pipeline *pipeline = &cmd_buffer->state.graphics_pipeline->base;
   struct radv_shader *compute_shader = radv_get_shader(pipeline, MESA_SHADER_TASK);
   struct radeon_cmdbuf *cs = cmd_buffer->ace_internal.cs;

   const uint32_t count_indirect_enable = !!count_va;
   const uint32_t xyz_dim_enable = compute_shader->info.cs.uses_grid_size;
   const uint32_t draw_id_enable = compute_shader->info.vs.needs_draw_id;
   const uint32_t dispatch_initiator = cmd_buffer->device->dispatch_initiator_task |
                                       S_00B800_CS_W32_EN(compute_shader->info.wave_size == 32);

   const struct radv_userdata_info *ring_entry_loc =
      radv_lookup_user_sgpr(pipeline, MESA_SHADER_TASK, AC_UD_TASK_RING_ENTRY);
   const struct radv_userdata_info *xyz_dim_loc =
      radv_lookup_user_sgpr(pipeline, MESA_SHADER_TASK, AC_UD_CS_GRID_SIZE);
   const struct radv_userdata_info *draw_id_loc =
      radv_lookup_user_sgpr(pipeline, MESA_SHADER_TASK, AC_UD_CS_TASK_DRAW_ID);

   assert(ring_entry_loc->sgpr_idx != -1 && ring_entry_loc->num_sgprs == 1);
   assert(!xyz_dim_enable || (xyz_dim_loc->sgpr_idx != -1 && xyz_dim_loc->num_sgprs == 3));
   assert(!draw_id_enable || (draw_id_loc->sgpr_idx != -1 && draw_id_loc->num_sgprs == 1));

   const uint32_t ring_entry_reg =
      (R_00B900_COMPUTE_USER_DATA_0 + ring_entry_loc->sgpr_idx * 4 - SI_SH_REG_OFFSET) >> 2;
   const uint32_t xyz_dim_reg =
      !xyz_dim_enable
         ? 0
         : (R_00B900_COMPUTE_USER_DATA_0 + xyz_dim_loc->sgpr_idx * 4 - SI_SH_REG_OFFSET) >> 2;
   const uint32_t draw_id_reg =
      !draw_id_enable
         ? 0
         : (R_00B900_COMPUTE_USER_DATA_0 + draw_id_loc->sgpr_idx * 4 - SI_SH_REG_OFFSET) >> 2;

   radeon_emit(cs, PKT3(PKT3_DISPATCH_TASKMESH_INDIRECT_MULTI_ACE, 9, 0) | PKT3_SHADER_TYPE_S(1));
   radeon_emit(cs, data_va);
   radeon_emit(cs, data_va >> 32);
   radeon_emit(cs, ring_entry_reg & 0xFFFF);
   radeon_emit(cs, (count_indirect_enable << 1) | (draw_id_enable << 2) | (xyz_dim_enable << 3) |
                      (draw_id_reg << 16));
   radeon_emit(cs, xyz_dim_reg & 0xFFFF);
   radeon_emit(cs, draw_count);
   radeon_emit(cs, count_va);
   radeon_emit(cs, count_va >> 32);
   radeon_emit(cs, stride);
   radeon_emit(cs, dispatch_initiator);
}

ALWAYS_INLINE static void
radv_cs_emit_dispatch_taskmesh_gfx_packet(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   bool predicating = cmd_buffer->state.predicating;

   struct radv_userdata_info *ring_entry_loc = &pipeline->last_vgt_api_stage_locs[AC_UD_TASK_RING_ENTRY];

   assert(ring_entry_loc->sgpr_idx != -1);

   uint32_t base_reg = cmd_buffer->state.graphics_pipeline->vtx_base_sgpr;
   uint32_t xyz_dim_reg = ((base_reg + 4) - SI_SH_REG_OFFSET) >> 2;
   uint32_t ring_entry_reg = ((base_reg + ring_entry_loc->sgpr_idx * 4) - SI_SH_REG_OFFSET) >> 2;

   radeon_emit(cs, PKT3(PKT3_DISPATCH_TASKMESH_GFX, 2, predicating));
   radeon_emit(cs, (ring_entry_reg << 16) | (xyz_dim_reg & 0xFFFF));
   radeon_emit(cs, 0);
   radeon_emit(cs, V_0287F0_DI_SRC_SEL_AUTO_INDEX);
}

ALWAYS_INLINE static void
radv_emit_userdata_vertex_internal(struct radv_cmd_buffer *cmd_buffer,
                                   const struct radv_draw_info *info, const uint32_t vertex_offset)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const bool uses_baseinstance = state->graphics_pipeline->uses_baseinstance;
   const bool uses_drawid = state->graphics_pipeline->uses_drawid;

   radeon_set_sh_reg_seq(cs, state->graphics_pipeline->vtx_base_sgpr, state->graphics_pipeline->vtx_emit_num);

   radeon_emit(cs, vertex_offset);
   state->last_vertex_offset = vertex_offset;
   if (uses_drawid) {
      radeon_emit(cs, 0);
      state->last_drawid = 0;
   }
   if (uses_baseinstance) {
      radeon_emit(cs, info->first_instance);
      state->last_first_instance = info->first_instance;
   }
}

ALWAYS_INLINE static void
radv_emit_userdata_vertex(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info,
                          const uint32_t vertex_offset)
{
   const struct radv_cmd_state *state = &cmd_buffer->state;
   const bool uses_baseinstance = state->graphics_pipeline->uses_baseinstance;
   const bool uses_drawid = state->graphics_pipeline->uses_drawid;

   if (vertex_offset != state->last_vertex_offset ||
       (uses_drawid && 0 != state->last_drawid) ||
       (uses_baseinstance && info->first_instance != state->last_first_instance))
      radv_emit_userdata_vertex_internal(cmd_buffer, info, vertex_offset);
}

ALWAYS_INLINE static void
radv_emit_userdata_vertex_drawid(struct radv_cmd_buffer *cmd_buffer, uint32_t vertex_offset, uint32_t drawid)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   radeon_set_sh_reg_seq(cs, state->graphics_pipeline->vtx_base_sgpr, 1 + !!drawid);
   radeon_emit(cs, vertex_offset);
   state->last_vertex_offset = vertex_offset;
   if (drawid)
      radeon_emit(cs, drawid);

}

ALWAYS_INLINE static void
radv_emit_userdata_mesh(struct radv_cmd_buffer *cmd_buffer,
                        const uint32_t x, const uint32_t y, const uint32_t z,
                        const uint32_t first_task)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const bool uses_drawid = state->graphics_pipeline->uses_drawid;

   radeon_set_sh_reg_seq(cs, state->graphics_pipeline->vtx_base_sgpr, state->graphics_pipeline->vtx_emit_num);
   radeon_emit(cs, first_task);
   radeon_emit(cs, x);
   radeon_emit(cs, y);
   radeon_emit(cs, z);

   if (uses_drawid) {
      radeon_emit(cs, 0);
      state->last_drawid = 0;
   }
}

ALWAYS_INLINE static void
radv_emit_userdata_mesh_first_task_0_draw_id_0(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   struct radv_graphics_pipeline *pipeline = state->graphics_pipeline;
   const bool uses_drawid = pipeline->uses_drawid;

   radeon_set_sh_reg_seq(cs, pipeline->vtx_base_sgpr, 1);
   radeon_emit(cs, 0);

   if (uses_drawid) {
      radeon_set_sh_reg_seq(cs, pipeline->vtx_base_sgpr + (pipeline->vtx_emit_num - 1) * 4, 1);
      radeon_emit(cs, 0);
   }
}

ALWAYS_INLINE static void
radv_emit_userdata_task_ib_only(struct radv_cmd_buffer *cmd_buffer, uint64_t ib_va,
                                uint32_t ib_stride)
{
   struct radv_pipeline *pipeline = &cmd_buffer->state.graphics_pipeline->base;
   struct radeon_cmdbuf *cs = cmd_buffer->ace_internal.cs;

   struct radv_userdata_info *task_ib_loc =
      radv_lookup_user_sgpr(pipeline, MESA_SHADER_TASK, AC_UD_CS_TASK_IB);

   if (task_ib_loc->sgpr_idx != -1) {
      assert(task_ib_loc->num_sgprs == 3);
      unsigned task_ib_reg = R_00B900_COMPUTE_USER_DATA_0 + task_ib_loc->sgpr_idx * 4;

      radeon_set_sh_reg_seq(cs, task_ib_reg, 3);
      radeon_emit(cs, ib_va);
      radeon_emit(cs, ib_va >> 32);
      radeon_emit(cs, ib_stride);
   }
}

ALWAYS_INLINE static void
radv_emit_userdata_task(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z,
                        uint32_t draw_id, uint32_t first_task, uint64_t ib_va)
{
   struct radv_pipeline *pipeline = &cmd_buffer->state.graphics_pipeline->base;
   struct radeon_cmdbuf *cs = cmd_buffer->ace_internal.cs;

   struct radv_userdata_info *xyz_loc =
      radv_lookup_user_sgpr(pipeline, MESA_SHADER_TASK, AC_UD_CS_GRID_SIZE);
   struct radv_userdata_info *draw_id_loc =
      radv_lookup_user_sgpr(pipeline, MESA_SHADER_TASK, AC_UD_CS_TASK_DRAW_ID);

   if (xyz_loc->sgpr_idx != -1) {
      assert(xyz_loc->num_sgprs == 3);
      unsigned xyz_reg = R_00B900_COMPUTE_USER_DATA_0 + xyz_loc->sgpr_idx * 4;

      radeon_set_sh_reg_seq(cs, xyz_reg, 3);
      radeon_emit(cs, x);
      radeon_emit(cs, y);
      radeon_emit(cs, z);
   }

   if (draw_id_loc->sgpr_idx != -1) {
      assert(draw_id_loc->num_sgprs == 1);
      unsigned draw_id_reg = R_00B900_COMPUTE_USER_DATA_0 + draw_id_loc->sgpr_idx * 4;

      radeon_set_sh_reg_seq(cs, draw_id_reg, 1);
      radeon_emit(cs, draw_id);
   }

   radv_emit_userdata_task_ib_only(cmd_buffer, ib_va, first_task ? 8 : 0);
}

ALWAYS_INLINE static void
radv_emit_draw_packets_indexed(struct radv_cmd_buffer *cmd_buffer,
                               const struct radv_draw_info *info,
                               uint32_t drawCount, const VkMultiDrawIndexedInfoEXT *minfo,
                               uint32_t stride,
                               const int32_t *vertexOffset)

{
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const int index_size = radv_get_vgt_index_size(state->index_type);
   unsigned i = 0;
   const bool uses_drawid = state->graphics_pipeline->uses_drawid;
   const bool can_eop =
      !uses_drawid && cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX10;

   if (uses_drawid) {
      if (vertexOffset) {
         radv_emit_userdata_vertex(cmd_buffer, info, *vertexOffset);
         vk_foreach_multi_draw_indexed(draw, i, minfo, drawCount, stride) {
            const uint32_t remaining_indexes = MAX2(state->max_index_count, draw->firstIndex) - draw->firstIndex;

            /* Skip draw calls with 0-sized index buffers if the GPU can't handle them */
            if (!remaining_indexes &&
                cmd_buffer->device->physical_device->rad_info.has_zero_index_buffer_bug)
               continue;

            if (i > 0)
               radeon_set_sh_reg(cs, state->graphics_pipeline->vtx_base_sgpr + sizeof(uint32_t), i);

            const uint64_t index_va = state->index_va + draw->firstIndex * index_size;

            if (!state->render.view_mask) {
               radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
            } else {
               u_foreach_bit(view, state->render.view_mask) {
                  radv_emit_view_index(cmd_buffer, view);

                  radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
               }
            }
         }
      } else {
         vk_foreach_multi_draw_indexed(draw, i, minfo, drawCount, stride) {
            const uint32_t remaining_indexes = MAX2(state->max_index_count, draw->firstIndex) - draw->firstIndex;

            /* Skip draw calls with 0-sized index buffers if the GPU can't handle them */
            if (!remaining_indexes &&
                cmd_buffer->device->physical_device->rad_info.has_zero_index_buffer_bug)
               continue;

            if (i > 0) {
               if (state->last_vertex_offset != draw->vertexOffset)
                  radv_emit_userdata_vertex_drawid(cmd_buffer, draw->vertexOffset, i);
               else
                  radeon_set_sh_reg(cs, state->graphics_pipeline->vtx_base_sgpr + sizeof(uint32_t), i);
            } else
               radv_emit_userdata_vertex(cmd_buffer, info, draw->vertexOffset);

            const uint64_t index_va = state->index_va + draw->firstIndex * index_size;

            if (!state->render.view_mask) {
               radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
            } else {
               u_foreach_bit(view, state->render.view_mask) {
                  radv_emit_view_index(cmd_buffer, view);

                  radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
               }
            }
         }
      }
      if (drawCount > 1) {
         state->last_drawid = drawCount - 1;
      }
   } else {
      if (vertexOffset) {
         if (cmd_buffer->device->physical_device->rad_info.gfx_level == GFX10) {
            /* GFX10 has a bug that consecutive draw packets with NOT_EOP must not have
             * count == 0 for the last draw that doesn't have NOT_EOP.
             */
            while (drawCount > 1) {
               const VkMultiDrawIndexedInfoEXT *last = (const VkMultiDrawIndexedInfoEXT*)(((const uint8_t*)minfo) + (drawCount - 1) * stride);
               if (last->indexCount)
                  break;
               drawCount--;
            }
         }

         radv_emit_userdata_vertex(cmd_buffer, info, *vertexOffset);
         vk_foreach_multi_draw_indexed(draw, i, minfo, drawCount, stride) {
            const uint32_t remaining_indexes = MAX2(state->max_index_count, draw->firstIndex) - draw->firstIndex;

            /* Skip draw calls with 0-sized index buffers if the GPU can't handle them */
            if (!remaining_indexes &&
                cmd_buffer->device->physical_device->rad_info.has_zero_index_buffer_bug)
               continue;

            const uint64_t index_va = state->index_va + draw->firstIndex * index_size;

            if (!state->render.view_mask) {
               radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, can_eop && i < drawCount - 1);
            } else {
               u_foreach_bit(view, state->render.view_mask) {
                  radv_emit_view_index(cmd_buffer, view);

                  radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
               }
            }
         }
      } else {
         vk_foreach_multi_draw_indexed(draw, i, minfo, drawCount, stride) {
            const uint32_t remaining_indexes = MAX2(state->max_index_count, draw->firstIndex) - draw->firstIndex;

            /* Skip draw calls with 0-sized index buffers if the GPU can't handle them */
            if (!remaining_indexes &&
                cmd_buffer->device->physical_device->rad_info.has_zero_index_buffer_bug)
               continue;

            const VkMultiDrawIndexedInfoEXT *next = (const VkMultiDrawIndexedInfoEXT*)(i < drawCount - 1 ? ((uint8_t*)draw + stride) : NULL);
            const bool offset_changes = next && next->vertexOffset != draw->vertexOffset;
            radv_emit_userdata_vertex(cmd_buffer, info, draw->vertexOffset);

            const uint64_t index_va = state->index_va + draw->firstIndex * index_size;

            if (!state->render.view_mask) {
               radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, can_eop && !offset_changes && i < drawCount - 1);
            } else {
               u_foreach_bit(view, state->render.view_mask) {
                  radv_emit_view_index(cmd_buffer, view);

                  radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
               }
            }
         }
      }
      if (drawCount > 1) {
         state->last_drawid = drawCount - 1;
      }
   }
}

ALWAYS_INLINE static void
radv_emit_direct_draw_packets(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info,
                              uint32_t drawCount, const VkMultiDrawInfoEXT *minfo,
                              uint32_t use_opaque, uint32_t stride)
{
   unsigned i = 0;
   const uint32_t view_mask = cmd_buffer->state.render.view_mask;
   const bool uses_drawid = cmd_buffer->state.graphics_pipeline->uses_drawid;
   uint32_t last_start = 0;

   vk_foreach_multi_draw(draw, i, minfo, drawCount, stride) {
      if (!i)
         radv_emit_userdata_vertex(cmd_buffer, info, draw->firstVertex);
      else
         radv_emit_userdata_vertex_drawid(cmd_buffer, draw->firstVertex, uses_drawid ? i : 0);

      if (!view_mask) {
         radv_cs_emit_draw_packet(cmd_buffer, draw->vertexCount, use_opaque);
      } else {
         u_foreach_bit(view, view_mask) {
            radv_emit_view_index(cmd_buffer, view);
            radv_cs_emit_draw_packet(cmd_buffer, draw->vertexCount, use_opaque);
         }
      }
      last_start = draw->firstVertex;
   }
   if (drawCount > 1) {
       struct radv_cmd_state *state = &cmd_buffer->state;
       state->last_vertex_offset = last_start;
       if (uses_drawid)
           state->last_drawid = drawCount - 1;
   }
}

ALWAYS_INLINE static void
radv_emit_direct_mesh_draw_packet(struct radv_cmd_buffer *cmd_buffer,
                                  uint32_t x, uint32_t y, uint32_t z,
                                  uint32_t first_task)
{
   const uint32_t view_mask = cmd_buffer->state.render.view_mask;
   const uint32_t count = x * y * z;

   radv_emit_userdata_mesh(cmd_buffer, x, y, z, first_task);

   if (!view_mask) {
      radv_cs_emit_draw_packet(cmd_buffer, count, 0);
   } else {
      u_foreach_bit(view, view_mask) {
         radv_emit_view_index(cmd_buffer, view);
         radv_cs_emit_draw_packet(cmd_buffer, count, 0);
      }
   }
}

ALWAYS_INLINE static void
radv_emit_indirect_mesh_draw_packets(struct radv_cmd_buffer *cmd_buffer,
                                     const struct radv_draw_info *info)
{
   const struct radv_cmd_state *state = &cmd_buffer->state;
   struct radeon_winsys *ws = cmd_buffer->device->ws;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const uint64_t va =
      radv_buffer_get_va(info->indirect->bo) + info->indirect->offset + info->indirect_offset;
   const uint64_t count_va = !info->count_buffer
                                ? 0
                                : radv_buffer_get_va(info->count_buffer->bo) +
                                     info->count_buffer->offset + info->count_buffer_offset;

   radv_cs_add_buffer(ws, cs, info->indirect->bo);

   if (info->count_buffer) {
      radv_cs_add_buffer(ws, cs, info->count_buffer->bo);
   }

   radeon_emit(cs, PKT3(PKT3_SET_BASE, 2, 0));
   radeon_emit(cs, 1);
   radeon_emit(cs, va);
   radeon_emit(cs, va >> 32);

   radv_emit_userdata_mesh_first_task_0_draw_id_0(cmd_buffer);

   if (!state->render.view_mask) {
      radv_cs_emit_indirect_mesh_draw_packet(cmd_buffer, info->count, count_va, info->stride);
   } else {
      u_foreach_bit (i, state->render.view_mask) {
         radv_emit_view_index(cmd_buffer, i);
         radv_cs_emit_indirect_mesh_draw_packet(cmd_buffer, info->count, count_va, info->stride);
      }
   }
}

ALWAYS_INLINE static void
radv_emit_direct_taskmesh_draw_packets(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y,
                                       uint32_t z, uint32_t first_task)
{
   uint64_t fake_ib_va = 0;
   const uint32_t view_mask = cmd_buffer->state.render.view_mask;
   const unsigned num_views = MAX2(1, util_bitcount(view_mask));
   unsigned ace_predication_size = num_views * 6; /* DISPATCH_TASKMESH_DIRECT_ACE size */

   if (first_task) {
      /* Pass this as the IB to the shader for emulating firstTask in task shaders. */
      uint32_t fake_ib_dwords[2] = {x, first_task};
      unsigned fake_ib_offset;
      radv_cmd_buffer_upload_data(cmd_buffer, 8, fake_ib_dwords, &fake_ib_offset);
      fake_ib_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + fake_ib_offset;
   }

   radv_emit_userdata_task(cmd_buffer, x, y, z, 0, first_task, fake_ib_va);
   radv_emit_userdata_mesh_first_task_0_draw_id_0(cmd_buffer);
   radv_cs_emit_compute_predication(&cmd_buffer->state, cmd_buffer->ace_internal.cs,
                                    cmd_buffer->mec_inv_pred_va, &cmd_buffer->mec_inv_pred_emitted,
                                    ace_predication_size);

   if (!view_mask) {
      radv_cs_emit_dispatch_taskmesh_direct_ace_packet(cmd_buffer, x, y, z);
      radv_cs_emit_dispatch_taskmesh_gfx_packet(cmd_buffer);
   } else {
      u_foreach_bit (view, view_mask) {
         radv_emit_view_index(cmd_buffer, view);
         radv_cs_emit_dispatch_taskmesh_direct_ace_packet(cmd_buffer, x, y, z);
         radv_cs_emit_dispatch_taskmesh_gfx_packet(cmd_buffer);
      }
   }
}

static void
radv_emit_indirect_taskmesh_draw_packets(struct radv_cmd_buffer *cmd_buffer,
                                         const struct radv_draw_info *info, uint64_t nv_ib_va,
                                         uint32_t nv_ib_stride)
{
   const uint32_t view_mask = cmd_buffer->state.render.view_mask;
   struct radeon_winsys *ws = cmd_buffer->device->ws;
   const unsigned num_views = MAX2(1, util_bitcount(view_mask));
   unsigned ace_predication_size = num_views * 11; /* DISPATCH_TASKMESH_INDIRECT_MULTI_ACE size */
   struct radeon_cmdbuf *ace_cs = cmd_buffer->ace_internal.cs;

   const uint64_t va =
      radv_buffer_get_va(info->indirect->bo) + info->indirect->offset + info->indirect_offset;
   const uint64_t count_va = !info->count_buffer
                                ? 0
                                : radv_buffer_get_va(info->count_buffer->bo) +
                                     info->count_buffer->offset + info->count_buffer_offset;
   uint64_t workaround_cond_va = 0;

   if (num_views > 1)
      ace_predication_size += num_views * 3; /* SET_SH_REG size (view index SGPR) */

   if (count_va)
      radv_cs_add_buffer(ws, cmd_buffer->ace_internal.cs, info->count_buffer->bo);

   if (cmd_buffer->device->physical_device->rad_info.has_taskmesh_indirect0_bug && count_va) {
      /* MEC firmware bug workaround.
       * When the count buffer contains zero, DISPATCH_TASKMESH_INDIRECT_MULTI_ACE hangs.
       * - We must ensure that DISPATCH_TASKMESH_INDIRECT_MULTI_ACE
       *   is only executed when the count buffer contains non-zero.
       * - Furthermore, we must also ensure that each DISPATCH_TASKMESH_GFX packet
       *   has a matching ACE packet.
       *
       * As a workaround:
       * - Reserve a dword in the upload buffer and initialize it to 1 for the workaround
       * - When count != 0, write 0 to the workaround BO and execute the indirect dispatch
       * - When workaround BO != 0 (count was 0), execute an empty direct dispatch
       */

      uint32_t workaround_cond_init = 0;
      uint32_t workaround_cond_off;
      if (!radv_cmd_buffer_upload_data(cmd_buffer, 4, &workaround_cond_init, &workaround_cond_off))
         vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);

      workaround_cond_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + workaround_cond_off;

      radeon_emit(ace_cs, PKT3(PKT3_COPY_DATA, 4, 0));
      radeon_emit(ace_cs, COPY_DATA_SRC_SEL(COPY_DATA_IMM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) |
                             COPY_DATA_WR_CONFIRM);
      radeon_emit(ace_cs, 1);
      radeon_emit(ace_cs, 0);
      radeon_emit(ace_cs, workaround_cond_va);
      radeon_emit(ace_cs, workaround_cond_va >> 32);

      /* 2x COND_EXEC + 1x COPY_DATA + Nx DISPATCH_TASKMESH_DIRECT_ACE */
      ace_predication_size += 2 * 5 + 6 + 6 * num_views;
   }

   radv_cs_add_buffer(ws, cmd_buffer->ace_internal.cs, info->indirect->bo);
   radv_emit_userdata_task_ib_only(cmd_buffer, nv_ib_va, nv_ib_stride);
   radv_emit_userdata_mesh_first_task_0_draw_id_0(cmd_buffer);
   radv_cs_emit_compute_predication(&cmd_buffer->state, cmd_buffer->ace_internal.cs,
                                    cmd_buffer->mec_inv_pred_va, &cmd_buffer->mec_inv_pred_emitted,
                                    ace_predication_size);

   if (workaround_cond_va) {
      radeon_emit(ace_cs, PKT3(PKT3_COND_EXEC, 3, 0));
      radeon_emit(ace_cs, count_va);
      radeon_emit(ace_cs, count_va >> 32);
      radeon_emit(ace_cs, 0);
      radeon_emit(ace_cs,
                  6 + 11 * num_views); /* 1x COPY_DATA + Nx DISPATCH_TASKMESH_INDIRECT_MULTI_ACE */

      radeon_emit(ace_cs, PKT3(PKT3_COPY_DATA, 4, 0));
      radeon_emit(ace_cs, COPY_DATA_SRC_SEL(COPY_DATA_IMM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) |
                             COPY_DATA_WR_CONFIRM);
      radeon_emit(ace_cs, 0);
      radeon_emit(ace_cs, 0);
      radeon_emit(ace_cs, workaround_cond_va);
      radeon_emit(ace_cs, workaround_cond_va >> 32);
   }

   if (!view_mask) {
      radv_cs_emit_dispatch_taskmesh_indirect_multi_ace_packet(cmd_buffer, va, info->count,
                                                               count_va, info->stride);
      radv_cs_emit_dispatch_taskmesh_gfx_packet(cmd_buffer);
   } else {
      u_foreach_bit (view, view_mask) {
         radv_emit_view_index(cmd_buffer, view);
         radv_cs_emit_dispatch_taskmesh_indirect_multi_ace_packet(cmd_buffer, va, info->count,
                                                                  count_va, info->stride);
         radv_cs_emit_dispatch_taskmesh_gfx_packet(cmd_buffer);
      }
   }

   if (workaround_cond_va) {
      radeon_emit(ace_cs, PKT3(PKT3_COND_EXEC, 3, 0));
      radeon_emit(ace_cs, workaround_cond_va);
      radeon_emit(ace_cs, workaround_cond_va >> 32);
      radeon_emit(ace_cs, 0);
      radeon_emit(ace_cs, 6 * num_views); /* Nx DISPATCH_TASKMESH_DIRECT_ACE */

      for (unsigned v = 0; v < num_views; ++v) {
         radv_cs_emit_dispatch_taskmesh_direct_ace_packet(cmd_buffer, 0, 0, 0);
      }
   }
}

static void
radv_emit_indirect_draw_packets(struct radv_cmd_buffer *cmd_buffer,
                                const struct radv_draw_info *info)
{
   const struct radv_cmd_state *state = &cmd_buffer->state;
   struct radeon_winsys *ws = cmd_buffer->device->ws;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const uint64_t va =
      radv_buffer_get_va(info->indirect->bo) + info->indirect->offset + info->indirect_offset;
   const uint64_t count_va = info->count_buffer
                                ? radv_buffer_get_va(info->count_buffer->bo) +
                                     info->count_buffer->offset + info->count_buffer_offset
                                : 0;

   radv_cs_add_buffer(ws, cs, info->indirect->bo);

   radeon_emit(cs, PKT3(PKT3_SET_BASE, 2, 0));
   radeon_emit(cs, 1);
   radeon_emit(cs, va);
   radeon_emit(cs, va >> 32);

   if (info->count_buffer) {
      radv_cs_add_buffer(ws, cs, info->count_buffer->bo);
   }

   if (!state->render.view_mask) {
      radv_cs_emit_indirect_draw_packet(cmd_buffer, info->indexed, info->count, count_va,
                                        info->stride);
   } else {
      u_foreach_bit(i, state->render.view_mask)
      {
         radv_emit_view_index(cmd_buffer, i);

         radv_cs_emit_indirect_draw_packet(cmd_buffer, info->indexed, info->count, count_va,
                                           info->stride);
      }
   }
}

/*
 * Vega and raven have a bug which triggers if there are multiple context
 * register contexts active at the same time with different scissor values.
 *
 * There are two possible workarounds:
 * 1) Wait for PS_PARTIAL_FLUSH every time the scissor is changed. That way
 *    there is only ever 1 active set of scissor values at the same time.
 *
 * 2) Whenever the hardware switches contexts we have to set the scissor
 *    registers again even if it is a noop. That way the new context gets
 *    the correct scissor values.
 *
 * This implements option 2. radv_need_late_scissor_emission needs to
 * return true on affected HW if radv_emit_all_graphics_states sets
 * any context registers.
 */
static bool
radv_need_late_scissor_emission(struct radv_cmd_buffer *cmd_buffer,
                                const struct radv_draw_info *info)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (!cmd_buffer->device->physical_device->rad_info.has_gfx9_scissor_bug)
      return false;

   if (cmd_buffer->state.context_roll_without_scissor_emitted || info->strmout_buffer)
      return true;

   uint64_t used_states =
      cmd_buffer->state.graphics_pipeline->needed_dynamic_state | ~RADV_CMD_DIRTY_DYNAMIC_ALL;

   /* Index, vertex and streamout buffers don't change context regs, and
    * pipeline is already handled.
    */
   used_states &= ~(RADV_CMD_DIRTY_INDEX_BUFFER | RADV_CMD_DIRTY_VERTEX_BUFFER |
                    RADV_CMD_DIRTY_DYNAMIC_VERTEX_INPUT | RADV_CMD_DIRTY_STREAMOUT_BUFFER |
                    RADV_CMD_DIRTY_PIPELINE);

   if (cmd_buffer->state.dirty & used_states)
      return true;

   uint32_t primitive_reset_index = radv_get_primitive_reset_index(cmd_buffer);

   if (info->indexed && state->dynamic.vk.ia.primitive_restart_enable &&
       primitive_reset_index != state->last_primitive_reset_index)
      return true;

   return false;
}

ALWAYS_INLINE static uint32_t
radv_get_ngg_culling_settings(struct radv_cmd_buffer *cmd_buffer, bool vp_y_inverted)
{
   const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   /* Disable shader culling entirely when conservative overestimate is used.
    * The face culling algorithm can delete very tiny triangles (even if unintended).
    */
   if (d->vk.rs.conservative_mode == VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT)
      return radv_nggc_none;

   /* With graphics pipeline library, NGG culling is unconditionally compiled into shaders
    * because we don't know the primitive topology at compile time, so we should
    * disable it dynamically for points or lines.
    */
   const unsigned num_vertices_per_prim = si_conv_prim_to_gs_out(d->vk.ia.primitive_topology) + 1;
   if (num_vertices_per_prim != 3)
      return radv_nggc_none;

   /* Cull every triangle when rasterizer discard is enabled. */
   if (d->vk.rs.rasterizer_discard_enable)
      return radv_nggc_front_face | radv_nggc_back_face;

   uint32_t nggc_settings = radv_nggc_none;

   /* The culling code needs to know whether face is CW or CCW. */
   bool ccw = d->vk.rs.front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE;

   /* Take inverted viewport into account. */
   ccw ^= vp_y_inverted;

   if (ccw)
      nggc_settings |= radv_nggc_face_is_ccw;

   /* Face culling settings. */
   if (d->vk.rs.cull_mode & VK_CULL_MODE_FRONT_BIT)
      nggc_settings |= radv_nggc_front_face;
   if (d->vk.rs.cull_mode & VK_CULL_MODE_BACK_BIT)
      nggc_settings |= radv_nggc_back_face;

   /* Small primitive culling assumes a sample position at (0.5, 0.5)
    * so don't enable it with user sample locations.
    */
   if (!pipeline->uses_user_sample_locations) {
      nggc_settings |= radv_nggc_small_primitives;

      /* small_prim_precision = num_samples / 2^subpixel_bits
       * num_samples is also always a power of two, so the small prim precision can only be
       * a power of two between 2^-2 and 2^-6, therefore it's enough to remember the exponent.
       */
      unsigned rasterization_samples = radv_get_rasterization_samples(cmd_buffer);
      unsigned subpixel_bits = 256;
      int32_t small_prim_precision_log2 = util_logbase2(rasterization_samples) - util_logbase2(subpixel_bits);
      nggc_settings |= ((uint32_t) small_prim_precision_log2 << 24u);
   }

   return nggc_settings;
}

static void
radv_emit_ngg_culling_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   const unsigned stage = pipeline->last_vgt_api_stage;
   const uint32_t base_reg = pipeline->base.user_data_0[stage];

   /* Get viewport transform. */
   float vp_scale[2], vp_translate[2];
   memcpy(vp_scale, cmd_buffer->state.dynamic.hw_vp.xform[0].scale, 2 * sizeof(float));
   memcpy(vp_translate, cmd_buffer->state.dynamic.hw_vp.xform[0].translate, 2 * sizeof(float));
   bool vp_y_inverted = (-vp_scale[1] + vp_translate[1]) > (vp_scale[1] + vp_translate[1]);

   /* Get current culling settings. */
   uint32_t nggc_settings = radv_get_ngg_culling_settings(cmd_buffer, vp_y_inverted);

   if (cmd_buffer->state.dirty &
       (RADV_CMD_DIRTY_PIPELINE | RADV_CMD_DIRTY_DYNAMIC_VIEWPORT | RADV_CMD_DIRTY_DYNAMIC_RASTERIZATION_SAMPLES)) {
      /* Correction for inverted Y */
      if (vp_y_inverted) {
         vp_scale[1] = -vp_scale[1];
         vp_translate[1] = -vp_translate[1];
      }

      /* Correction for number of samples per pixel. */
      for (unsigned i = 0; i < 2; ++i) {
         vp_scale[i] *= (float) cmd_buffer->state.dynamic.vk.ms.rasterization_samples;
         vp_translate[i] *= (float) cmd_buffer->state.dynamic.vk.ms.rasterization_samples;
      }

      uint32_t vp_reg_values[4] = {fui(vp_scale[0]), fui(vp_scale[1]), fui(vp_translate[0]), fui(vp_translate[1])};
      const int8_t vp_sgpr_idx = pipeline->last_vgt_api_stage_locs[AC_UD_NGG_VIEWPORT].sgpr_idx;
      assert(vp_sgpr_idx != -1);
      radeon_set_sh_reg_seq(cmd_buffer->cs, base_reg + vp_sgpr_idx * 4, 4);
      radeon_emit_array(cmd_buffer->cs, vp_reg_values, 4);
   }

   const int8_t nggc_sgpr_idx =
      pipeline->last_vgt_api_stage_locs[AC_UD_NGG_CULLING_SETTINGS].sgpr_idx;
   assert(nggc_sgpr_idx != -1);

   radeon_set_sh_reg(cmd_buffer->cs, base_reg + nggc_sgpr_idx * 4, nggc_settings);
}

static void
radv_emit_all_graphics_states(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info,
                              bool pipeline_is_dirty)
{
   const struct radv_device *device = cmd_buffer->device;
   bool late_scissor_emission;

   if (cmd_buffer->state.graphics_pipeline->base.shaders[MESA_SHADER_FRAGMENT]->info.ps.has_epilog) {
      struct radv_shader_part *ps_epilog = NULL;

      if (cmd_buffer->state.graphics_pipeline->ps_epilog) {
         ps_epilog = cmd_buffer->state.graphics_pipeline->ps_epilog;
      } else if ((cmd_buffer->state.emitted_graphics_pipeline != cmd_buffer->state.graphics_pipeline ||
                 (cmd_buffer->state.dirty & (RADV_CMD_DIRTY_DYNAMIC_COLOR_WRITE_MASK |
                                             RADV_CMD_DIRTY_DYNAMIC_COLOR_BLEND_ENABLE |
                                             RADV_CMD_DIRTY_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE |
                                             RADV_CMD_DIRTY_DYNAMIC_COLOR_BLEND_EQUATION)))) {
         ps_epilog = lookup_ps_epilog(cmd_buffer);
         if (!ps_epilog) {
            vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
            return;
         }

         cmd_buffer->state.col_format_non_compacted = ps_epilog->spi_shader_col_format;
         if (cmd_buffer->state.graphics_pipeline->need_null_export_workaround &&
             !cmd_buffer->state.col_format_non_compacted)
            cmd_buffer->state.col_format_non_compacted = V_028714_SPI_SHADER_32_R;
         if (device->physical_device->rad_info.rbplus_allowed)
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RBPLUS;
      }

      if (ps_epilog)
         radv_emit_ps_epilog_state(cmd_buffer, ps_epilog, pipeline_is_dirty);
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_RBPLUS)
      radv_emit_rbplus_state(cmd_buffer);

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_NGG_QUERY) {
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_NGG_QUERY;
      radv_flush_ngg_query_state(cmd_buffer);
   }

   if ((cmd_buffer->state.dirty &
        (RADV_CMD_DIRTY_PIPELINE | RADV_CMD_DIRTY_DYNAMIC_CULL_MODE |
         RADV_CMD_DIRTY_DYNAMIC_FRONT_FACE | RADV_CMD_DIRTY_DYNAMIC_RASTERIZER_DISCARD_ENABLE |
         RADV_CMD_DIRTY_DYNAMIC_VIEWPORT | RADV_CMD_DIRTY_DYNAMIC_CONSERVATIVE_RAST_MODE |
         RADV_CMD_DIRTY_DYNAMIC_RASTERIZATION_SAMPLES | RADV_CMD_DIRTY_DYNAMIC_PRIMITIVE_TOPOLOGY)) &&
       cmd_buffer->state.has_nggc)
      radv_emit_ngg_culling_state(cmd_buffer);

   if ((cmd_buffer->state.dirty & (RADV_CMD_DIRTY_DYNAMIC_COLOR_WRITE_MASK |
                                   RADV_CMD_DIRTY_DYNAMIC_RASTERIZATION_SAMPLES |
                                   RADV_CMD_DIRTY_DYNAMIC_LINE_RASTERIZATION_MODE)) ||
       cmd_buffer->state.emitted_graphics_pipeline != cmd_buffer->state.graphics_pipeline)
      radv_emit_binning_state(cmd_buffer);

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_PIPELINE)
      radv_emit_graphics_pipeline(cmd_buffer);

   /* This should be before the cmd_buffer->state.dirty is cleared
    * (excluding RADV_CMD_DIRTY_PIPELINE) and after
    * cmd_buffer->state.context_roll_without_scissor_emitted is set. */
   late_scissor_emission = radv_need_late_scissor_emission(cmd_buffer, info);

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_FRAMEBUFFER)
      radv_emit_framebuffer_state(cmd_buffer);

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_GUARDBAND)
      radv_emit_guardband_state(cmd_buffer);

   if (info->indexed) {
      if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_INDEX_BUFFER)
         radv_emit_index_buffer(cmd_buffer, info->indirect);
   } else {
      /* On GFX7 and later, non-indexed draws overwrite VGT_INDEX_TYPE,
       * so the state must be re-emitted before the next indexed
       * draw.
       */
      if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX7) {
         cmd_buffer->state.last_index_type = -1;
      }
   }

   if (cmd_buffer->device->force_vrs != RADV_FORCE_VRS_1x1) {
      struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
      uint64_t dynamic_states =
         cmd_buffer->state.dirty & cmd_buffer->state.emitted_graphics_pipeline->needed_dynamic_state;

      if ((dynamic_states & RADV_CMD_DIRTY_DYNAMIC_FRAGMENT_SHADING_RATE) &&
          d->vk.fsr.fragment_size.width == 1 && d->vk.fsr.fragment_size.height == 1 &&
          d->vk.fsr.combiner_ops[0] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR &&
          d->vk.fsr.combiner_ops[1] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR) {
         /* When per-vertex VRS is forced and the dynamic fragment shading rate is a no-op, ignore
          * it. This is needed for vkd3d-proton because it always declares per-draw VRS as dynamic.
          */
         cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_DYNAMIC_FRAGMENT_SHADING_RATE;
      }
   }

   /* Pre-compute some tessellation info that depend on the number of patch control points when the
    * bound pipeline declared this state as dynamic.
    */
   if (cmd_buffer->state.graphics_pipeline->dynamic_states & RADV_DYNAMIC_PATCH_CONTROL_POINTS) {
      uint64_t dynamic_states =
         cmd_buffer->state.dirty & cmd_buffer->state.emitted_graphics_pipeline->needed_dynamic_state;

      if (dynamic_states & RADV_CMD_DIRTY_DYNAMIC_PATCH_CONTROL_POINTS) {
         const struct radv_physical_device *pdevice = device->physical_device;
         const struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
         const struct radv_shader *tcs = pipeline->base.shaders[MESA_SHADER_TESS_CTRL];
         const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

         /* Compute the number of patches and emit the context register. */
         cmd_buffer->state.tess_num_patches = get_tcs_num_patches(
            d->vk.ts.patch_control_points, tcs->info.tcs.tcs_vertices_out,
            tcs->info.tcs.num_linked_inputs, tcs->info.tcs.num_linked_outputs,
            tcs->info.tcs.num_linked_patch_outputs, pdevice->hs.tess_offchip_block_dw_size,
            pdevice->rad_info.gfx_level, pdevice->rad_info.family);

         /* Compute the LDS size and emit the shader register. */
         cmd_buffer->state.tess_lds_size = calculate_tess_lds_size(
            pdevice->rad_info.gfx_level, d->vk.ts.patch_control_points,
            tcs->info.tcs.tcs_vertices_out, tcs->info.tcs.num_linked_inputs,
            cmd_buffer->state.tess_num_patches, tcs->info.tcs.num_linked_outputs,
            tcs->info.tcs.num_linked_patch_outputs);
      }
   }

   radv_cmd_buffer_flush_dynamic_state(cmd_buffer, pipeline_is_dirty);

   radv_emit_draw_registers(cmd_buffer, info);

   if (late_scissor_emission)
      radv_emit_scissor(cmd_buffer);
}

/* MUST inline this function to avoid massive perf loss in drawoverhead */
ALWAYS_INLINE static bool
radv_before_draw(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info, uint32_t drawCount)
{
   const bool has_prefetch = cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX7;
   const bool pipeline_is_dirty = (cmd_buffer->state.dirty & RADV_CMD_DIRTY_PIPELINE) &&
                                  cmd_buffer->state.graphics_pipeline != cmd_buffer->state.emitted_graphics_pipeline;

   ASSERTED const unsigned cdw_max =
      radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 4096 + 128 * (drawCount - 1));

   if (likely(!info->indirect)) {
      /* GFX6-GFX7 treat instance_count==0 as instance_count==1. There is
       * no workaround for indirect draws, but we can at least skip
       * direct draws.
       */
      if (unlikely(!info->instance_count))
         return false;

      /* Handle count == 0. */
      if (unlikely(!info->count && !info->strmout_buffer))
         return false;
   }

   /* Need to apply this workaround early as it can set flush flags. */
   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_FRAMEBUFFER)
      radv_emit_fb_mip_change_flush(cmd_buffer);

   /* Use optimal packet order based on whether we need to sync the
    * pipeline.
    */
   if (cmd_buffer->state.flush_bits &
       (RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB |
        RADV_CMD_FLAG_PS_PARTIAL_FLUSH | RADV_CMD_FLAG_CS_PARTIAL_FLUSH)) {
      /* If we have to wait for idle, set all states first, so that
       * all SET packets are processed in parallel with previous draw
       * calls. Then upload descriptors, set shader pointers, and
       * draw, and prefetch at the end. This ensures that the time
       * the CUs are idle is very short. (there are only SET_SH
       * packets between the wait and the draw)
       */
      radv_emit_all_graphics_states(cmd_buffer, info, pipeline_is_dirty);
      si_emit_cache_flush(cmd_buffer);
      /* <-- CUs are idle here --> */

      radv_upload_graphics_shader_descriptors(cmd_buffer);
   } else {
      const bool need_prefetch = has_prefetch && cmd_buffer->state.prefetch_L2_mask;

      /* If we don't wait for idle, start prefetches first, then set
       * states, and draw at the end.
       */
      si_emit_cache_flush(cmd_buffer);

      if (need_prefetch) {
         /* Only prefetch the vertex shader and VBO descriptors
          * in order to start the draw as soon as possible.
          */
         radv_emit_prefetch_L2(cmd_buffer, cmd_buffer->state.graphics_pipeline, true);
      }

      radv_upload_graphics_shader_descriptors(cmd_buffer);

      radv_emit_all_graphics_states(cmd_buffer, info, pipeline_is_dirty);
   }

   radv_describe_draw(cmd_buffer);
   if (likely(!info->indirect)) {
      struct radv_cmd_state *state = &cmd_buffer->state;
      struct radeon_cmdbuf *cs = cmd_buffer->cs;
      assert(state->graphics_pipeline->vtx_base_sgpr);
      if (state->last_num_instances != info->instance_count ||
          cmd_buffer->device->uses_shadow_regs) {
         radeon_emit(cs, PKT3(PKT3_NUM_INSTANCES, 0, false));
         radeon_emit(cs, info->instance_count);
         state->last_num_instances = info->instance_count;
      }
   }
   assert(cmd_buffer->cs->cdw <= cdw_max);

   return true;
}

ALWAYS_INLINE static bool
radv_before_taskmesh_draw(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info,
                          uint32_t drawCount)
{
   /* For direct draws, this makes sure we don't draw anything.
    * For indirect draws, this is necessary to prevent a GPU hang (on MEC version < 100).
    */
   if (unlikely(!info->count))
      return false;

   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radv_physical_device *pdevice = cmd_buffer->device->physical_device;
   struct radeon_cmdbuf *ace_cs = cmd_buffer->ace_internal.cs;
   struct radv_shader *task_shader = radv_get_shader(&pipeline->base, MESA_SHADER_TASK);

   assert(!task_shader || ace_cs);

   const VkShaderStageFlags stages = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT | (task_shader ? VK_SHADER_STAGE_TASK_BIT_EXT : 0);
   const bool pipeline_is_dirty =
      cmd_buffer->state.dirty & RADV_CMD_DIRTY_PIPELINE &&
      cmd_buffer->state.graphics_pipeline != cmd_buffer->state.emitted_graphics_pipeline;
   const bool need_task_semaphore = task_shader && radv_flush_gfx2ace_semaphore(cmd_buffer);

   ASSERTED const unsigned cdw_max =
      radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 4096 + 128 * (drawCount - 1));
   ASSERTED const unsigned ace_cdw_max = !ace_cs ? 0 :
      radeon_check_space(cmd_buffer->device->ws, ace_cs, 4096 + 128 * (drawCount - 1));

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_FRAMEBUFFER)
      radv_emit_fb_mip_change_flush(cmd_buffer);

   radv_emit_all_graphics_states(cmd_buffer, info, pipeline_is_dirty);
   if (task_shader && pipeline_is_dirty) {
      radv_pipeline_emit_hw_cs(pdevice, ace_cs, task_shader);
      radv_pipeline_emit_compute_state(pdevice, ace_cs, task_shader);
   }

   si_emit_cache_flush(cmd_buffer);

   if (task_shader) {
      radv_ace_internal_cache_flush(cmd_buffer);

      if (need_task_semaphore) {
         radv_wait_gfx2ace_semaphore(cmd_buffer);
      }
   }

   radv_flush_descriptors(cmd_buffer, stages, &pipeline->base, VK_PIPELINE_BIND_POINT_GRAPHICS);

   const VkShaderStageFlags pc_stages =
      radv_must_flush_constants(cmd_buffer, &pipeline->base, stages);
   if (pc_stages)
      radv_flush_constants(cmd_buffer, pc_stages, &pipeline->base, VK_PIPELINE_BIND_POINT_GRAPHICS);

   radv_describe_draw(cmd_buffer);
   if (likely(!info->indirect)) {
      struct radv_cmd_state *state = &cmd_buffer->state;
      if (unlikely(state->last_num_instances != 1)) {
         struct radeon_cmdbuf *cs = cmd_buffer->cs;
         radeon_emit(cs, PKT3(PKT3_NUM_INSTANCES, 0, false));
         radeon_emit(cs, 1);
         state->last_num_instances = 1;
      }
   }

   assert(cmd_buffer->cs->cdw <= cdw_max);
   assert(!ace_cs || ace_cs->cdw <= ace_cdw_max);

   return true;
}

ALWAYS_INLINE static void
radv_after_draw(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radeon_info *rad_info = &cmd_buffer->device->physical_device->rad_info;
   bool has_prefetch = cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX7;
   /* Start prefetches after the draw has been started. Both will
    * run in parallel, but starting the draw first is more
    * important.
    */
   if (has_prefetch && cmd_buffer->state.prefetch_L2_mask) {
      radv_emit_prefetch_L2(cmd_buffer, cmd_buffer->state.graphics_pipeline, false);
   }

   /* Workaround for a VGT hang when streamout is enabled.
    * It must be done after drawing.
    */
   if (radv_is_streamout_enabled(cmd_buffer) &&
       (rad_info->family == CHIP_HAWAII || rad_info->family == CHIP_TONGA ||
        rad_info->family == CHIP_FIJI)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VGT_STREAMOUT_SYNC;
   }

   radv_cmd_buffer_after_draw(cmd_buffer, RADV_CMD_FLAG_PS_PARTIAL_FLUSH);
}

static struct radv_buffer
radv_nv_mesh_indirect_bo(struct radv_cmd_buffer *cmd_buffer,
                         struct radv_buffer *buffer, VkDeviceSize offset,
                         uint32_t draw_count, uint32_t stride)
{
   /* Translates the indirect BO format used by NV_mesh_shader API
    * to the BO format used by DRAW_INDIRECT / DRAW_INDIRECT_MULTI.
    */

   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   struct radeon_winsys *ws = cmd_buffer->device->ws;

   const size_t src_stride = MAX2(stride, sizeof(VkDrawMeshTasksIndirectCommandNV));
   const size_t dst_stride = sizeof(VkDrawIndirectCommand);
   const size_t src_off_task_count = offsetof(VkDrawMeshTasksIndirectCommandNV, taskCount);
   const size_t src_off_first_task = offsetof(VkDrawMeshTasksIndirectCommandNV, firstTask);
   const size_t dst_off_vertex_count = offsetof(VkDrawIndirectCommand, vertexCount);
   const size_t dst_off_first_vertex = offsetof(VkDrawIndirectCommand, firstVertex);

   /* Fill the buffer with all zeroes except instanceCount = 1.
    * This helps emit fewer copy packets below.
    */
   VkDrawIndirectCommand *fill_data = (VkDrawIndirectCommand *) alloca(dst_stride * draw_count);
   const VkDrawIndirectCommand filler = { .instanceCount = 1 };
   for (unsigned i = 0; i < draw_count; ++i)
      fill_data[i] = filler;

   /* We'll have to copy data from the API BO. */
   uint64_t va = radv_buffer_get_va(buffer->bo) + buffer->offset + offset;
   radv_cs_add_buffer(ws, cs, buffer->bo);

   /* Allocate some space in the upload BO. */
   unsigned out_offset;
   radv_cmd_buffer_upload_data(cmd_buffer, dst_stride * draw_count, fill_data, &out_offset);
   const uint64_t new_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + out_offset;

   ASSERTED unsigned cdw_max = radeon_check_space(ws, cs, 12 * draw_count + 2);

   /* Copy data from the API BO so that the format is suitable for the
    * indirect draw packet:
    * - vertexCount = taskCount (copied here)
    * - instanceCount = 1 (filled by CPU above)
    * - firstVertex = firstTask (copied here)
    * - firstInstance = 0 (filled by CPU above)
    */
   for (unsigned i = 0; i < draw_count; ++i) {
      const uint64_t src_task_count = va + i * src_stride + src_off_task_count;
      const uint64_t src_first_task = va + i * src_stride + src_off_first_task;
      const uint64_t dst_vertex_count = new_va + i * dst_stride + dst_off_vertex_count;
      const uint64_t dst_first_vertex = new_va + i * dst_stride + dst_off_first_vertex;

      radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, cmd_buffer->state.predicating));
      radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) |
                      COPY_DATA_WR_CONFIRM);
      radeon_emit(cs, src_task_count);
      radeon_emit(cs, src_task_count >> 32);
      radeon_emit(cs, dst_vertex_count);
      radeon_emit(cs, dst_vertex_count >> 32);

      radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, cmd_buffer->state.predicating));
      radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) |
                      COPY_DATA_WR_CONFIRM);
      radeon_emit(cs, src_first_task);
      radeon_emit(cs, src_first_task >> 32);
      radeon_emit(cs, dst_first_vertex);
      radeon_emit(cs, dst_first_vertex >> 32);
   }

   /* Wait for the copies to finish */
   radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
   radeon_emit(cs, 0);

   /* The draw packet can now use this buffer: */
   struct radv_buffer buf = *buffer;
   buf.bo = cmd_buffer->upload.upload_bo;
   buf.offset = out_offset;

   assert(cmd_buffer->cs->cdw <= cdw_max);

   return buf;
}

static struct radv_buffer
radv_nv_task_indirect_bo(struct radv_cmd_buffer *cmd_buffer, struct radv_buffer *buffer,
                         VkDeviceSize offset, uint32_t draw_count, uint32_t stride)
{
   /* Translates the indirect BO format used by NV_mesh_shader API
    * to the BO format used by DISPATCH_TASKMESH_INDIRECT_MULTI_ACE.
    */

   assert(draw_count);
   static_assert(sizeof(VkDispatchIndirectCommand) == 12, "Incorrect size of taskmesh command.");

   struct radeon_cmdbuf *cs = cmd_buffer->ace_internal.cs;
   struct radeon_winsys *ws = cmd_buffer->device->ws;

   const size_t src_stride = MAX2(stride, sizeof(VkDrawMeshTasksIndirectCommandNV));
   const size_t dst_stride = sizeof(VkDispatchIndirectCommand);
   const size_t src_off_task_count = offsetof(VkDrawMeshTasksIndirectCommandNV, taskCount);
   const size_t dst_off_x = offsetof(VkDispatchIndirectCommand, x);

   const unsigned new_disp_size = dst_stride * draw_count;

   const uint64_t va = radv_buffer_get_va(buffer->bo) + buffer->offset + offset;
   radv_cs_add_buffer(ws, cs, buffer->bo);

   /* Fill the buffer with X=0, Y=1, Z=1. */
   VkDispatchIndirectCommand *fill_data = (VkDispatchIndirectCommand *)alloca(new_disp_size);
   for (unsigned i = 0; i < draw_count; ++i) {
      fill_data[i].x = 0;
      fill_data[i].y = 1;
      fill_data[i].z = 1;
   }

   /* Allocate space in the upload BO. */
   unsigned out_offset;
   ASSERTED bool uploaded =
      radv_cmd_buffer_upload_data(cmd_buffer, new_disp_size, fill_data, &out_offset);
   const uint64_t new_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + out_offset;
   assert(uploaded);

   /* Clamp draw count to fit the actual size of the buffer.
    * This is to avoid potential out of bounds copies (eg. for draws with an indirect count buffer).
    * The remaining indirect draws will stay filled with X=0, Y=1, Z=1 which is harmless.
    */
   draw_count = MIN2(draw_count, (buffer->vk.size - buffer->offset - offset) / src_stride);

   ASSERTED unsigned cdw_max = radeon_check_space(ws, cs, 6 * draw_count + 2);

   /* Copy taskCount from the NV API BO to the X dispatch size of the compatible BO. */
   for (unsigned i = 0; i < draw_count; ++i) {
      const uint64_t src_task_count = va + i * src_stride + src_off_task_count;
      const uint64_t dst_x = new_va + i * dst_stride + dst_off_x;

      radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, cmd_buffer->state.predicating));
      radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) |
                         COPY_DATA_WR_CONFIRM);
      radeon_emit(cs, src_task_count);
      radeon_emit(cs, src_task_count >> 32);
      radeon_emit(cs, dst_x);
      radeon_emit(cs, dst_x >> 32);
   }

   assert(cs->cdw <= cdw_max);

   /* The draw packet can now use this buffer: */
   struct radv_buffer buf = *buffer;
   buf.bo = cmd_buffer->upload.upload_bo;
   buf.offset = out_offset;

   return buf;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
             uint32_t firstVertex, uint32_t firstInstance)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_draw_info info;

   info.count = vertexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_buffer = NULL;
   info.indirect = NULL;
   info.indexed = false;

   if (!radv_before_draw(cmd_buffer, &info, 1))
      return;
   const VkMultiDrawInfoEXT minfo = { firstVertex, vertexCount };
   radv_emit_direct_draw_packets(cmd_buffer, &info, 1, &minfo, 0, 0);
   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMultiEXT(VkCommandBuffer commandBuffer, uint32_t drawCount, const VkMultiDrawInfoEXT *pVertexInfo,
                          uint32_t instanceCount, uint32_t firstInstance, uint32_t stride)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_draw_info info;

   if (!drawCount)
      return;

   info.count = pVertexInfo->vertexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_buffer = NULL;
   info.indirect = NULL;
   info.indexed = false;

   if (!radv_before_draw(cmd_buffer, &info, drawCount))
      return;
   radv_emit_direct_draw_packets(cmd_buffer, &info, drawCount, pVertexInfo, 0, stride);
   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
                    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_draw_info info;

   info.indexed = true;
   info.count = indexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_buffer = NULL;
   info.indirect = NULL;

   if (!radv_before_draw(cmd_buffer, &info, 1))
      return;
   const VkMultiDrawIndexedInfoEXT minfo = { firstIndex, indexCount, vertexOffset };
   radv_emit_draw_packets_indexed(cmd_buffer, &info, 1, &minfo, 0, NULL);
   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMultiIndexedEXT(VkCommandBuffer commandBuffer, uint32_t drawCount, const VkMultiDrawIndexedInfoEXT *pIndexInfo,
                            uint32_t instanceCount, uint32_t firstInstance, uint32_t stride, const int32_t *pVertexOffset)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_draw_info info;

   if (!drawCount)
      return;

   const VkMultiDrawIndexedInfoEXT *minfo = pIndexInfo;
   info.indexed = true;
   info.count = minfo->indexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_buffer = NULL;
   info.indirect = NULL;

   if (!radv_before_draw(cmd_buffer, &info, drawCount))
      return;
   radv_emit_draw_packets_indexed(cmd_buffer, &info, drawCount, pIndexInfo, stride, pVertexOffset);
   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset,
                     uint32_t drawCount, uint32_t stride)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, buffer, _buffer);
   struct radv_draw_info info;

   info.count = drawCount;
   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;
   info.strmout_buffer = NULL;
   info.count_buffer = NULL;
   info.indexed = false;
   info.instance_count = 0;

   if (!radv_before_draw(cmd_buffer, &info, 1))
      return;
   radv_emit_indirect_draw_packets(cmd_buffer, &info);
   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset,
                            uint32_t drawCount, uint32_t stride)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, buffer, _buffer);
   struct radv_draw_info info;

   info.indexed = true;
   info.count = drawCount;
   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;
   info.count_buffer = NULL;
   info.strmout_buffer = NULL;
   info.instance_count = 0;

   if (!radv_before_draw(cmd_buffer, &info, 1))
      return;
   radv_emit_indirect_draw_packets(cmd_buffer, &info);
   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndirectCount(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset,
                          VkBuffer _countBuffer, VkDeviceSize countBufferOffset,
                          uint32_t maxDrawCount, uint32_t stride)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, buffer, _buffer);
   RADV_FROM_HANDLE(radv_buffer, count_buffer, _countBuffer);
   struct radv_draw_info info;

   info.count = maxDrawCount;
   info.indirect = buffer;
   info.indirect_offset = offset;
   info.count_buffer = count_buffer;
   info.count_buffer_offset = countBufferOffset;
   info.stride = stride;
   info.strmout_buffer = NULL;
   info.indexed = false;
   info.instance_count = 0;

   if (!radv_before_draw(cmd_buffer, &info, 1))
      return;
   radv_emit_indirect_draw_packets(cmd_buffer, &info);
   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer, VkBuffer _buffer,
                                 VkDeviceSize offset, VkBuffer _countBuffer,
                                 VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
                                 uint32_t stride)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, buffer, _buffer);
   RADV_FROM_HANDLE(radv_buffer, count_buffer, _countBuffer);
   struct radv_draw_info info;

   info.indexed = true;
   info.count = maxDrawCount;
   info.indirect = buffer;
   info.indirect_offset = offset;
   info.count_buffer = count_buffer;
   info.count_buffer_offset = countBufferOffset;
   info.stride = stride;
   info.strmout_buffer = NULL;
   info.instance_count = 0;

   if (!radv_before_draw(cmd_buffer, &info, 1))
      return;
   radv_emit_indirect_draw_packets(cmd_buffer, &info);
   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMeshTasksNV(VkCommandBuffer commandBuffer, uint32_t taskCount, uint32_t firstTask)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radv_draw_info info;

   info.count = taskCount;
   info.instance_count = 1;
   info.first_instance = 0;
   info.stride = 0;
   info.indexed = false;
   info.strmout_buffer = NULL;
   info.count_buffer = NULL;
   info.indirect = NULL;

   if (!radv_before_taskmesh_draw(cmd_buffer, &info, 1))
      return;

   if (radv_pipeline_has_stage(pipeline, MESA_SHADER_TASK)) {
      radv_emit_direct_taskmesh_draw_packets(cmd_buffer, taskCount, 1, 1, firstTask);
   } else {
      radv_emit_direct_mesh_draw_packet(cmd_buffer, taskCount, 1, 1, firstTask);
   }

   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMeshTasksEXT(VkCommandBuffer commandBuffer, uint32_t x, uint32_t y, uint32_t z)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radv_draw_info info;

   info.count = x * y * z;
   info.instance_count = 1;
   info.first_instance = 0;
   info.stride = 0;
   info.indexed = false;
   info.strmout_buffer = NULL;
   info.count_buffer = NULL;
   info.indirect = NULL;

   if (!radv_before_taskmesh_draw(cmd_buffer, &info, 1))
      return;

   if (radv_pipeline_has_stage(pipeline, MESA_SHADER_TASK)) {
      radv_emit_direct_taskmesh_draw_packets(cmd_buffer, x, y, z, 0);
   } else {
      radv_emit_direct_mesh_draw_packet(cmd_buffer, x, y, z, 0);
   }

   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMeshTasksIndirectNV(VkCommandBuffer commandBuffer, VkBuffer _buffer,
                                VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, buffer, _buffer);

   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radv_draw_info info;

   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;
   info.count = drawCount;
   info.strmout_buffer = NULL;
   info.count_buffer = NULL;
   info.indexed = false;
   info.instance_count = 0;

   if (!radv_before_taskmesh_draw(cmd_buffer, &info, drawCount))
      return;

   /* Indirect draw with mesh shader only:
    * Use DRAW_INDIRECT / DRAW_INDIRECT_MULTI like normal indirect draws.
    * Needed because DISPATCH_MESH_INDIRECT_MULTI doesn't support firstTask.
    *
    * Indirect draw with task + mesh shaders:
    * Use DISPATCH_TASKMESH_INDIRECT_MULTI_ACE + DISPATCH_TASKMESH_GFX.
    * These packets don't support firstTask so we implement that by
    * reading the NV command's indirect buffer in the shader.
    *
    * The indirect BO layout from the NV_mesh_shader API is incompatible
    * with AMD HW. To make it work, we allocate some space
    * in the upload buffer and copy the data to it.
    */

   if (radv_pipeline_has_stage(pipeline, MESA_SHADER_TASK)) {
      uint64_t nv_ib_va = radv_buffer_get_va(buffer->bo) + buffer->offset + offset;
      uint32_t nv_ib_stride = MAX2(stride, sizeof(VkDrawMeshTasksIndirectCommandNV));
      struct radv_buffer buf =
         radv_nv_task_indirect_bo(cmd_buffer, buffer, offset, drawCount, stride);
      info.indirect = &buf;
      info.indirect_offset = 0;
      info.stride = sizeof(VkDispatchIndirectCommand);

      radv_emit_indirect_taskmesh_draw_packets(cmd_buffer, &info, nv_ib_va, nv_ib_stride);
   } else {
      struct radv_buffer buf =
         radv_nv_mesh_indirect_bo(cmd_buffer, buffer, offset, drawCount, stride);
      info.indirect = &buf;
      info.indirect_offset = 0;
      info.stride = sizeof(VkDrawIndirectCommand);

      radv_emit_indirect_draw_packets(cmd_buffer, &info);
   }

   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMeshTasksIndirectEXT(VkCommandBuffer commandBuffer, VkBuffer _buffer,
                                 VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
   if (!drawCount)
      return;

   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, buffer, _buffer);

   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radv_draw_info info;

   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;
   info.count = drawCount;
   info.strmout_buffer = NULL;
   info.count_buffer = NULL;
   info.indexed = false;
   info.instance_count = 0;

   if (!radv_before_taskmesh_draw(cmd_buffer, &info, drawCount))
      return;

   if (radv_pipeline_has_stage(pipeline, MESA_SHADER_TASK)) {
      radv_emit_indirect_taskmesh_draw_packets(cmd_buffer, &info, 0, 0);
   } else {
      radv_emit_indirect_mesh_draw_packets(cmd_buffer, &info);
   }

   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMeshTasksIndirectCountNV(VkCommandBuffer commandBuffer, VkBuffer _buffer,
                                     VkDeviceSize offset, VkBuffer _countBuffer,
                                     VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
                                     uint32_t stride)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, buffer, _buffer);
   RADV_FROM_HANDLE(radv_buffer, count_buffer, _countBuffer);

   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radv_draw_info info;

   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;
   info.count = maxDrawCount;
   info.strmout_buffer = NULL;
   info.count_buffer = count_buffer;
   info.count_buffer_offset = countBufferOffset;
   info.indexed = false;
   info.instance_count = 0;

   if (!radv_before_taskmesh_draw(cmd_buffer, &info, maxDrawCount))
      return;

   if (radv_pipeline_has_stage(pipeline, MESA_SHADER_TASK)) {
      uint64_t nv_ib_va = radv_buffer_get_va(buffer->bo) + buffer->offset + offset;
      uint32_t nv_ib_stride = MAX2(stride, sizeof(VkDrawMeshTasksIndirectCommandNV));
      struct radv_buffer buf =
         radv_nv_task_indirect_bo(cmd_buffer, buffer, offset, maxDrawCount, stride);
      info.indirect = &buf;
      info.indirect_offset = 0;
      info.stride = sizeof(VkDispatchIndirectCommand);

      radv_emit_indirect_taskmesh_draw_packets(cmd_buffer, &info, nv_ib_va, nv_ib_stride);
   } else {
      struct radv_buffer buf =
         radv_nv_mesh_indirect_bo(cmd_buffer, buffer, offset, maxDrawCount, stride);
      info.indirect = &buf;
      info.indirect_offset = 0;
      info.stride = sizeof(VkDrawIndirectCommand);

      radv_emit_indirect_draw_packets(cmd_buffer, &info);
   }

   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMeshTasksIndirectCountEXT(VkCommandBuffer commandBuffer, VkBuffer _buffer,
                                      VkDeviceSize offset, VkBuffer _countBuffer,
                                      VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
                                      uint32_t stride)
{

   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, buffer, _buffer);
   RADV_FROM_HANDLE(radv_buffer, count_buffer, _countBuffer);

   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radv_draw_info info;

   info.indirect = buffer;
   info.indirect_offset = offset;
   info.stride = stride;
   info.count = maxDrawCount;
   info.strmout_buffer = NULL;
   info.count_buffer = count_buffer;
   info.count_buffer_offset = countBufferOffset;
   info.indexed = false;
   info.instance_count = 0;

   if (!radv_before_taskmesh_draw(cmd_buffer, &info, maxDrawCount))
      return;

   if (radv_pipeline_has_stage(pipeline, MESA_SHADER_TASK)) {
      radv_emit_indirect_taskmesh_draw_packets(cmd_buffer, &info, 0, 0);
   } else {
      radv_emit_indirect_mesh_draw_packets(cmd_buffer, &info);
   }

   radv_after_draw(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdExecuteGeneratedCommandsNV(VkCommandBuffer commandBuffer, VkBool32 isPreprocessed,
                                   const VkGeneratedCommandsInfoNV *pGeneratedCommandsInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_indirect_command_layout, layout,
                  pGeneratedCommandsInfo->indirectCommandsLayout);
   VK_FROM_HANDLE(radv_buffer, prep_buffer, pGeneratedCommandsInfo->preprocessBuffer);

   /* The only actions that can be done are draws, so skip on other queues. */
   if (cmd_buffer->qf != RADV_QUEUE_GENERAL)
      return;

   /* Secondary command buffers are needed for the full extension but can't use
    * PKT3_INDIRECT_BUFFER_CIK.
    */
   assert(cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);

   radv_prepare_dgc(cmd_buffer, pGeneratedCommandsInfo);

   struct radv_draw_info info;

   info.count = pGeneratedCommandsInfo->sequencesCount;
   info.indirect = prep_buffer; /* We're not really going use it this way, but a good signal
                                   that this is not direct. */
   info.indirect_offset = 0;
   info.stride = 0;
   info.strmout_buffer = NULL;
   info.count_buffer = NULL;
   info.indexed = layout->indexed;
   info.instance_count = 0;

   if (!radv_before_draw(cmd_buffer, &info, 1))
      return;

   uint32_t cmdbuf_size = radv_get_indirect_cmdbuf_size(pGeneratedCommandsInfo);
   uint64_t va = radv_buffer_get_va(prep_buffer->bo) + prep_buffer->offset +
                 pGeneratedCommandsInfo->preprocessOffset;
   const uint32_t view_mask = cmd_buffer->state.render.view_mask;

   radeon_emit(cmd_buffer->cs, PKT3(PKT3_PFP_SYNC_ME, 0, cmd_buffer->state.predicating));
   radeon_emit(cmd_buffer->cs, 0);

   if (!view_mask) {
      radeon_emit(cmd_buffer->cs, PKT3(PKT3_INDIRECT_BUFFER_CIK, 2, 0));
      radeon_emit(cmd_buffer->cs, va);
      radeon_emit(cmd_buffer->cs, va >> 32);
      radeon_emit(cmd_buffer->cs, cmdbuf_size >> 2);
   } else {
      u_foreach_bit (view, view_mask) {
         radv_emit_view_index(cmd_buffer, view);

         radeon_emit(cmd_buffer->cs, PKT3(PKT3_INDIRECT_BUFFER_CIK, 2, 0));
         radeon_emit(cmd_buffer->cs, va);
         radeon_emit(cmd_buffer->cs, va >> 32);
         radeon_emit(cmd_buffer->cs, cmdbuf_size >> 2);
      }
   }

   if (layout->binds_index_buffer) {
      cmd_buffer->state.last_index_type = -1;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_INDEX_BUFFER;
   }

   if (layout->bind_vbo_mask)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VERTEX_BUFFER;

   if (layout->binds_state)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_FRONT_FACE;

   cmd_buffer->push_constant_stages |= ~0;

   cmd_buffer->state.last_index_type = -1;
   cmd_buffer->state.last_num_instances = -1;
   cmd_buffer->state.last_vertex_offset = -1;
   cmd_buffer->state.last_first_instance = -1;
   cmd_buffer->state.last_drawid = -1;

   radv_after_draw(cmd_buffer);
}

static void
radv_emit_dispatch_packets(struct radv_cmd_buffer *cmd_buffer,
                           struct radv_compute_pipeline *pipeline,
                           const struct radv_dispatch_info *info)
{
   struct radv_shader *compute_shader = pipeline->base.shaders[MESA_SHADER_COMPUTE];
   unsigned dispatch_initiator = cmd_buffer->device->dispatch_initiator;
   struct radeon_winsys *ws = cmd_buffer->device->ws;
   bool predicating = cmd_buffer->state.predicating;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   struct radv_userdata_info *loc;

   radv_describe_dispatch(cmd_buffer, info->blocks[0], info->blocks[1], info->blocks[2]);

   loc = radv_lookup_user_sgpr(&pipeline->base, MESA_SHADER_COMPUTE, AC_UD_CS_GRID_SIZE);

   ASSERTED unsigned cdw_max = radeon_check_space(ws, cs, 30);

   if (compute_shader->info.wave_size == 32) {
      assert(cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX10);
      dispatch_initiator |= S_00B800_CS_W32_EN(1);
   }

   if (info->ordered)
      dispatch_initiator &= ~S_00B800_ORDER_MODE(1);

   if (info->va) {
      if (info->indirect)
         radv_cs_add_buffer(ws, cs, info->indirect);

      if (info->unaligned) {
         radeon_set_sh_reg_seq(cs, R_00B81C_COMPUTE_NUM_THREAD_X, 3);
         radeon_emit(cs, S_00B81C_NUM_THREAD_FULL(compute_shader->info.cs.block_size[0]));
         radeon_emit(cs, S_00B81C_NUM_THREAD_FULL(compute_shader->info.cs.block_size[1]));
         radeon_emit(cs, S_00B81C_NUM_THREAD_FULL(compute_shader->info.cs.block_size[2]));

         dispatch_initiator |= S_00B800_USE_THREAD_DIMENSIONS(1);
      }

      if (loc->sgpr_idx != -1) {
         unsigned reg = R_00B900_COMPUTE_USER_DATA_0 + loc->sgpr_idx * 4;

         if (cmd_buffer->device->load_grid_size_from_user_sgpr) {
            assert(cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX10_3);
            radeon_emit(cs, PKT3(PKT3_LOAD_SH_REG_INDEX, 3, 0));
            radeon_emit(cs, info->va);
            radeon_emit(cs, info->va >> 32);
            radeon_emit(cs, (reg - SI_SH_REG_OFFSET) >> 2);
            radeon_emit(cs, 3);
         } else {
            radv_emit_shader_pointer(cmd_buffer->device, cmd_buffer->cs, reg, info->va, true);
         }
      }

      if (radv_cmd_buffer_uses_mec(cmd_buffer)) {
         radv_cs_emit_compute_predication(&cmd_buffer->state, cs, cmd_buffer->mec_inv_pred_va,
                                          &cmd_buffer->mec_inv_pred_emitted,
                                          4 /* DISPATCH_INDIRECT size */);
         radeon_emit(cs, PKT3(PKT3_DISPATCH_INDIRECT, 2, 0) | PKT3_SHADER_TYPE_S(1));
         radeon_emit(cs, info->va);
         radeon_emit(cs, info->va >> 32);
         radeon_emit(cs, dispatch_initiator);
      } else {
         radeon_emit(cs, PKT3(PKT3_SET_BASE, 2, 0) | PKT3_SHADER_TYPE_S(1));
         radeon_emit(cs, 1);
         radeon_emit(cs, info->va);
         radeon_emit(cs, info->va >> 32);

         radeon_emit(cs, PKT3(PKT3_DISPATCH_INDIRECT, 1, predicating) | PKT3_SHADER_TYPE_S(1));
         radeon_emit(cs, 0);
         radeon_emit(cs, dispatch_initiator);
      }
   } else {
      unsigned blocks[3] = {info->blocks[0], info->blocks[1], info->blocks[2]};
      unsigned offsets[3] = {info->offsets[0], info->offsets[1], info->offsets[2]};

      if (info->unaligned) {
         unsigned *cs_block_size = compute_shader->info.cs.block_size;
         unsigned remainder[3];

         /* If aligned, these should be an entire block size,
          * not 0.
          */
         remainder[0] = blocks[0] + cs_block_size[0] - align_u32_npot(blocks[0], cs_block_size[0]);
         remainder[1] = blocks[1] + cs_block_size[1] - align_u32_npot(blocks[1], cs_block_size[1]);
         remainder[2] = blocks[2] + cs_block_size[2] - align_u32_npot(blocks[2], cs_block_size[2]);

         blocks[0] = round_up_u32(blocks[0], cs_block_size[0]);
         blocks[1] = round_up_u32(blocks[1], cs_block_size[1]);
         blocks[2] = round_up_u32(blocks[2], cs_block_size[2]);

         for (unsigned i = 0; i < 3; ++i) {
            assert(offsets[i] % cs_block_size[i] == 0);
            offsets[i] /= cs_block_size[i];
         }

         radeon_set_sh_reg_seq(cs, R_00B81C_COMPUTE_NUM_THREAD_X, 3);
         radeon_emit(cs, S_00B81C_NUM_THREAD_FULL(cs_block_size[0]) |
                            S_00B81C_NUM_THREAD_PARTIAL(remainder[0]));
         radeon_emit(cs, S_00B81C_NUM_THREAD_FULL(cs_block_size[1]) |
                            S_00B81C_NUM_THREAD_PARTIAL(remainder[1]));
         radeon_emit(cs, S_00B81C_NUM_THREAD_FULL(cs_block_size[2]) |
                            S_00B81C_NUM_THREAD_PARTIAL(remainder[2]));

         dispatch_initiator |= S_00B800_PARTIAL_TG_EN(1);
      }

      if (loc->sgpr_idx != -1) {
         if (cmd_buffer->device->load_grid_size_from_user_sgpr) {
            assert(loc->num_sgprs == 3);

            radeon_set_sh_reg_seq(cs, R_00B900_COMPUTE_USER_DATA_0 + loc->sgpr_idx * 4, 3);
            radeon_emit(cs, blocks[0]);
            radeon_emit(cs, blocks[1]);
            radeon_emit(cs, blocks[2]);
         } else {
            uint32_t offset;
            if (!radv_cmd_buffer_upload_data(cmd_buffer, 12, blocks, &offset))
               return;

            uint64_t va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + offset;
            radv_emit_shader_pointer(cmd_buffer->device, cmd_buffer->cs,
                                     R_00B900_COMPUTE_USER_DATA_0 + loc->sgpr_idx * 4, va, true);
         }
      }

      if (offsets[0] || offsets[1] || offsets[2]) {
         radeon_set_sh_reg_seq(cs, R_00B810_COMPUTE_START_X, 3);
         radeon_emit(cs, offsets[0]);
         radeon_emit(cs, offsets[1]);
         radeon_emit(cs, offsets[2]);

         /* The blocks in the packet are not counts but end values. */
         for (unsigned i = 0; i < 3; ++i)
            blocks[i] += offsets[i];
      } else {
         dispatch_initiator |= S_00B800_FORCE_START_AT_000(1);
      }

      if (radv_cmd_buffer_uses_mec(cmd_buffer)) {
         radv_cs_emit_compute_predication(&cmd_buffer->state, cs, cmd_buffer->mec_inv_pred_va,
                                          &cmd_buffer->mec_inv_pred_emitted,
                                          5 /* DISPATCH_DIRECT size */);
         predicating = false;
      }

      radeon_emit(cs, PKT3(PKT3_DISPATCH_DIRECT, 3, predicating) | PKT3_SHADER_TYPE_S(1));
      radeon_emit(cs, blocks[0]);
      radeon_emit(cs, blocks[1]);
      radeon_emit(cs, blocks[2]);
      radeon_emit(cs, dispatch_initiator);
   }

   assert(cmd_buffer->cs->cdw <= cdw_max);
}

static void
radv_upload_compute_shader_descriptors(struct radv_cmd_buffer *cmd_buffer,
                                       struct radv_compute_pipeline *pipeline,
                                       VkPipelineBindPoint bind_point)
{
   radv_flush_descriptors(cmd_buffer, VK_SHADER_STAGE_COMPUTE_BIT, &pipeline->base, bind_point);
   const VkShaderStageFlags stages = bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR
                                        ? RADV_RT_STAGE_BITS
                                        : VK_SHADER_STAGE_COMPUTE_BIT;
   const VkShaderStageFlags pc_stages =
      radv_must_flush_constants(cmd_buffer, &pipeline->base, stages);
   if (pc_stages)
      radv_flush_constants(cmd_buffer, pc_stages, &pipeline->base, bind_point);
}

static void
radv_dispatch(struct radv_cmd_buffer *cmd_buffer, const struct radv_dispatch_info *info,
              struct radv_compute_pipeline *pipeline, VkPipelineBindPoint bind_point)
{
   bool has_prefetch = cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX7;
   bool pipeline_is_dirty = pipeline != cmd_buffer->state.emitted_compute_pipeline;

   if (pipeline->cs_regalloc_hang_bug)
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_PS_PARTIAL_FLUSH |
                                      RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   if (cmd_buffer->state.flush_bits &
       (RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB |
        RADV_CMD_FLAG_PS_PARTIAL_FLUSH | RADV_CMD_FLAG_CS_PARTIAL_FLUSH)) {
      /* If we have to wait for idle, set all states first, so that
       * all SET packets are processed in parallel with previous draw
       * calls. Then upload descriptors, set shader pointers, and
       * dispatch, and prefetch at the end. This ensures that the
       * time the CUs are idle is very short. (there are only SET_SH
       * packets between the wait and the draw)
       */
      radv_emit_compute_pipeline(cmd_buffer, pipeline);
      si_emit_cache_flush(cmd_buffer);
      /* <-- CUs are idle here --> */

      radv_upload_compute_shader_descriptors(cmd_buffer, pipeline, bind_point);

      radv_emit_dispatch_packets(cmd_buffer, pipeline, info);
      /* <-- CUs are busy here --> */

      /* Start prefetches after the dispatch has been started. Both
       * will run in parallel, but starting the dispatch first is
       * more important.
       */
      if (has_prefetch && pipeline_is_dirty) {
         radv_emit_shader_prefetch(cmd_buffer, pipeline->base.shaders[MESA_SHADER_COMPUTE]);
      }
   } else {
      /* If we don't wait for idle, start prefetches first, then set
       * states, and dispatch at the end.
       */
      si_emit_cache_flush(cmd_buffer);

      if (has_prefetch && pipeline_is_dirty) {
         radv_emit_shader_prefetch(cmd_buffer, pipeline->base.shaders[MESA_SHADER_COMPUTE]);
      }

      radv_upload_compute_shader_descriptors(cmd_buffer, pipeline, bind_point);

      radv_emit_compute_pipeline(cmd_buffer, pipeline);
      radv_emit_dispatch_packets(cmd_buffer, pipeline, info);
   }

   if (pipeline_is_dirty) {
      /* Raytracing uses compute shaders but has separate bind points and pipelines.
       * So if we set compute userdata & shader registers we should dirty the raytracing
       * ones and the other way around.
       *
       * We only need to do this when the pipeline is dirty because when we switch between
       * the two we always need to switch pipelines.
       */
      radv_mark_descriptor_sets_dirty(cmd_buffer, bind_point == VK_PIPELINE_BIND_POINT_COMPUTE
                                                     ? VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR
                                                     : VK_PIPELINE_BIND_POINT_COMPUTE);
   }

   if (pipeline->cs_regalloc_hang_bug)
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   radv_cmd_buffer_after_draw(cmd_buffer, RADV_CMD_FLAG_CS_PARTIAL_FLUSH);
}

void
radv_compute_dispatch(struct radv_cmd_buffer *cmd_buffer, const struct radv_dispatch_info *info)
{
   radv_dispatch(cmd_buffer, info, cmd_buffer->state.compute_pipeline,
                 VK_PIPELINE_BIND_POINT_COMPUTE);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDispatchBase(VkCommandBuffer commandBuffer, uint32_t base_x, uint32_t base_y,
                     uint32_t base_z, uint32_t x, uint32_t y, uint32_t z)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_dispatch_info info = {0};

   info.blocks[0] = x;
   info.blocks[1] = y;
   info.blocks[2] = z;

   info.offsets[0] = base_x;
   info.offsets[1] = base_y;
   info.offsets[2] = base_z;
   radv_compute_dispatch(cmd_buffer, &info);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, buffer, _buffer);
   struct radv_dispatch_info info = {0};

   info.indirect = buffer->bo;
   info.va = radv_buffer_get_va(buffer->bo) + buffer->offset + offset;

   radv_compute_dispatch(cmd_buffer, &info);
}

void
radv_unaligned_dispatch(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z)
{
   struct radv_dispatch_info info = {0};

   info.blocks[0] = x;
   info.blocks[1] = y;
   info.blocks[2] = z;
   info.unaligned = 1;

   radv_compute_dispatch(cmd_buffer, &info);
}

void
radv_indirect_dispatch(struct radv_cmd_buffer *cmd_buffer, struct radeon_winsys_bo *bo, uint64_t va)
{
   struct radv_dispatch_info info = {0};

   info.indirect = bo;
   info.va = va;

   radv_compute_dispatch(cmd_buffer, &info);
}

enum radv_rt_mode {
   radv_rt_mode_direct,
   radv_rt_mode_indirect,
   radv_rt_mode_indirect2,
};

static void
radv_trace_rays(struct radv_cmd_buffer *cmd_buffer, const VkTraceRaysIndirectCommand2KHR *tables,
                uint64_t indirect_va, enum radv_rt_mode mode)
{
   struct radv_compute_pipeline *pipeline = &cmd_buffer->state.rt_pipeline->base;
   uint32_t base_reg = pipeline->base.user_data_0[MESA_SHADER_COMPUTE];

   struct radv_dispatch_info info = {0};
   info.unaligned = true;

   uint64_t launch_size_va;
   uint64_t sbt_va;

   if (mode != radv_rt_mode_indirect2) {
      uint32_t upload_size = mode == radv_rt_mode_direct
                                ? sizeof(VkTraceRaysIndirectCommand2KHR)
                                : offsetof(VkTraceRaysIndirectCommand2KHR, width);

      uint32_t offset;
      if (!radv_cmd_buffer_upload_data(cmd_buffer, upload_size, tables, &offset))
         return;

      uint64_t upload_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + offset;

      launch_size_va = (mode == radv_rt_mode_direct)
                          ? upload_va + offsetof(VkTraceRaysIndirectCommand2KHR, width)
                          : indirect_va;
      sbt_va = upload_va;
   } else {
      launch_size_va = indirect_va + offsetof(VkTraceRaysIndirectCommand2KHR, width);
      sbt_va = indirect_va;
   }

   if (mode == radv_rt_mode_direct) {
      info.blocks[0] = tables->width;
      info.blocks[1] = tables->height;
      info.blocks[2] = tables->depth;
   } else
      info.va = launch_size_va;

   ASSERTED unsigned cdw_max = radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 11);

   struct radv_userdata_info *desc_loc =
      radv_lookup_user_sgpr(&pipeline->base, MESA_SHADER_COMPUTE, AC_UD_CS_SBT_DESCRIPTORS);
   if (desc_loc->sgpr_idx != -1) {
      radv_emit_shader_pointer(cmd_buffer->device, cmd_buffer->cs,
                               base_reg + desc_loc->sgpr_idx * 4, sbt_va, true);
   }

   struct radv_userdata_info *size_loc =
      radv_lookup_user_sgpr(&pipeline->base, MESA_SHADER_COMPUTE, AC_UD_CS_RAY_LAUNCH_SIZE_ADDR);
   if (size_loc->sgpr_idx != -1) {
      radv_emit_shader_pointer(cmd_buffer->device, cmd_buffer->cs,
                               base_reg + size_loc->sgpr_idx * 4, launch_size_va, true);
   }

   struct radv_userdata_info *base_loc = radv_lookup_user_sgpr(
      &pipeline->base, MESA_SHADER_COMPUTE, AC_UD_CS_RAY_DYNAMIC_CALLABLE_STACK_BASE);
   if (base_loc->sgpr_idx != -1) {
      struct radv_shader_info *cs_info = &pipeline->base.shaders[MESA_SHADER_COMPUTE]->info;
      radeon_set_sh_reg(cmd_buffer->cs, R_00B900_COMPUTE_USER_DATA_0 + base_loc->sgpr_idx * 4,
                        pipeline->base.scratch_bytes_per_wave / cs_info->wave_size);
   }

   assert(cmd_buffer->cs->cdw <= cdw_max);

   radv_dispatch(cmd_buffer, &info, pipeline, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdTraceRaysKHR(VkCommandBuffer commandBuffer,
                     const VkStridedDeviceAddressRegionKHR *pRaygenShaderBindingTable,
                     const VkStridedDeviceAddressRegionKHR *pMissShaderBindingTable,
                     const VkStridedDeviceAddressRegionKHR *pHitShaderBindingTable,
                     const VkStridedDeviceAddressRegionKHR *pCallableShaderBindingTable,
                     uint32_t width, uint32_t height, uint32_t depth)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   VkTraceRaysIndirectCommand2KHR tables = {
      .raygenShaderRecordAddress = pRaygenShaderBindingTable->deviceAddress,
      .raygenShaderRecordSize = pRaygenShaderBindingTable->size,
      .missShaderBindingTableAddress = pMissShaderBindingTable->deviceAddress,
      .missShaderBindingTableSize = pMissShaderBindingTable->size,
      .missShaderBindingTableStride = pMissShaderBindingTable->stride,
      .hitShaderBindingTableAddress = pHitShaderBindingTable->deviceAddress,
      .hitShaderBindingTableSize = pHitShaderBindingTable->size,
      .hitShaderBindingTableStride = pHitShaderBindingTable->stride,
      .callableShaderBindingTableAddress = pCallableShaderBindingTable->deviceAddress,
      .callableShaderBindingTableSize = pCallableShaderBindingTable->size,
      .callableShaderBindingTableStride = pCallableShaderBindingTable->stride,
      .width = width,
      .height = height,
      .depth = depth,
   };

   radv_trace_rays(cmd_buffer, &tables, 0, radv_rt_mode_direct);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdTraceRaysIndirectKHR(VkCommandBuffer commandBuffer,
                             const VkStridedDeviceAddressRegionKHR *pRaygenShaderBindingTable,
                             const VkStridedDeviceAddressRegionKHR *pMissShaderBindingTable,
                             const VkStridedDeviceAddressRegionKHR *pHitShaderBindingTable,
                             const VkStridedDeviceAddressRegionKHR *pCallableShaderBindingTable,
                             VkDeviceAddress indirectDeviceAddress)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   assert(cmd_buffer->device->use_global_bo_list);

   VkTraceRaysIndirectCommand2KHR tables = {
      .raygenShaderRecordAddress = pRaygenShaderBindingTable->deviceAddress,
      .raygenShaderRecordSize = pRaygenShaderBindingTable->size,
      .missShaderBindingTableAddress = pMissShaderBindingTable->deviceAddress,
      .missShaderBindingTableSize = pMissShaderBindingTable->size,
      .missShaderBindingTableStride = pMissShaderBindingTable->stride,
      .hitShaderBindingTableAddress = pHitShaderBindingTable->deviceAddress,
      .hitShaderBindingTableSize = pHitShaderBindingTable->size,
      .hitShaderBindingTableStride = pHitShaderBindingTable->stride,
      .callableShaderBindingTableAddress = pCallableShaderBindingTable->deviceAddress,
      .callableShaderBindingTableSize = pCallableShaderBindingTable->size,
      .callableShaderBindingTableStride = pCallableShaderBindingTable->stride,
   };

   radv_trace_rays(cmd_buffer, &tables, indirectDeviceAddress, radv_rt_mode_indirect);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdTraceRaysIndirect2KHR(VkCommandBuffer commandBuffer, VkDeviceAddress indirectDeviceAddress)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   assert(cmd_buffer->device->use_global_bo_list);

   radv_trace_rays(cmd_buffer, NULL, indirectDeviceAddress, radv_rt_mode_indirect2);
}

static void
radv_set_rt_stack_size(struct radv_cmd_buffer *cmd_buffer, uint32_t size)
{
   unsigned wave_size = 0;
   unsigned scratch_bytes_per_wave = 0;

   if (cmd_buffer->state.rt_pipeline) {
      scratch_bytes_per_wave = cmd_buffer->state.rt_pipeline->base.base.scratch_bytes_per_wave;
      wave_size =
         cmd_buffer->state.rt_pipeline->base.base.shaders[MESA_SHADER_COMPUTE]->info.wave_size;
   }

   /* The hardware register is specified as a multiple of 256 DWORDS. */
   scratch_bytes_per_wave += align(size * wave_size, 1024);

   cmd_buffer->compute_scratch_size_per_wave_needed =
      MAX2(cmd_buffer->compute_scratch_size_per_wave_needed, scratch_bytes_per_wave);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRayTracingPipelineStackSizeKHR(VkCommandBuffer commandBuffer, uint32_t size)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   radv_set_rt_stack_size(cmd_buffer, size);
   cmd_buffer->state.rt_stack_size = size;
}

/*
 * For HTILE we have the following interesting clear words:
 *   0xfffff30f: Uncompressed, full depth range, for depth+stencil HTILE
 *   0xfffc000f: Uncompressed, full depth range, for depth only HTILE.
 *   0xfffffff0: Clear depth to 1.0
 *   0x00000000: Clear depth to 0.0
 */
static void
radv_initialize_htile(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                      const VkImageSubresourceRange *range)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint32_t htile_value = radv_get_htile_initial_value(cmd_buffer->device, image);
   VkClearDepthStencilValue value = {0};
   struct radv_barrier_data barrier = {0};

   barrier.layout_transitions.init_mask_ram = 1;
   radv_describe_layout_transition(cmd_buffer, &barrier);

   /* Transitioning from LAYOUT_UNDEFINED layout not everyone is consistent
    * in considering previous rendering work for WAW hazards. */
   state->flush_bits |=
      radv_src_access_flush(cmd_buffer, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, image);

   if (image->planes[0].surface.has_stencil &&
       !(range->aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
      /* Flush caches before performing a separate aspect initialization because it's a
       * read-modify-write operation.
       */
      state->flush_bits |= radv_dst_access_flush(cmd_buffer, VK_ACCESS_2_SHADER_READ_BIT, image);
   }

   state->flush_bits |= radv_clear_htile(cmd_buffer, image, range, htile_value);

   radv_set_ds_clear_metadata(cmd_buffer, image, range, value, range->aspectMask);

   if (radv_image_is_tc_compat_htile(image) && (range->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) {
      /* Initialize the TC-compat metada value to 0 because by
       * default DB_Z_INFO.RANGE_PRECISION is set to 1, and we only
       * need have to conditionally update its value when performing
       * a fast depth clear.
       */
      radv_set_tc_compat_zrange_metadata(cmd_buffer, image, range, 0);
   }
}

static void
radv_handle_depth_image_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                   VkImageLayout src_layout, VkImageLayout dst_layout,
                                   unsigned src_queue_mask, unsigned dst_queue_mask,
                                   const VkImageSubresourceRange *range,
                                   struct radv_sample_locations_state *sample_locs)
{
   struct radv_device *device = cmd_buffer->device;

   if (!radv_htile_enabled(image, range->baseMipLevel))
      return;

   if (src_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
      radv_initialize_htile(cmd_buffer, image, range);
   } else if (!radv_layout_is_htile_compressed(device, image, src_layout, src_queue_mask) &&
              radv_layout_is_htile_compressed(device, image, dst_layout, dst_queue_mask)) {
      radv_initialize_htile(cmd_buffer, image, range);
   } else if (radv_layout_is_htile_compressed(device, image, src_layout, src_queue_mask) &&
              !radv_layout_is_htile_compressed(device, image, dst_layout, dst_queue_mask)) {
      cmd_buffer->state.flush_bits |=
         RADV_CMD_FLAG_FLUSH_AND_INV_DB | RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;

      radv_expand_depth_stencil(cmd_buffer, image, range, sample_locs);

      cmd_buffer->state.flush_bits |=
         RADV_CMD_FLAG_FLUSH_AND_INV_DB | RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
   }
}

static uint32_t
radv_init_cmask(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                const VkImageSubresourceRange *range, uint32_t value)
{
   struct radv_barrier_data barrier = {0};

   barrier.layout_transitions.init_mask_ram = 1;
   radv_describe_layout_transition(cmd_buffer, &barrier);

   return radv_clear_cmask(cmd_buffer, image, range, value);
}

uint32_t
radv_init_fmask(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                const VkImageSubresourceRange *range)
{
   static const uint32_t fmask_clear_values[4] = {0x00000000, 0x02020202, 0xE4E4E4E4, 0x76543210};
   uint32_t log2_samples = util_logbase2(image->info.samples);
   uint32_t value = fmask_clear_values[log2_samples];
   struct radv_barrier_data barrier = {0};

   barrier.layout_transitions.init_mask_ram = 1;
   radv_describe_layout_transition(cmd_buffer, &barrier);

   return radv_clear_fmask(cmd_buffer, image, range, value);
}

uint32_t
radv_init_dcc(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
              const VkImageSubresourceRange *range, uint32_t value)
{
   struct radv_barrier_data barrier = {0};
   uint32_t flush_bits = 0;
   unsigned size = 0;

   barrier.layout_transitions.init_mask_ram = 1;
   radv_describe_layout_transition(cmd_buffer, &barrier);

   flush_bits |= radv_clear_dcc(cmd_buffer, image, range, value);

   if (cmd_buffer->device->physical_device->rad_info.gfx_level == GFX8) {
      /* When DCC is enabled with mipmaps, some levels might not
       * support fast clears and we have to initialize them as "fully
       * expanded".
       */
      /* Compute the size of all fast clearable DCC levels. */
      for (unsigned i = 0; i < image->planes[0].surface.num_meta_levels; i++) {
         struct legacy_surf_dcc_level *dcc_level = &image->planes[0].surface.u.legacy.color.dcc_level[i];
         unsigned dcc_fast_clear_size =
            dcc_level->dcc_slice_fast_clear_size * image->info.array_size;

         if (!dcc_fast_clear_size)
            break;

         size = dcc_level->dcc_offset + dcc_fast_clear_size;
      }

      /* Initialize the mipmap levels without DCC. */
      if (size != image->planes[0].surface.meta_size) {
         flush_bits |= radv_fill_buffer(cmd_buffer, image, image->bindings[0].bo,
                                        radv_buffer_get_va(image->bindings[0].bo) +
                                           image->bindings[0].offset +
                                           image->planes[0].surface.meta_offset + size,
                                        image->planes[0].surface.meta_size - size, 0xffffffff);
      }
   }

   return flush_bits;
}

/**
 * Initialize DCC/FMASK/CMASK metadata for a color image.
 */
static void
radv_init_color_image_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                               VkImageLayout src_layout, VkImageLayout dst_layout,
                               unsigned src_queue_mask, unsigned dst_queue_mask,
                               const VkImageSubresourceRange *range)
{
   uint32_t flush_bits = 0;

   /* Transitioning from LAYOUT_UNDEFINED layout not everyone is
    * consistent in considering previous rendering work for WAW hazards.
    */
   cmd_buffer->state.flush_bits |=
      radv_src_access_flush(cmd_buffer, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, image);

   if (radv_image_has_cmask(image)) {
      uint32_t value;

      if (cmd_buffer->device->physical_device->rad_info.gfx_level == GFX9) {
         /* TODO: Fix clearing CMASK layers on GFX9. */
         if (radv_image_is_tc_compat_cmask(image) ||
             (radv_image_has_fmask(image) &&
              radv_layout_can_fast_clear(cmd_buffer->device, image, range->baseMipLevel, dst_layout,
                                         dst_queue_mask))) {
            value = 0xccccccccu;
         } else {
            value = 0xffffffffu;
         }
      } else {
         static const uint32_t cmask_clear_values[4] = {0xffffffff, 0xdddddddd, 0xeeeeeeee, 0xffffffff};
         uint32_t log2_samples = util_logbase2(image->info.samples);

         value = cmask_clear_values[log2_samples];
      }

      flush_bits |= radv_init_cmask(cmd_buffer, image, range, value);
   }

   if (radv_image_has_fmask(image)) {
      flush_bits |= radv_init_fmask(cmd_buffer, image, range);
   }

   if (radv_dcc_enabled(image, range->baseMipLevel)) {
      uint32_t value = 0xffffffffu; /* Fully expanded mode. */

      if (radv_layout_dcc_compressed(cmd_buffer->device, image, range->baseMipLevel,
                                     dst_layout, dst_queue_mask)) {
         value = 0u;
      }

      flush_bits |= radv_init_dcc(cmd_buffer, image, range, value);
   }

   if (radv_image_has_cmask(image) || radv_dcc_enabled(image, range->baseMipLevel)) {
      radv_update_fce_metadata(cmd_buffer, image, range, false);

      uint32_t color_values[2] = {0};
      radv_set_color_clear_metadata(cmd_buffer, image, range, color_values);
   }

   cmd_buffer->state.flush_bits |= flush_bits;
}

static void
radv_retile_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                       VkImageLayout src_layout, VkImageLayout dst_layout, unsigned dst_queue_mask)
{
   /* If the image is read-only, we don't have to retile DCC because it can't change. */
   if (!(image->vk.usage & RADV_IMAGE_USAGE_WRITE_BITS))
      return;

   if (src_layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
       (dst_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
        (dst_queue_mask & (1u << RADV_QUEUE_FOREIGN))))
      radv_retile_dcc(cmd_buffer, image);
}

static bool
radv_image_need_retile(const struct radv_image *image)
{
   return image->planes[0].surface.display_dcc_offset &&
          image->planes[0].surface.display_dcc_offset != image->planes[0].surface.meta_offset;
}

/**
 * Handle color image transitions for DCC/FMASK/CMASK.
 */
static void
radv_handle_color_image_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                   VkImageLayout src_layout, VkImageLayout dst_layout,
                                   unsigned src_queue_mask, unsigned dst_queue_mask,
                                   const VkImageSubresourceRange *range)
{
   bool dcc_decompressed = false, fast_clear_flushed = false;

   if (!radv_image_has_cmask(image) && !radv_image_has_fmask(image) &&
       !radv_dcc_enabled(image, range->baseMipLevel))
      return;

   if (src_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
      radv_init_color_image_metadata(cmd_buffer, image, src_layout, dst_layout,
                                     src_queue_mask, dst_queue_mask, range);

      if (radv_image_need_retile(image))
         radv_retile_transition(cmd_buffer, image, src_layout, dst_layout, dst_queue_mask);
      return;
   }

   if (radv_dcc_enabled(image, range->baseMipLevel)) {
      if (src_layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
         cmd_buffer->state.flush_bits |= radv_init_dcc(cmd_buffer, image, range, 0xffffffffu);
      } else if (radv_layout_dcc_compressed(cmd_buffer->device, image, range->baseMipLevel,
                                            src_layout, src_queue_mask) &&
                 !radv_layout_dcc_compressed(cmd_buffer->device, image, range->baseMipLevel,
                                             dst_layout, dst_queue_mask)) {
         radv_decompress_dcc(cmd_buffer, image, range);
         dcc_decompressed = true;
      } else if (radv_layout_can_fast_clear(cmd_buffer->device, image, range->baseMipLevel,
                                            src_layout, src_queue_mask) &&
                 !radv_layout_can_fast_clear(cmd_buffer->device, image, range->baseMipLevel,
                                             dst_layout, dst_queue_mask)) {
         radv_fast_clear_flush_image_inplace(cmd_buffer, image, range);
         fast_clear_flushed = true;
      }

      if (radv_image_need_retile(image))
         radv_retile_transition(cmd_buffer, image, src_layout, dst_layout, dst_queue_mask);
   } else if (radv_image_has_cmask(image) || radv_image_has_fmask(image)) {
      if (radv_layout_can_fast_clear(cmd_buffer->device, image, range->baseMipLevel,
                                     src_layout, src_queue_mask) &&
          !radv_layout_can_fast_clear(cmd_buffer->device, image, range->baseMipLevel,
                                      dst_layout, dst_queue_mask)) {
         radv_fast_clear_flush_image_inplace(cmd_buffer, image, range);
         fast_clear_flushed = true;
      }
   }

   /* MSAA color decompress. */
   if (radv_image_has_fmask(image) &&
       (image->vk.usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) &&
       radv_layout_fmask_compressed(cmd_buffer->device, image, src_layout, src_queue_mask) &&
       !radv_layout_fmask_compressed(cmd_buffer->device, image, dst_layout, dst_queue_mask)) {
      if (radv_dcc_enabled(image, range->baseMipLevel) &&
          !radv_image_use_dcc_image_stores(cmd_buffer->device, image) && !dcc_decompressed) {
         /* A DCC decompress is required before expanding FMASK
          * when DCC stores aren't supported to avoid being in
          * a state where DCC is compressed and the main
          * surface is uncompressed.
          */
         radv_decompress_dcc(cmd_buffer, image, range);
      } else if (!fast_clear_flushed) {
         /* A FMASK decompress is required before expanding
          * FMASK.
          */
         radv_fast_clear_flush_image_inplace(cmd_buffer, image, range);
      }

      struct radv_barrier_data barrier = {0};
      barrier.layout_transitions.fmask_color_expand = 1;
      radv_describe_layout_transition(cmd_buffer, &barrier);

      radv_expand_fmask_image_inplace(cmd_buffer, image, range);
   }
}

static void
radv_handle_image_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                             VkImageLayout src_layout, VkImageLayout dst_layout,
                             uint32_t src_family_index, uint32_t dst_family_index,
                             const VkImageSubresourceRange *range,
                             struct radv_sample_locations_state *sample_locs)
{
   enum radv_queue_family src_qf = vk_queue_to_radv(cmd_buffer->device->physical_device, src_family_index);
   enum radv_queue_family dst_qf = vk_queue_to_radv(cmd_buffer->device->physical_device, dst_family_index);
   if (image->exclusive && src_family_index != dst_family_index) {
      /* This is an acquire or a release operation and there will be
       * a corresponding release/acquire. Do the transition in the
       * most flexible queue. */

      assert(src_qf == cmd_buffer->qf ||
             dst_qf == cmd_buffer->qf);

      if (src_family_index == VK_QUEUE_FAMILY_EXTERNAL || src_family_index == VK_QUEUE_FAMILY_FOREIGN_EXT)
         return;

      if (cmd_buffer->qf == RADV_QUEUE_TRANSFER)
         return;

      if (cmd_buffer->qf == RADV_QUEUE_COMPUTE &&
          (src_qf == RADV_QUEUE_GENERAL || dst_qf == RADV_QUEUE_GENERAL))
         return;
   }

   unsigned src_queue_mask =
      radv_image_queue_family_mask(image, src_qf, cmd_buffer->qf);
   unsigned dst_queue_mask =
      radv_image_queue_family_mask(image, dst_qf, cmd_buffer->qf);

   if (src_layout == dst_layout && src_queue_mask == dst_queue_mask)
      return;

   if (image->vk.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
      radv_handle_depth_image_transition(cmd_buffer, image, src_layout, dst_layout,
                                         src_queue_mask, dst_queue_mask, range, sample_locs);
   } else {
      radv_handle_color_image_transition(cmd_buffer, image, src_layout, dst_layout,
                                         src_queue_mask, dst_queue_mask, range);
   }
}

static void
radv_cp_dma_wait_for_stages(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 stage_mask)
{
   /* Make sure CP DMA is idle because the driver might have performed a DMA operation for copying a
    * buffer (or a MSAA image using FMASK). Note that updating a buffer is considered a clear
    * operation but it might also use a CP DMA copy in some rare situations. Other operations using
    * a CP DMA clear are implicitly synchronized (see CP_DMA_SYNC).
    */
   if (stage_mask & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT |
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT))
      si_cp_dma_wait_for_idle(cmd_buffer);
}

static void
radv_barrier(struct radv_cmd_buffer *cmd_buffer, const VkDependencyInfo *dep_info,
             enum rgp_barrier_reason reason)
{
   enum radv_cmd_flush_bits src_flush_bits = 0;
   enum radv_cmd_flush_bits dst_flush_bits = 0;
   VkPipelineStageFlags2 src_stage_mask = 0;
   VkPipelineStageFlags2 dst_stage_mask = 0;

   if (cmd_buffer->state.render.active)
      radv_mark_noncoherent_rb(cmd_buffer);

   radv_describe_barrier_start(cmd_buffer, reason);

   for (uint32_t i = 0; i < dep_info->memoryBarrierCount; i++) {
      src_stage_mask |= dep_info->pMemoryBarriers[i].srcStageMask;
      src_flush_bits |=
         radv_src_access_flush(cmd_buffer, dep_info->pMemoryBarriers[i].srcAccessMask, NULL);
      dst_stage_mask |= dep_info->pMemoryBarriers[i].dstStageMask;
      dst_flush_bits |=
         radv_dst_access_flush(cmd_buffer, dep_info->pMemoryBarriers[i].dstAccessMask, NULL);
   }

   for (uint32_t i = 0; i < dep_info->bufferMemoryBarrierCount; i++) {
      src_stage_mask |= dep_info->pBufferMemoryBarriers[i].srcStageMask;
      src_flush_bits |=
         radv_src_access_flush(cmd_buffer, dep_info->pBufferMemoryBarriers[i].srcAccessMask, NULL);
      dst_stage_mask |= dep_info->pBufferMemoryBarriers[i].dstStageMask;
      dst_flush_bits |=
         radv_dst_access_flush(cmd_buffer, dep_info->pBufferMemoryBarriers[i].dstAccessMask, NULL);
   }

   for (uint32_t i = 0; i < dep_info->imageMemoryBarrierCount; i++) {
      RADV_FROM_HANDLE(radv_image, image, dep_info->pImageMemoryBarriers[i].image);

      src_stage_mask |= dep_info->pImageMemoryBarriers[i].srcStageMask;
      src_flush_bits |=
         radv_src_access_flush(cmd_buffer, dep_info->pImageMemoryBarriers[i].srcAccessMask, image);
      dst_stage_mask |= dep_info->pImageMemoryBarriers[i].dstStageMask;
      dst_flush_bits |=
         radv_dst_access_flush(cmd_buffer, dep_info->pImageMemoryBarriers[i].dstAccessMask, image);
   }

   /* The Vulkan spec 1.1.98 says:
    *
    * "An execution dependency with only
    *  VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT in the destination stage mask
    *  will only prevent that stage from executing in subsequently
    *  submitted commands. As this stage does not perform any actual
    *  execution, this is not observable - in effect, it does not delay
    *  processing of subsequent commands. Similarly an execution dependency
    *  with only VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT in the source stage mask
    *  will effectively not wait for any prior commands to complete."
    */
   if (dst_stage_mask != VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT)
      radv_stage_flush(cmd_buffer, src_stage_mask);
   cmd_buffer->state.flush_bits |= src_flush_bits;

   radv_ace_internal_barrier(cmd_buffer, src_stage_mask, 0);

   for (uint32_t i = 0; i < dep_info->imageMemoryBarrierCount; i++) {
      RADV_FROM_HANDLE(radv_image, image, dep_info->pImageMemoryBarriers[i].image);

      const struct VkSampleLocationsInfoEXT *sample_locs_info =
         vk_find_struct_const(dep_info->pImageMemoryBarriers[i].pNext, SAMPLE_LOCATIONS_INFO_EXT);
      struct radv_sample_locations_state sample_locations;

      if (sample_locs_info) {
         assert(image->vk.create_flags & VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT);
         sample_locations.per_pixel = sample_locs_info->sampleLocationsPerPixel;
         sample_locations.grid_size = sample_locs_info->sampleLocationGridSize;
         sample_locations.count = sample_locs_info->sampleLocationsCount;
         typed_memcpy(&sample_locations.locations[0], sample_locs_info->pSampleLocations,
                      sample_locs_info->sampleLocationsCount);
      }

      radv_handle_image_transition(
         cmd_buffer, image, dep_info->pImageMemoryBarriers[i].oldLayout,
         dep_info->pImageMemoryBarriers[i].newLayout,
         dep_info->pImageMemoryBarriers[i].srcQueueFamilyIndex,
         dep_info->pImageMemoryBarriers[i].dstQueueFamilyIndex,
         &dep_info->pImageMemoryBarriers[i].subresourceRange, sample_locs_info ? &sample_locations : NULL);
   }

   radv_ace_internal_barrier(cmd_buffer, 0, dst_stage_mask);
   radv_cp_dma_wait_for_stages(cmd_buffer, src_stage_mask);

   cmd_buffer->state.flush_bits |= dst_flush_bits;

   radv_describe_barrier_end(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                         const VkDependencyInfo *pDependencyInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   radv_barrier(cmd_buffer, pDependencyInfo, RGP_BARRIER_EXTERNAL_CMD_PIPELINE_BARRIER);
}

static void
write_event(struct radv_cmd_buffer *cmd_buffer, struct radv_event *event,
            VkPipelineStageFlags2 stageMask, unsigned value)
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint64_t va = radv_buffer_get_va(event->bo);

   si_emit_cache_flush(cmd_buffer);

   radv_cs_add_buffer(cmd_buffer->device->ws, cs, event->bo);

   ASSERTED unsigned cdw_max = radeon_check_space(cmd_buffer->device->ws, cs, 28);

   if (stageMask & (VK_PIPELINE_STAGE_2_COPY_BIT |
                    VK_PIPELINE_STAGE_2_RESOLVE_BIT |
                    VK_PIPELINE_STAGE_2_BLIT_BIT |
                    VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
      /* Be conservative for now. */
      stageMask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
   }

   /* Flags that only require a top-of-pipe event. */
   VkPipelineStageFlags2 top_of_pipe_flags = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;

   /* Flags that only require a post-index-fetch event. */
   VkPipelineStageFlags2 post_index_fetch_flags =
      top_of_pipe_flags | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;

   /* Flags that only require signaling post PS. */
   VkPipelineStageFlags2 post_ps_flags =
      post_index_fetch_flags | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
      VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
      VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
      VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT |
      VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT |
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR |
      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

   /* Flags that only require signaling post CS. */
   VkPipelineStageFlags2 post_cs_flags = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

   radv_cp_dma_wait_for_stages(cmd_buffer, stageMask);

   if (!(stageMask & ~top_of_pipe_flags)) {
      /* Just need to sync the PFP engine. */
      radeon_emit(cs, PKT3(PKT3_WRITE_DATA, 3, 0));
      radeon_emit(cs, S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(V_370_PFP));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, value);
   } else if (!(stageMask & ~post_index_fetch_flags)) {
      /* Sync ME because PFP reads index and indirect buffers. */
      radeon_emit(cs, PKT3(PKT3_WRITE_DATA, 3, 0));
      radeon_emit(cs, S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(V_370_ME));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, value);
   } else {
      unsigned event_type;

      if (!(stageMask & ~post_ps_flags)) {
         /* Sync previous fragment shaders. */
         event_type = V_028A90_PS_DONE;
      } else if (!(stageMask & ~post_cs_flags)) {
         /* Sync previous compute shaders. */
         event_type = V_028A90_CS_DONE;
      } else {
         /* Otherwise, sync all prior GPU work. */
         event_type = V_028A90_BOTTOM_OF_PIPE_TS;
      }

      si_cs_emit_write_event_eop(cs, cmd_buffer->device->physical_device->rad_info.gfx_level,
                                 radv_cmd_buffer_uses_mec(cmd_buffer), event_type, 0,
                                 EOP_DST_SEL_MEM, EOP_DATA_SEL_VALUE_32BIT, va, value,
                                 cmd_buffer->gfx9_eop_bug_va);
   }

   assert(cmd_buffer->cs->cdw <= cdw_max);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent _event,
                  const VkDependencyInfo* pDependencyInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_event, event, _event);
   VkPipelineStageFlags2 src_stage_mask = 0;

   for (uint32_t i = 0; i < pDependencyInfo->memoryBarrierCount; i++)
      src_stage_mask |= pDependencyInfo->pMemoryBarriers[i].srcStageMask;
   for (uint32_t i = 0; i < pDependencyInfo->bufferMemoryBarrierCount; i++)
      src_stage_mask |= pDependencyInfo->pBufferMemoryBarriers[i].srcStageMask;
   for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; i++)
      src_stage_mask |= pDependencyInfo->pImageMemoryBarriers[i].srcStageMask;

   write_event(cmd_buffer, event, src_stage_mask, 1);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdResetEvent2(VkCommandBuffer commandBuffer, VkEvent _event,
                    VkPipelineStageFlags2 stageMask)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_event, event, _event);

   write_event(cmd_buffer, event, stageMask, 0);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdWaitEvents2(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents,
                    const VkDependencyInfo* pDependencyInfos)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   for (unsigned i = 0; i < eventCount; ++i) {
      RADV_FROM_HANDLE(radv_event, event, pEvents[i]);
      uint64_t va = radv_buffer_get_va(event->bo);

      radv_cs_add_buffer(cmd_buffer->device->ws, cs, event->bo);

      ASSERTED unsigned cdw_max = radeon_check_space(cmd_buffer->device->ws, cs, 7);

      radv_cp_wait_mem(cs, WAIT_REG_MEM_EQUAL, va, 1, 0xffffffff);
      assert(cmd_buffer->cs->cdw <= cdw_max);
   }

   radv_barrier(cmd_buffer, pDependencyInfos, RGP_BARRIER_EXTERNAL_CMD_WAIT_EVENTS);
}

/* VK_EXT_conditional_rendering */
VKAPI_ATTR void VKAPI_CALL
radv_CmdBeginConditionalRenderingEXT(
   VkCommandBuffer commandBuffer,
   const VkConditionalRenderingBeginInfoEXT *pConditionalRenderingBegin)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, buffer, pConditionalRenderingBegin->buffer);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   unsigned pred_op = PREDICATION_OP_BOOL32;
   bool draw_visible = true;
   uint64_t va;

   va = radv_buffer_get_va(buffer->bo) + buffer->offset + pConditionalRenderingBegin->offset;

   /* By default, if the 32-bit value at offset in buffer memory is zero,
    * then the rendering commands are discarded, otherwise they are
    * executed as normal. If the inverted flag is set, all commands are
    * discarded if the value is non zero.
    */
   if (pConditionalRenderingBegin->flags & VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT) {
      draw_visible = false;
   }

   si_emit_cache_flush(cmd_buffer);

   if (cmd_buffer->qf == RADV_QUEUE_GENERAL &&
       !cmd_buffer->device->physical_device->rad_info.has_32bit_predication) {
      uint64_t pred_value = 0, pred_va;
      unsigned pred_offset;

      /* From the Vulkan spec 1.1.107:
       *
       * "If the 32-bit value at offset in buffer memory is zero,
       *  then the rendering commands are discarded, otherwise they
       *  are executed as normal. If the value of the predicate in
       *  buffer memory changes while conditional rendering is
       *  active, the rendering commands may be discarded in an
       *  implementation-dependent way. Some implementations may
       *  latch the value of the predicate upon beginning conditional
       *  rendering while others may read it before every rendering
       *  command."
       *
       * But, the AMD hardware treats the predicate as a 64-bit
       * value which means we need a workaround in the driver.
       * Luckily, it's not required to support if the value changes
       * when predication is active.
       *
       * The workaround is as follows:
       * 1) allocate a 64-value in the upload BO and initialize it
       *    to 0
       * 2) copy the 32-bit predicate value to the upload BO
       * 3) use the new allocated VA address for predication
       *
       * Based on the conditionalrender demo, it's faster to do the
       * COPY_DATA in ME  (+ sync PFP) instead of PFP.
       */
      radv_cmd_buffer_upload_data(cmd_buffer, 8, &pred_value, &pred_offset);

      pred_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + pred_offset;

      radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
      radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) |
                         COPY_DATA_WR_CONFIRM);
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, pred_va);
      radeon_emit(cs, pred_va >> 32);

      radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
      radeon_emit(cs, 0);

      va = pred_va;
      pred_op = PREDICATION_OP_BOOL64;
   }

   /* MEC doesn't support predication, we emulate it elsewhere. */
   if (!radv_cmd_buffer_uses_mec(cmd_buffer)) {
      si_emit_set_predication_state(cmd_buffer, draw_visible, pred_op, va);
   }

   /* Store conditional rendering user info. */
   cmd_buffer->state.predicating = true;
   cmd_buffer->state.predication_type = draw_visible;
   cmd_buffer->state.predication_op = pred_op;
   cmd_buffer->state.predication_va = va;
   cmd_buffer->mec_inv_pred_emitted = false;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdEndConditionalRenderingEXT(VkCommandBuffer commandBuffer)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   /* MEC doesn't support predication, no need to emit anything here. */
   if (!radv_cmd_buffer_uses_mec(cmd_buffer)) {
      si_emit_set_predication_state(cmd_buffer, false, 0, 0);
   }

   /* Reset conditional rendering user info. */
   cmd_buffer->state.predicating = false;
   cmd_buffer->state.predication_type = -1;
   cmd_buffer->state.predication_op = 0;
   cmd_buffer->state.predication_va = 0;
   cmd_buffer->mec_inv_pred_emitted = false;
}

/* VK_EXT_transform_feedback */
VKAPI_ATTR void VKAPI_CALL
radv_CmdBindTransformFeedbackBuffersEXT(VkCommandBuffer commandBuffer, uint32_t firstBinding,
                                        uint32_t bindingCount, const VkBuffer *pBuffers,
                                        const VkDeviceSize *pOffsets, const VkDeviceSize *pSizes)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_streamout_binding *sb = cmd_buffer->streamout_bindings;
   uint8_t enabled_mask = 0;

   assert(firstBinding + bindingCount <= MAX_SO_BUFFERS);
   for (uint32_t i = 0; i < bindingCount; i++) {
      uint32_t idx = firstBinding + i;

      sb[idx].buffer = radv_buffer_from_handle(pBuffers[i]);
      sb[idx].offset = pOffsets[i];

      if (!pSizes || pSizes[i] == VK_WHOLE_SIZE) {
         sb[idx].size = sb[idx].buffer->vk.size - sb[idx].offset;
      } else {
         sb[idx].size = pSizes[i];
      }

      radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, sb[idx].buffer->bo);

      enabled_mask |= 1 << idx;
   }

   cmd_buffer->state.streamout.enabled_mask |= enabled_mask;

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_BUFFER;
}

void
radv_emit_streamout_enable(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   bool streamout_enabled = radv_is_streamout_enabled(cmd_buffer);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint32_t enabled_stream_buffers_mask = 0;

   if (pipeline && pipeline->streamout_shader) {
      enabled_stream_buffers_mask = pipeline->streamout_shader->info.so.enabled_stream_buffers_mask;
   }

   radeon_set_context_reg_seq(cs, R_028B94_VGT_STRMOUT_CONFIG, 2);
   radeon_emit(cs, S_028B94_STREAMOUT_0_EN(streamout_enabled) | S_028B94_RAST_STREAM(0) |
                      S_028B94_STREAMOUT_1_EN(streamout_enabled) |
                      S_028B94_STREAMOUT_2_EN(streamout_enabled) |
                      S_028B94_STREAMOUT_3_EN(streamout_enabled));
   radeon_emit(cs, so->hw_enabled_mask & enabled_stream_buffers_mask);

   cmd_buffer->state.context_roll_without_scissor_emitted = true;
}

static void
radv_set_streamout_enable(struct radv_cmd_buffer *cmd_buffer, bool enable)
{
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   bool old_streamout_enabled = radv_is_streamout_enabled(cmd_buffer);
   uint32_t old_hw_enabled_mask = so->hw_enabled_mask;

   so->streamout_enabled = enable;

   so->hw_enabled_mask = so->enabled_mask | (so->enabled_mask << 4) | (so->enabled_mask << 8) |
                         (so->enabled_mask << 12);

   if (!cmd_buffer->device->physical_device->use_ngg_streamout &&
       ((old_streamout_enabled != radv_is_streamout_enabled(cmd_buffer)) ||
        (old_hw_enabled_mask != so->hw_enabled_mask)))
      radv_emit_streamout_enable(cmd_buffer);

   if (cmd_buffer->device->physical_device->use_ngg_streamout) {
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_NGG_QUERY;

      /* Re-emit streamout buffers to unbind them. */
      if (!enable)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_BUFFER;
   }
}

static void
radv_flush_vgt_streamout(struct radv_cmd_buffer *cmd_buffer)
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   unsigned reg_strmout_cntl;

   /* The register is at different places on different ASICs. */
   if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX9) {
      reg_strmout_cntl = R_0300FC_CP_STRMOUT_CNTL;
      radeon_emit(cs, PKT3(PKT3_WRITE_DATA, 3, 0));
      radeon_emit(cs, S_370_DST_SEL(V_370_MEM_MAPPED_REGISTER) | S_370_ENGINE_SEL(V_370_ME));
      radeon_emit(cs, R_0300FC_CP_STRMOUT_CNTL >> 2);
      radeon_emit(cs, 0);
      radeon_emit(cs, 0);
   } else if (cmd_buffer->device->physical_device->rad_info.gfx_level >= GFX7) {
      reg_strmout_cntl = R_0300FC_CP_STRMOUT_CNTL;
      radeon_set_uconfig_reg(cs, reg_strmout_cntl, 0);
   } else {
      reg_strmout_cntl = R_0084FC_CP_STRMOUT_CNTL;
      radeon_set_config_reg(cs, reg_strmout_cntl, 0);
   }

   radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
   radeon_emit(cs, EVENT_TYPE(V_028A90_SO_VGTSTREAMOUT_FLUSH) | EVENT_INDEX(0));

   radeon_emit(cs, PKT3(PKT3_WAIT_REG_MEM, 5, 0));
   radeon_emit(cs,
               WAIT_REG_MEM_EQUAL); /* wait until the register is equal to the reference value */
   radeon_emit(cs, reg_strmout_cntl >> 2); /* register */
   radeon_emit(cs, 0);
   radeon_emit(cs, S_0084FC_OFFSET_UPDATE_DONE(1)); /* reference value */
   radeon_emit(cs, S_0084FC_OFFSET_UPDATE_DONE(1)); /* mask */
   radeon_emit(cs, 4);                              /* poll interval */
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBeginTransformFeedbackEXT(VkCommandBuffer commandBuffer, uint32_t firstCounterBuffer,
                                  uint32_t counterBufferCount, const VkBuffer *pCounterBuffers,
                                  const VkDeviceSize *pCounterBufferOffsets)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_streamout_binding *sb = cmd_buffer->streamout_bindings;
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radv_shader_info *info = &pipeline->streamout_shader->info;
   unsigned last_target = util_last_bit(so->enabled_mask) - 1;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   assert(firstCounterBuffer + counterBufferCount <= MAX_SO_BUFFERS);
   if (cmd_buffer->device->physical_device->use_ngg_streamout) {
      /* Sync because the next streamout operation will overwrite GDS and we
       * have to make sure it's idle.
       * TODO: Improve by tracking if there is a streamout operation in
       * flight.
       */
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VS_PARTIAL_FLUSH;
      si_emit_cache_flush(cmd_buffer);
   } else {
      radv_flush_vgt_streamout(cmd_buffer);
   }

   u_foreach_bit(i, so->enabled_mask)
   {
      int32_t counter_buffer_idx = i - firstCounterBuffer;
      if (counter_buffer_idx >= 0 && counter_buffer_idx >= counterBufferCount)
         counter_buffer_idx = -1;

      bool append =
         counter_buffer_idx >= 0 && pCounterBuffers && pCounterBuffers[counter_buffer_idx];
      uint64_t va = 0;

      if (append) {
         RADV_FROM_HANDLE(radv_buffer, buffer, pCounterBuffers[counter_buffer_idx]);
         uint64_t counter_buffer_offset = 0;

         if (pCounterBufferOffsets)
            counter_buffer_offset = pCounterBufferOffsets[counter_buffer_idx];

         va += radv_buffer_get_va(buffer->bo);
         va += buffer->offset + counter_buffer_offset;

         radv_cs_add_buffer(cmd_buffer->device->ws, cs, buffer->bo);
      }

      if (cmd_buffer->device->physical_device->use_ngg_streamout) {
         radeon_emit(cs, PKT3(PKT3_DMA_DATA, 5, 0));
         radeon_emit(cs, S_411_SRC_SEL(append ? V_411_SRC_ADDR_TC_L2 : V_411_DATA) |
                            S_411_DST_SEL(V_411_GDS) | S_411_CP_SYNC(i == last_target));
         radeon_emit(cs, va);
         radeon_emit(cs, va >> 32);
         radeon_emit(cs, 4 * i); /* destination in GDS */
         radeon_emit(cs, 0);
         radeon_emit(cs, S_415_BYTE_COUNT_GFX9(4) | S_415_DISABLE_WR_CONFIRM_GFX9(i != last_target));
      } else {
         /* AMD GCN binds streamout buffers as shader resources.
          * VGT only counts primitives and tells the shader through
          * SGPRs what to do.
          */
         radeon_set_context_reg_seq(cs, R_028AD0_VGT_STRMOUT_BUFFER_SIZE_0 + 16 * i, 2);
         radeon_emit(cs, sb[i].size >> 2);      /* BUFFER_SIZE (in DW) */
         radeon_emit(cs, info->so.strides[i]);  /* VTX_STRIDE (in DW) */

         cmd_buffer->state.context_roll_without_scissor_emitted = true;

         if (append) {
            radeon_emit(cs, PKT3(PKT3_STRMOUT_BUFFER_UPDATE, 4, 0));
            radeon_emit(cs, STRMOUT_SELECT_BUFFER(i) | STRMOUT_DATA_TYPE(1) |    /* offset in bytes */
                            STRMOUT_OFFSET_SOURCE(STRMOUT_OFFSET_FROM_MEM));     /* control */
            radeon_emit(cs, 0);                                                  /* unused */
            radeon_emit(cs, 0);                                                  /* unused */
            radeon_emit(cs, va);                                                 /* src address lo */
            radeon_emit(cs, va >> 32);                                           /* src address hi */
         } else {
            /* Start from the beginning. */
            radeon_emit(cs, PKT3(PKT3_STRMOUT_BUFFER_UPDATE, 4, 0));
            radeon_emit(cs, STRMOUT_SELECT_BUFFER(i) | STRMOUT_DATA_TYPE(1) |    /* offset in bytes */
                            STRMOUT_OFFSET_SOURCE(STRMOUT_OFFSET_FROM_PACKET));  /* control */
            radeon_emit(cs, 0);                                                  /* unused */
            radeon_emit(cs, 0);                                                  /* unused */
            radeon_emit(cs, 0);                                                  /* unused */
            radeon_emit(cs, 0);                                                  /* unused */
         }
      }
   }

   radv_set_streamout_enable(cmd_buffer, true);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdEndTransformFeedbackEXT(VkCommandBuffer commandBuffer, uint32_t firstCounterBuffer,
                                uint32_t counterBufferCount, const VkBuffer *pCounterBuffers,
                                const VkDeviceSize *pCounterBufferOffsets)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   assert(firstCounterBuffer + counterBufferCount <= MAX_SO_BUFFERS);

   if (!cmd_buffer->device->physical_device->use_ngg_streamout)
      radv_flush_vgt_streamout(cmd_buffer);

   u_foreach_bit(i, so->enabled_mask)
   {
      int32_t counter_buffer_idx = i - firstCounterBuffer;
      if (counter_buffer_idx >= 0 && counter_buffer_idx >= counterBufferCount)
         counter_buffer_idx = -1;

      bool append =
         counter_buffer_idx >= 0 && pCounterBuffers && pCounterBuffers[counter_buffer_idx];
      uint64_t va = 0;

      if (append) {
         RADV_FROM_HANDLE(radv_buffer, buffer, pCounterBuffers[counter_buffer_idx]);
         uint64_t counter_buffer_offset = 0;

         if (pCounterBufferOffsets)
            counter_buffer_offset = pCounterBufferOffsets[counter_buffer_idx];

         va += radv_buffer_get_va(buffer->bo);
         va += buffer->offset + counter_buffer_offset;

         radv_cs_add_buffer(cmd_buffer->device->ws, cs, buffer->bo);
      }

      if (cmd_buffer->device->physical_device->use_ngg_streamout) {
         if (append) {
            si_cs_emit_write_event_eop(cs, cmd_buffer->device->physical_device->rad_info.gfx_level,
                                       radv_cmd_buffer_uses_mec(cmd_buffer), V_028A90_PS_DONE, 0,
                                       EOP_DST_SEL_TC_L2, EOP_DATA_SEL_GDS, va, EOP_DATA_GDS(i, 1), 0);
         }
      } else {
         if (append) {
            radeon_emit(cs, PKT3(PKT3_STRMOUT_BUFFER_UPDATE, 4, 0));
            radeon_emit(cs, STRMOUT_SELECT_BUFFER(i) | STRMOUT_DATA_TYPE(1) | /* offset in bytes */
                            STRMOUT_OFFSET_SOURCE(STRMOUT_OFFSET_NONE) |
                            STRMOUT_STORE_BUFFER_FILLED_SIZE);                /* control */
            radeon_emit(cs, va);                                              /* dst address lo */
            radeon_emit(cs, va >> 32);                                        /* dst address hi */
            radeon_emit(cs, 0);                                               /* unused */
            radeon_emit(cs, 0);                                               /* unused */
         }

         /* Deactivate transform feedback by zeroing the buffer size.
          * The counters (primitives generated, primitives emitted) may
          * be enabled even if there is not buffer bound. This ensures
          * that the primitives-emitted query won't increment.
          */
         radeon_set_context_reg(cs, R_028AD0_VGT_STRMOUT_BUFFER_SIZE_0 + 16 * i, 0);

         cmd_buffer->state.context_roll_without_scissor_emitted = true;
      }
   }

   radv_set_streamout_enable(cmd_buffer, false);
}

static void
radv_emit_strmout_buffer(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *draw_info)
{
   const enum amd_gfx_level gfx_level = cmd_buffer->device->physical_device->rad_info.gfx_level;
   uint64_t va = radv_buffer_get_va(draw_info->strmout_buffer->bo);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;

   va += draw_info->strmout_buffer->offset + draw_info->strmout_buffer_offset;

   radeon_set_context_reg(cs, R_028B30_VGT_STRMOUT_DRAW_OPAQUE_VERTEX_STRIDE, draw_info->stride);

   if (gfx_level >= GFX10) {
      /* Emitting a COPY_DATA packet should be enough because RADV doesn't support preemption
       * (shadow memory) but for unknown reasons, it can lead to GPU hangs on GFX10+.
       */
      radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
      radeon_emit(cs, 0);

      radeon_emit(cs, PKT3(PKT3_LOAD_CONTEXT_REG_INDEX, 3, 0));
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, (R_028B2C_VGT_STRMOUT_DRAW_OPAQUE_BUFFER_FILLED_SIZE - SI_CONTEXT_REG_OFFSET) >> 2);
      radeon_emit(cs, 1); /* 1 DWORD */
   } else {
      radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
      radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_REG) |
                      COPY_DATA_WR_CONFIRM);
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
      radeon_emit(cs, R_028B2C_VGT_STRMOUT_DRAW_OPAQUE_BUFFER_FILLED_SIZE >> 2);
      radeon_emit(cs, 0); /* unused */
   }

   radv_cs_add_buffer(cmd_buffer->device->ws, cs, draw_info->strmout_buffer->bo);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndirectByteCountEXT(VkCommandBuffer commandBuffer, uint32_t instanceCount,
                                 uint32_t firstInstance, VkBuffer _counterBuffer,
                                 VkDeviceSize counterBufferOffset, uint32_t counterOffset,
                                 uint32_t vertexStride)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, counterBuffer, _counterBuffer);
   struct radv_draw_info info;

   info.count = 0;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_buffer = counterBuffer;
   info.strmout_buffer_offset = counterBufferOffset;
   info.stride = vertexStride;
   info.indexed = false;
   info.indirect = NULL;

   if (!radv_before_draw(cmd_buffer, &info, 1))
      return;
   struct VkMultiDrawInfoEXT minfo = { 0, 0 };
   radv_emit_strmout_buffer(cmd_buffer, &info);
   radv_emit_direct_draw_packets(cmd_buffer, &info, 1, &minfo, S_0287F0_USE_OPAQUE(1), 0);
   radv_after_draw(cmd_buffer);
}

/* VK_AMD_buffer_marker */
VKAPI_ATTR void VKAPI_CALL
radv_CmdWriteBufferMarker2AMD(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage,
                              VkBuffer dstBuffer, VkDeviceSize dstOffset, uint32_t marker)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, buffer, dstBuffer);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   uint64_t va = radv_buffer_get_va(buffer->bo) + buffer->offset + dstOffset;

   si_emit_cache_flush(cmd_buffer);

   ASSERTED unsigned cdw_max = radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 12);

   if (!(stage & ~VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
      radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
      radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_IMM) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) |
                         COPY_DATA_WR_CONFIRM);
      radeon_emit(cs, marker);
      radeon_emit(cs, 0);
      radeon_emit(cs, va);
      radeon_emit(cs, va >> 32);
   } else {
      si_cs_emit_write_event_eop(cs, cmd_buffer->device->physical_device->rad_info.gfx_level,
                                 radv_cmd_buffer_uses_mec(cmd_buffer), V_028A90_BOTTOM_OF_PIPE_TS,
                                 0, EOP_DST_SEL_MEM, EOP_DATA_SEL_VALUE_32BIT, va, marker,
                                 cmd_buffer->gfx9_eop_bug_va);
   }

   assert(cmd_buffer->cs->cdw <= cdw_max);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindPipelineShaderGroupNV(VkCommandBuffer commandBuffer,
                                  VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline,
                                  uint32_t groupIndex)
{
   fprintf(stderr, "radv: unimplemented vkCmdBindPipelineShaderGroupNV\n");
   abort();
}

/* VK_EXT_descriptor_buffer */
VKAPI_ATTR void VKAPI_CALL
radv_CmdBindDescriptorBuffersEXT(VkCommandBuffer commandBuffer, uint32_t bufferCount,
                                 const VkDescriptorBufferBindingInfoEXT *pBindingInfos)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   for (uint32_t i = 0; i < bufferCount; i++) {
      cmd_buffer->descriptor_buffers[i] = pBindingInfos[i].address;
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDescriptorBufferOffsetsEXT(VkCommandBuffer commandBuffer,
                                      VkPipelineBindPoint pipelineBindPoint,
                                      VkPipelineLayout _layout, uint32_t firstSet, uint32_t setCount,
                                      const uint32_t *pBufferIndices, const VkDeviceSize *pOffsets)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, pipelineBindPoint);

   for (unsigned i = 0; i < setCount; i++) {
      unsigned idx = i + firstSet;

      descriptors_state->descriptor_buffers[idx] =
         cmd_buffer->descriptor_buffers[pBufferIndices[i]] + pOffsets[i];

      radv_set_descriptor_set(cmd_buffer, pipelineBindPoint, NULL, idx);
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindDescriptorBufferEmbeddedSamplersEXT(VkCommandBuffer commandBuffer,
                                                VkPipelineBindPoint pipelineBindPoint,
                                                VkPipelineLayout _layout, uint32_t set)
{
   /* This is a no-op because embedded samplers are inlined at compile time. */
}
