/*
 * Copyright 2012 Advanced Micro Devices, Inc.
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

#include "ac_shader_util.h"
#include "ac_gpu_info.h"

#include "sid.h"
#include "util/u_math.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

unsigned ac_get_spi_shader_z_format(bool writes_z, bool writes_stencil, bool writes_samplemask,
                                    bool writes_mrt0_alpha)
{
   /* If writes_mrt0_alpha is true, one other flag must be true too. */
   assert(!writes_mrt0_alpha || writes_z || writes_stencil || writes_samplemask);

   if (writes_z || writes_mrt0_alpha) {
      /* Z needs 32 bits. */
      if (writes_samplemask || writes_mrt0_alpha)
         return V_028710_SPI_SHADER_32_ABGR;
      else if (writes_stencil)
         return V_028710_SPI_SHADER_32_GR;
      else
         return V_028710_SPI_SHADER_32_R;
   } else if (writes_stencil || writes_samplemask) {
      /* Both stencil and sample mask need only 16 bits. */
      return V_028710_SPI_SHADER_UINT16_ABGR;
   } else {
      return V_028710_SPI_SHADER_ZERO;
   }
}

unsigned ac_get_cb_shader_mask(unsigned spi_shader_col_format)
{
   unsigned i, cb_shader_mask = 0;

   for (i = 0; i < 8; i++) {
      switch ((spi_shader_col_format >> (i * 4)) & 0xf) {
      case V_028714_SPI_SHADER_ZERO:
         break;
      case V_028714_SPI_SHADER_32_R:
         cb_shader_mask |= 0x1 << (i * 4);
         break;
      case V_028714_SPI_SHADER_32_GR:
         cb_shader_mask |= 0x3 << (i * 4);
         break;
      case V_028714_SPI_SHADER_32_AR:
         cb_shader_mask |= 0x9u << (i * 4);
         break;
      case V_028714_SPI_SHADER_FP16_ABGR:
      case V_028714_SPI_SHADER_UNORM16_ABGR:
      case V_028714_SPI_SHADER_SNORM16_ABGR:
      case V_028714_SPI_SHADER_UINT16_ABGR:
      case V_028714_SPI_SHADER_SINT16_ABGR:
      case V_028714_SPI_SHADER_32_ABGR:
         cb_shader_mask |= 0xfu << (i * 4);
         break;
      default:
         assert(0);
      }
   }
   return cb_shader_mask;
}

/**
 * Calculate the appropriate setting of VGT_GS_MODE when \p shader is a
 * geometry shader.
 */
uint32_t ac_vgt_gs_mode(unsigned gs_max_vert_out, enum amd_gfx_level gfx_level)
{
   unsigned cut_mode;

   assert (gfx_level < GFX11);

   if (gs_max_vert_out <= 128) {
      cut_mode = V_028A40_GS_CUT_128;
   } else if (gs_max_vert_out <= 256) {
      cut_mode = V_028A40_GS_CUT_256;
   } else if (gs_max_vert_out <= 512) {
      cut_mode = V_028A40_GS_CUT_512;
   } else {
      assert(gs_max_vert_out <= 1024);
      cut_mode = V_028A40_GS_CUT_1024;
   }

   return S_028A40_MODE(V_028A40_GS_SCENARIO_G) | S_028A40_CUT_MODE(cut_mode) |
          S_028A40_ES_WRITE_OPTIMIZE(gfx_level <= GFX8) | S_028A40_GS_WRITE_OPTIMIZE(1) |
          S_028A40_ONCHIP(gfx_level >= GFX9 ? 1 : 0);
}

/// Translate a (dfmt, nfmt) pair into a chip-appropriate combined format
/// value for LLVM8+ tbuffer intrinsics.
unsigned ac_get_tbuffer_format(enum amd_gfx_level gfx_level, unsigned dfmt, unsigned nfmt)
{
   // Some games try to access vertex buffers without a valid format.
   // This is a game bug, but we should still handle it gracefully.
   if (dfmt == V_008F0C_GFX10_FORMAT_INVALID)
      return V_008F0C_GFX10_FORMAT_INVALID;

   if (gfx_level >= GFX11) {
      switch (dfmt) {
      default:
         unreachable("bad dfmt");
      case V_008F0C_BUF_DATA_FORMAT_INVALID:
         return V_008F0C_GFX11_FORMAT_INVALID;

      case V_008F0C_BUF_DATA_FORMAT_8:
         switch (nfmt) {
         case V_008F0C_BUF_NUM_FORMAT_UNORM:
            return V_008F0C_GFX11_FORMAT_8_UNORM;
         case V_008F0C_BUF_NUM_FORMAT_SNORM:
            return V_008F0C_GFX11_FORMAT_8_SNORM;
         case V_008F0C_BUF_NUM_FORMAT_USCALED:
            return V_008F0C_GFX11_FORMAT_8_USCALED;
         case V_008F0C_BUF_NUM_FORMAT_SSCALED:
            return V_008F0C_GFX11_FORMAT_8_SSCALED;
         default:
            unreachable("bad nfmt");
         case V_008F0C_BUF_NUM_FORMAT_UINT:
            return V_008F0C_GFX11_FORMAT_8_UINT;
         case V_008F0C_BUF_NUM_FORMAT_SINT:
            return V_008F0C_GFX11_FORMAT_8_SINT;
         }

      case V_008F0C_BUF_DATA_FORMAT_8_8:
         switch (nfmt) {
         case V_008F0C_BUF_NUM_FORMAT_UNORM:
            return V_008F0C_GFX11_FORMAT_8_8_UNORM;
         case V_008F0C_BUF_NUM_FORMAT_SNORM:
            return V_008F0C_GFX11_FORMAT_8_8_SNORM;
         case V_008F0C_BUF_NUM_FORMAT_USCALED:
            return V_008F0C_GFX11_FORMAT_8_8_USCALED;
         case V_008F0C_BUF_NUM_FORMAT_SSCALED:
            return V_008F0C_GFX11_FORMAT_8_8_SSCALED;
         default:
            unreachable("bad nfmt");
         case V_008F0C_BUF_NUM_FORMAT_UINT:
            return V_008F0C_GFX11_FORMAT_8_8_UINT;
         case V_008F0C_BUF_NUM_FORMAT_SINT:
            return V_008F0C_GFX11_FORMAT_8_8_SINT;
         }

      case V_008F0C_BUF_DATA_FORMAT_8_8_8_8:
         switch (nfmt) {
         case V_008F0C_BUF_NUM_FORMAT_UNORM:
            return V_008F0C_GFX11_FORMAT_8_8_8_8_UNORM;
         case V_008F0C_BUF_NUM_FORMAT_SNORM:
            return V_008F0C_GFX11_FORMAT_8_8_8_8_SNORM;
         case V_008F0C_BUF_NUM_FORMAT_USCALED:
            return V_008F0C_GFX11_FORMAT_8_8_8_8_USCALED;
         case V_008F0C_BUF_NUM_FORMAT_SSCALED:
            return V_008F0C_GFX11_FORMAT_8_8_8_8_SSCALED;
         default:
            unreachable("bad nfmt");
         case V_008F0C_BUF_NUM_FORMAT_UINT:
            return V_008F0C_GFX11_FORMAT_8_8_8_8_UINT;
         case V_008F0C_BUF_NUM_FORMAT_SINT:
            return V_008F0C_GFX11_FORMAT_8_8_8_8_SINT;
         }

      case V_008F0C_BUF_DATA_FORMAT_16:
         switch (nfmt) {
         case V_008F0C_BUF_NUM_FORMAT_UNORM:
            return V_008F0C_GFX11_FORMAT_16_UNORM;
         case V_008F0C_BUF_NUM_FORMAT_SNORM:
            return V_008F0C_GFX11_FORMAT_16_SNORM;
         case V_008F0C_BUF_NUM_FORMAT_USCALED:
            return V_008F0C_GFX11_FORMAT_16_USCALED;
         case V_008F0C_BUF_NUM_FORMAT_SSCALED:
            return V_008F0C_GFX11_FORMAT_16_SSCALED;
         default:
            unreachable("bad nfmt");
         case V_008F0C_BUF_NUM_FORMAT_UINT:
            return V_008F0C_GFX11_FORMAT_16_UINT;
         case V_008F0C_BUF_NUM_FORMAT_SINT:
            return V_008F0C_GFX11_FORMAT_16_SINT;
         case V_008F0C_BUF_NUM_FORMAT_FLOAT:
            return V_008F0C_GFX11_FORMAT_16_FLOAT;
         }

      case V_008F0C_BUF_DATA_FORMAT_16_16:
         switch (nfmt) {
         case V_008F0C_BUF_NUM_FORMAT_UNORM:
            return V_008F0C_GFX11_FORMAT_16_16_UNORM;
         case V_008F0C_BUF_NUM_FORMAT_SNORM:
            return V_008F0C_GFX11_FORMAT_16_16_SNORM;
         case V_008F0C_BUF_NUM_FORMAT_USCALED:
            return V_008F0C_GFX11_FORMAT_16_16_USCALED;
         case V_008F0C_BUF_NUM_FORMAT_SSCALED:
            return V_008F0C_GFX11_FORMAT_16_16_SSCALED;
         default:
            unreachable("bad nfmt");
         case V_008F0C_BUF_NUM_FORMAT_UINT:
            return V_008F0C_GFX11_FORMAT_16_16_UINT;
         case V_008F0C_BUF_NUM_FORMAT_SINT:
            return V_008F0C_GFX11_FORMAT_16_16_SINT;
         case V_008F0C_BUF_NUM_FORMAT_FLOAT:
            return V_008F0C_GFX11_FORMAT_16_16_FLOAT;
         }

      case V_008F0C_BUF_DATA_FORMAT_16_16_16_16:
         switch (nfmt) {
         case V_008F0C_BUF_NUM_FORMAT_UNORM:
            return V_008F0C_GFX11_FORMAT_16_16_16_16_UNORM;
         case V_008F0C_BUF_NUM_FORMAT_SNORM:
            return V_008F0C_GFX11_FORMAT_16_16_16_16_SNORM;
         case V_008F0C_BUF_NUM_FORMAT_USCALED:
            return V_008F0C_GFX11_FORMAT_16_16_16_16_USCALED;
         case V_008F0C_BUF_NUM_FORMAT_SSCALED:
            return V_008F0C_GFX11_FORMAT_16_16_16_16_SSCALED;
         default:
            unreachable("bad nfmt");
         case V_008F0C_BUF_NUM_FORMAT_UINT:
            return V_008F0C_GFX11_FORMAT_16_16_16_16_UINT;
         case V_008F0C_BUF_NUM_FORMAT_SINT:
            return V_008F0C_GFX11_FORMAT_16_16_16_16_SINT;
         case V_008F0C_BUF_NUM_FORMAT_FLOAT:
            return V_008F0C_GFX11_FORMAT_16_16_16_16_FLOAT;
         }

      case V_008F0C_BUF_DATA_FORMAT_32:
         switch (nfmt) {
         default:
            unreachable("bad nfmt");
         case V_008F0C_BUF_NUM_FORMAT_UINT:
            return V_008F0C_GFX11_FORMAT_32_UINT;
         case V_008F0C_BUF_NUM_FORMAT_SINT:
            return V_008F0C_GFX11_FORMAT_32_SINT;
         case V_008F0C_BUF_NUM_FORMAT_FLOAT:
            return V_008F0C_GFX11_FORMAT_32_FLOAT;
         }

      case V_008F0C_BUF_DATA_FORMAT_32_32:
         switch (nfmt) {
         default:
            unreachable("bad nfmt");
         case V_008F0C_BUF_NUM_FORMAT_UINT:
            return V_008F0C_GFX11_FORMAT_32_32_UINT;
         case V_008F0C_BUF_NUM_FORMAT_SINT:
            return V_008F0C_GFX11_FORMAT_32_32_SINT;
         case V_008F0C_BUF_NUM_FORMAT_FLOAT:
            return V_008F0C_GFX11_FORMAT_32_32_FLOAT;
         }

      case V_008F0C_BUF_DATA_FORMAT_32_32_32:
         switch (nfmt) {
         default:
            unreachable("bad nfmt");
         case V_008F0C_BUF_NUM_FORMAT_UINT:
            return V_008F0C_GFX11_FORMAT_32_32_32_UINT;
         case V_008F0C_BUF_NUM_FORMAT_SINT:
            return V_008F0C_GFX11_FORMAT_32_32_32_SINT;
         case V_008F0C_BUF_NUM_FORMAT_FLOAT:
            return V_008F0C_GFX11_FORMAT_32_32_32_FLOAT;
         }

      case V_008F0C_BUF_DATA_FORMAT_32_32_32_32:
         switch (nfmt) {
         default:
            unreachable("bad nfmt");
         case V_008F0C_BUF_NUM_FORMAT_UINT:
            return V_008F0C_GFX11_FORMAT_32_32_32_32_UINT;
         case V_008F0C_BUF_NUM_FORMAT_SINT:
            return V_008F0C_GFX11_FORMAT_32_32_32_32_SINT;
         case V_008F0C_BUF_NUM_FORMAT_FLOAT:
            return V_008F0C_GFX11_FORMAT_32_32_32_32_FLOAT;
         }

      case V_008F0C_BUF_DATA_FORMAT_2_10_10_10:
         switch (nfmt) {
         case V_008F0C_BUF_NUM_FORMAT_UNORM:
            return V_008F0C_GFX11_FORMAT_2_10_10_10_UNORM;
         case V_008F0C_BUF_NUM_FORMAT_SNORM:
            return V_008F0C_GFX11_FORMAT_2_10_10_10_SNORM;
         case V_008F0C_BUF_NUM_FORMAT_USCALED:
            return V_008F0C_GFX11_FORMAT_2_10_10_10_USCALED;
         case V_008F0C_BUF_NUM_FORMAT_SSCALED:
            return V_008F0C_GFX11_FORMAT_2_10_10_10_SSCALED;
         default:
            unreachable("bad nfmt");
         case V_008F0C_BUF_NUM_FORMAT_UINT:
            return V_008F0C_GFX11_FORMAT_2_10_10_10_UINT;
         case V_008F0C_BUF_NUM_FORMAT_SINT:
            return V_008F0C_GFX11_FORMAT_2_10_10_10_SINT;
         }

      case V_008F0C_BUF_DATA_FORMAT_10_11_11:
         switch (nfmt) {
         default:
            unreachable("bad nfmt");
         case V_008F0C_BUF_NUM_FORMAT_FLOAT:
            return V_008F0C_GFX11_FORMAT_10_11_11_FLOAT;
         }
      }
   } else if (gfx_level >= GFX10) {
      unsigned format;
      switch (dfmt) {
      default:
         unreachable("bad dfmt");
      case V_008F0C_BUF_DATA_FORMAT_INVALID:
         format = V_008F0C_GFX10_FORMAT_INVALID;
         break;
      case V_008F0C_BUF_DATA_FORMAT_8:
         format = V_008F0C_GFX10_FORMAT_8_UINT;
         break;
      case V_008F0C_BUF_DATA_FORMAT_8_8:
         format = V_008F0C_GFX10_FORMAT_8_8_UINT;
         break;
      case V_008F0C_BUF_DATA_FORMAT_8_8_8_8:
         format = V_008F0C_GFX10_FORMAT_8_8_8_8_UINT;
         break;
      case V_008F0C_BUF_DATA_FORMAT_16:
         format = V_008F0C_GFX10_FORMAT_16_UINT;
         break;
      case V_008F0C_BUF_DATA_FORMAT_16_16:
         format = V_008F0C_GFX10_FORMAT_16_16_UINT;
         break;
      case V_008F0C_BUF_DATA_FORMAT_16_16_16_16:
         format = V_008F0C_GFX10_FORMAT_16_16_16_16_UINT;
         break;
      case V_008F0C_BUF_DATA_FORMAT_32:
         format = V_008F0C_GFX10_FORMAT_32_UINT;
         break;
      case V_008F0C_BUF_DATA_FORMAT_32_32:
         format = V_008F0C_GFX10_FORMAT_32_32_UINT;
         break;
      case V_008F0C_BUF_DATA_FORMAT_32_32_32:
         format = V_008F0C_GFX10_FORMAT_32_32_32_UINT;
         break;
      case V_008F0C_BUF_DATA_FORMAT_32_32_32_32:
         format = V_008F0C_GFX10_FORMAT_32_32_32_32_UINT;
         break;
      case V_008F0C_BUF_DATA_FORMAT_2_10_10_10:
         format = V_008F0C_GFX10_FORMAT_2_10_10_10_UINT;
         break;
      case V_008F0C_BUF_DATA_FORMAT_10_11_11:
         format = V_008F0C_GFX10_FORMAT_10_11_11_UINT;
         break;
      }

      // Use the regularity properties of the combined format enum.
      //
      // Note: float is incompatible with 8-bit data formats,
      //       [us]{norm,scaled} are incomparible with 32-bit data formats.
      //       [us]scaled are not writable.
      switch (nfmt) {
      case V_008F0C_BUF_NUM_FORMAT_UNORM:
         format -= 4;
         break;
      case V_008F0C_BUF_NUM_FORMAT_SNORM:
         format -= 3;
         break;
      case V_008F0C_BUF_NUM_FORMAT_USCALED:
         format -= 2;
         break;
      case V_008F0C_BUF_NUM_FORMAT_SSCALED:
         format -= 1;
         break;
      default:
         unreachable("bad nfmt");
      case V_008F0C_BUF_NUM_FORMAT_UINT:
         break;
      case V_008F0C_BUF_NUM_FORMAT_SINT:
         format += 1;
         break;
      case V_008F0C_BUF_NUM_FORMAT_FLOAT:
         format += 2;
         break;
      }

      return format;
   } else {
      return dfmt | (nfmt << 4);
   }
}

static const struct ac_data_format_info data_format_table[] = {
   [V_008F0C_BUF_DATA_FORMAT_INVALID] = {0, 4, 0, V_008F0C_BUF_DATA_FORMAT_INVALID},
   [V_008F0C_BUF_DATA_FORMAT_8] = {1, 1, 1, V_008F0C_BUF_DATA_FORMAT_8},
   [V_008F0C_BUF_DATA_FORMAT_16] = {2, 1, 2, V_008F0C_BUF_DATA_FORMAT_16},
   [V_008F0C_BUF_DATA_FORMAT_8_8] = {2, 2, 1, V_008F0C_BUF_DATA_FORMAT_8},
   [V_008F0C_BUF_DATA_FORMAT_32] = {4, 1, 4, V_008F0C_BUF_DATA_FORMAT_32},
   [V_008F0C_BUF_DATA_FORMAT_16_16] = {4, 2, 2, V_008F0C_BUF_DATA_FORMAT_16},
   [V_008F0C_BUF_DATA_FORMAT_10_11_11] = {4, 3, 0, V_008F0C_BUF_DATA_FORMAT_10_11_11},
   [V_008F0C_BUF_DATA_FORMAT_11_11_10] = {4, 3, 0, V_008F0C_BUF_DATA_FORMAT_11_11_10},
   [V_008F0C_BUF_DATA_FORMAT_10_10_10_2] = {4, 4, 0, V_008F0C_BUF_DATA_FORMAT_10_10_10_2},
   [V_008F0C_BUF_DATA_FORMAT_2_10_10_10] = {4, 4, 0, V_008F0C_BUF_DATA_FORMAT_2_10_10_10},
   [V_008F0C_BUF_DATA_FORMAT_8_8_8_8] = {4, 4, 1, V_008F0C_BUF_DATA_FORMAT_8},
   [V_008F0C_BUF_DATA_FORMAT_32_32] = {8, 2, 4, V_008F0C_BUF_DATA_FORMAT_32},
   [V_008F0C_BUF_DATA_FORMAT_16_16_16_16] = {8, 4, 2, V_008F0C_BUF_DATA_FORMAT_16},
   [V_008F0C_BUF_DATA_FORMAT_32_32_32] = {12, 3, 4, V_008F0C_BUF_DATA_FORMAT_32},
   [V_008F0C_BUF_DATA_FORMAT_32_32_32_32] = {16, 4, 4, V_008F0C_BUF_DATA_FORMAT_32},
};

const struct ac_data_format_info *ac_get_data_format_info(unsigned dfmt)
{
   assert(dfmt < ARRAY_SIZE(data_format_table));
   return &data_format_table[dfmt];
}

#define DUP2(v) v, v
#define DUP3(v) v, v, v
#define DUP4(v) v, v, v, v

#define FMT(dfmt, nfmt) 0xb, {HW_FMT(dfmt, nfmt), HW_FMT(dfmt##_##dfmt, nfmt), HW_FMT_INVALID, HW_FMT(dfmt##_##dfmt##_##dfmt##_##dfmt, nfmt)}
#define FMT_32(nfmt) 0xf, {HW_FMT(32, nfmt), HW_FMT(32_32, nfmt), HW_FMT(32_32_32, nfmt), HW_FMT(32_32_32_32, nfmt)}
#define FMT_64(nfmt) 0x3, {HW_FMT(32_32, nfmt), HW_FMT(32_32_32_32, nfmt), DUP2(HW_FMT_INVALID)}
#define FMTP(dfmt, nfmt) 0xf, {DUP4(HW_FMT(dfmt, nfmt))}

#define DST_SEL(x, y, z, w) \
   (S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_##x) | S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_##y) | \
    S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_##z) | S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_##w))

#define LIST_NFMT_8_16(nfmt) \
   [(int)PIPE_FORMAT_R8_##nfmt] = {DST_SEL(X,0,0,1), 1, 1, 1, FMT(8, nfmt)}, \
   [(int)PIPE_FORMAT_R8G8_##nfmt] = {DST_SEL(X,Y,0,1), 2, 2, 1, FMT(8, nfmt)}, \
   [(int)PIPE_FORMAT_R8G8B8_##nfmt] = {DST_SEL(X,Y,Z,1), 3, 3, 1, FMT(8, nfmt)}, \
   [(int)PIPE_FORMAT_B8G8R8_##nfmt] = {DST_SEL(Z,Y,X,1), 3, 3, 1, FMT(8, nfmt)}, \
   [(int)PIPE_FORMAT_R8G8B8A8_##nfmt] = {DST_SEL(X,Y,Z,W), 4, 4, 1, FMT(8, nfmt)}, \
   [(int)PIPE_FORMAT_B8G8R8A8_##nfmt] = {DST_SEL(Z,Y,X,W), 4, 4, 1, FMT(8, nfmt)}, \
   [(int)PIPE_FORMAT_R16_##nfmt] = {DST_SEL(X,0,0,1), 2, 1, 2, FMT(16, nfmt)}, \
   [(int)PIPE_FORMAT_R16G16_##nfmt] = {DST_SEL(X,Y,0,1), 4, 2, 2, FMT(16, nfmt)}, \
   [(int)PIPE_FORMAT_R16G16B16_##nfmt] = {DST_SEL(X,Y,Z,1), 6, 3, 2, FMT(16, nfmt)}, \
   [(int)PIPE_FORMAT_R16G16B16A16_##nfmt] = {DST_SEL(X,Y,Z,W), 8, 4, 2, FMT(16, nfmt)},

#define LIST_NFMT_32_64(nfmt) \
   [(int)PIPE_FORMAT_R32_##nfmt] = {DST_SEL(X,0,0,1), 4, 1, 4, FMT_32(nfmt)}, \
   [(int)PIPE_FORMAT_R32G32_##nfmt] = {DST_SEL(X,Y,0,1), 8, 2, 4, FMT_32(nfmt)}, \
   [(int)PIPE_FORMAT_R32G32B32_##nfmt] = {DST_SEL(X,Y,Z,1), 12, 3, 4, FMT_32(nfmt)}, \
   [(int)PIPE_FORMAT_R32G32B32A32_##nfmt] = {DST_SEL(X,Y,Z,W), 16, 4, 4, FMT_32(nfmt)}, \
   [(int)PIPE_FORMAT_R64_##nfmt] = {DST_SEL(X,Y,0,0), 8, 1, 8, FMT_64(nfmt)}, \
   [(int)PIPE_FORMAT_R64G64_##nfmt] = {DST_SEL(X,Y,Z,W), 16, 2, 8, FMT_64(nfmt)}, \
   [(int)PIPE_FORMAT_R64G64B64_##nfmt] = {DST_SEL(X,Y,Z,W), 24, 3, 8, FMT_64(nfmt)}, \
   [(int)PIPE_FORMAT_R64G64B64A64_##nfmt] = {DST_SEL(X,Y,Z,W), 32, 4, 8, FMT_64(nfmt)}, \

#define VB_FORMATS \
   [(int)PIPE_FORMAT_NONE] = {DST_SEL(0,0,0,1), 0, 4, 0, 0xf, {DUP4(HW_FMT_INVALID)}}, \
   LIST_NFMT_8_16(UNORM) \
   LIST_NFMT_8_16(SNORM) \
   LIST_NFMT_8_16(USCALED) \
   LIST_NFMT_8_16(SSCALED) \
   LIST_NFMT_8_16(UINT) \
   LIST_NFMT_8_16(SINT) \
   LIST_NFMT_32_64(UINT) \
   LIST_NFMT_32_64(SINT) \
   LIST_NFMT_32_64(FLOAT) \
   [(int)PIPE_FORMAT_R16_FLOAT] = {DST_SEL(X,0,0,1), 2, 1, 2, FMT(16, FLOAT)}, \
   [(int)PIPE_FORMAT_R16G16_FLOAT] = {DST_SEL(X,Y,0,1), 4, 2, 2, FMT(16, FLOAT)}, \
   [(int)PIPE_FORMAT_R16G16B16_FLOAT] = {DST_SEL(X,Y,Z,1), 6, 3, 2, FMT(16, FLOAT)}, \
   [(int)PIPE_FORMAT_R16G16B16A16_FLOAT] = {DST_SEL(X,Y,Z,W), 8, 4, 2, FMT(16, FLOAT)}, \
   [(int)PIPE_FORMAT_B10G10R10A2_UNORM] = {DST_SEL(Z,Y,X,W), 4, 4, 0, FMTP(2_10_10_10, UNORM)}, \
   [(int)PIPE_FORMAT_B10G10R10A2_SNORM] = {DST_SEL(Z,Y,X,W), 4, 4, 0, FMTP(2_10_10_10, SNORM), \
                                           AA(AC_ALPHA_ADJUST_SNORM)}, \
   [(int)PIPE_FORMAT_B10G10R10A2_USCALED] = {DST_SEL(Z,Y,X,W), 4, 4, 0, FMTP(2_10_10_10, USCALED)}, \
   [(int)PIPE_FORMAT_B10G10R10A2_SSCALED] = {DST_SEL(Z,Y,X,W), 4, 4, 0, FMTP(2_10_10_10, SSCALED), \
                                             AA(AC_ALPHA_ADJUST_SSCALED)}, \
   [(int)PIPE_FORMAT_B10G10R10A2_UINT] = {DST_SEL(Z,Y,X,W), 4, 4, 0, FMTP(2_10_10_10, UINT)}, \
   [(int)PIPE_FORMAT_B10G10R10A2_SINT] = {DST_SEL(Z,Y,X,W), 4, 4, 0, FMTP(2_10_10_10, SINT), \
                                          AA(AC_ALPHA_ADJUST_SINT)}, \
   [(int)PIPE_FORMAT_R10G10B10A2_UNORM] = {DST_SEL(X,Y,Z,W), 4, 4, 0, FMTP(2_10_10_10, UNORM)}, \
   [(int)PIPE_FORMAT_R10G10B10A2_SNORM] = {DST_SEL(X,Y,Z,W), 4, 4, 0, FMTP(2_10_10_10, SNORM), \
                                           AA(AC_ALPHA_ADJUST_SNORM)}, \
   [(int)PIPE_FORMAT_R10G10B10A2_USCALED] = {DST_SEL(X,Y,Z,W), 4, 4, 0, FMTP(2_10_10_10, USCALED)}, \
   [(int)PIPE_FORMAT_R10G10B10A2_SSCALED] = {DST_SEL(X,Y,Z,W), 4, 4, 0, FMTP(2_10_10_10, SSCALED), \
                                             AA(AC_ALPHA_ADJUST_SSCALED)}, \
   [(int)PIPE_FORMAT_R10G10B10A2_UINT] = {DST_SEL(X,Y,Z,W), 4, 4, 0, FMTP(2_10_10_10, UINT)}, \
   [(int)PIPE_FORMAT_R10G10B10A2_SINT] = {DST_SEL(X,Y,Z,W), 4, 4, 0, FMTP(2_10_10_10, SINT), \
                                          AA(AC_ALPHA_ADJUST_SINT)}, \
   [(int)PIPE_FORMAT_R11G11B10_FLOAT] = {DST_SEL(X,Y,Z,W), 4, 3, 0, FMTP(10_11_11, FLOAT)}, \

#define HW_FMT(dfmt, nfmt) (V_008F0C_BUF_DATA_FORMAT_##dfmt | (V_008F0C_BUF_NUM_FORMAT_##nfmt << 4))
#define HW_FMT_INVALID (V_008F0C_BUF_DATA_FORMAT_INVALID | (V_008F0C_BUF_NUM_FORMAT_UNORM << 4))
#define AA(v) v
static const struct ac_vtx_format_info vb_formats_gfx6_alpha_adjust[] = {VB_FORMATS};
#undef AA

#define AA(v) AC_ALPHA_ADJUST_NONE
static const struct ac_vtx_format_info vb_formats_gfx6[] = {VB_FORMATS};
#undef HW_FMT_INVALID
#undef HW_FMT

#define HW_FMT(dfmt, nfmt) V_008F0C_GFX10_FORMAT_##dfmt##_##nfmt
#define HW_FMT_INVALID V_008F0C_GFX10_FORMAT_INVALID
static const struct ac_vtx_format_info vb_formats_gfx10[] = {VB_FORMATS};
#undef HW_FMT_INVALID
#undef HW_FMT

#define HW_FMT(dfmt, nfmt) V_008F0C_GFX11_FORMAT_##dfmt##_##nfmt
#define HW_FMT_INVALID V_008F0C_GFX11_FORMAT_INVALID
static const struct ac_vtx_format_info vb_formats_gfx11[] = {VB_FORMATS};

const struct ac_vtx_format_info *
ac_get_vtx_format_info_table(enum amd_gfx_level level, enum radeon_family family)
{
   if (level >= GFX11)
      return vb_formats_gfx11;
   else if (level >= GFX10)
      return vb_formats_gfx10;
   bool alpha_adjust = level <= GFX8 && family != CHIP_STONEY;
   return alpha_adjust ? vb_formats_gfx6_alpha_adjust : vb_formats_gfx6;
}

const struct ac_vtx_format_info *
ac_get_vtx_format_info(enum amd_gfx_level level, enum radeon_family family, enum pipe_format fmt)
{
   return &ac_get_vtx_format_info_table(level, family)[fmt];
}

enum ac_image_dim ac_get_sampler_dim(enum amd_gfx_level gfx_level, enum glsl_sampler_dim dim,
                                     bool is_array)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D:
      if (gfx_level == GFX9)
         return is_array ? ac_image_2darray : ac_image_2d;
      return is_array ? ac_image_1darray : ac_image_1d;
   case GLSL_SAMPLER_DIM_2D:
   case GLSL_SAMPLER_DIM_RECT:
   case GLSL_SAMPLER_DIM_EXTERNAL:
      return is_array ? ac_image_2darray : ac_image_2d;
   case GLSL_SAMPLER_DIM_3D:
      return ac_image_3d;
   case GLSL_SAMPLER_DIM_CUBE:
      return ac_image_cube;
   case GLSL_SAMPLER_DIM_MS:
      return is_array ? ac_image_2darraymsaa : ac_image_2dmsaa;
   case GLSL_SAMPLER_DIM_SUBPASS:
      return ac_image_2darray;
   case GLSL_SAMPLER_DIM_SUBPASS_MS:
      return ac_image_2darraymsaa;
   default:
      unreachable("bad sampler dim");
   }
}

enum ac_image_dim ac_get_image_dim(enum amd_gfx_level gfx_level, enum glsl_sampler_dim sdim,
                                   bool is_array)
{
   enum ac_image_dim dim = ac_get_sampler_dim(gfx_level, sdim, is_array);

   /* Match the resource type set in the descriptor. */
   if (dim == ac_image_cube || (gfx_level <= GFX8 && dim == ac_image_3d))
      dim = ac_image_2darray;
   else if (sdim == GLSL_SAMPLER_DIM_2D && !is_array && gfx_level == GFX9) {
      /* When a single layer of a 3D texture is bound, the shader
       * will refer to a 2D target, but the descriptor has a 3D type.
       * Since the HW ignores BASE_ARRAY in this case, we need to
       * send 3 coordinates. This doesn't hurt when the underlying
       * texture is non-3D.
       */
      dim = ac_image_3d;
   }

   return dim;
}

unsigned ac_get_fs_input_vgpr_cnt(const struct ac_shader_config *config,
                                  signed char *face_vgpr_index_ptr,
                                  signed char *ancillary_vgpr_index_ptr,
                                  signed char *sample_coverage_vgpr_index_ptr)
{
   unsigned num_input_vgprs = 0;
   signed char face_vgpr_index = -1;
   signed char ancillary_vgpr_index = -1;
   signed char sample_coverage_vgpr_index = -1;

   if (G_0286CC_PERSP_SAMPLE_ENA(config->spi_ps_input_addr))
      num_input_vgprs += 2;
   if (G_0286CC_PERSP_CENTER_ENA(config->spi_ps_input_addr))
      num_input_vgprs += 2;
   if (G_0286CC_PERSP_CENTROID_ENA(config->spi_ps_input_addr))
      num_input_vgprs += 2;
   if (G_0286CC_PERSP_PULL_MODEL_ENA(config->spi_ps_input_addr))
      num_input_vgprs += 3;
   if (G_0286CC_LINEAR_SAMPLE_ENA(config->spi_ps_input_addr))
      num_input_vgprs += 2;
   if (G_0286CC_LINEAR_CENTER_ENA(config->spi_ps_input_addr))
      num_input_vgprs += 2;
   if (G_0286CC_LINEAR_CENTROID_ENA(config->spi_ps_input_addr))
      num_input_vgprs += 2;
   if (G_0286CC_LINE_STIPPLE_TEX_ENA(config->spi_ps_input_addr))
      num_input_vgprs += 1;
   if (G_0286CC_POS_X_FLOAT_ENA(config->spi_ps_input_addr))
      num_input_vgprs += 1;
   if (G_0286CC_POS_Y_FLOAT_ENA(config->spi_ps_input_addr))
      num_input_vgprs += 1;
   if (G_0286CC_POS_Z_FLOAT_ENA(config->spi_ps_input_addr))
      num_input_vgprs += 1;
   if (G_0286CC_POS_W_FLOAT_ENA(config->spi_ps_input_addr))
      num_input_vgprs += 1;
   if (G_0286CC_FRONT_FACE_ENA(config->spi_ps_input_addr)) {
      face_vgpr_index = num_input_vgprs;
      num_input_vgprs += 1;
   }
   if (G_0286CC_ANCILLARY_ENA(config->spi_ps_input_addr)) {
      ancillary_vgpr_index = num_input_vgprs;
      num_input_vgprs += 1;
   }
   if (G_0286CC_SAMPLE_COVERAGE_ENA(config->spi_ps_input_addr)) {
      sample_coverage_vgpr_index = num_input_vgprs;
      num_input_vgprs += 1;
   }
   if (G_0286CC_POS_FIXED_PT_ENA(config->spi_ps_input_addr))
      num_input_vgprs += 1;

   if (face_vgpr_index_ptr)
      *face_vgpr_index_ptr = face_vgpr_index;
   if (ancillary_vgpr_index_ptr)
      *ancillary_vgpr_index_ptr = ancillary_vgpr_index;
   if (sample_coverage_vgpr_index_ptr)
      *sample_coverage_vgpr_index_ptr = sample_coverage_vgpr_index;

   return num_input_vgprs;
}

void ac_choose_spi_color_formats(unsigned format, unsigned swap, unsigned ntype,
                                 bool is_depth, bool use_rbplus,
                                 struct ac_spi_color_formats *formats)
{
   /* Alpha is needed for alpha-to-coverage.
    * Blending may be with or without alpha.
    */
   unsigned normal = 0;      /* most optimal, may not support blending or export alpha */
   unsigned alpha = 0;       /* exports alpha, but may not support blending */
   unsigned blend = 0;       /* supports blending, but may not export alpha */
   unsigned blend_alpha = 0; /* least optimal, supports blending and exports alpha */

   /* Choose the SPI color formats. These are required values for RB+.
    * Other chips have multiple choices, though they are not necessarily better.
    */
   switch (format) {
   case V_028C70_COLOR_5_6_5:
   case V_028C70_COLOR_1_5_5_5:
   case V_028C70_COLOR_5_5_5_1:
   case V_028C70_COLOR_4_4_4_4:
   case V_028C70_COLOR_10_11_11:
   case V_028C70_COLOR_11_11_10:
   case V_028C70_COLOR_5_9_9_9:
   case V_028C70_COLOR_8:
   case V_028C70_COLOR_8_8:
   case V_028C70_COLOR_8_8_8_8:
   case V_028C70_COLOR_10_10_10_2:
   case V_028C70_COLOR_2_10_10_10:
      if (ntype == V_028C70_NUMBER_UINT)
         alpha = blend = blend_alpha = normal = V_028714_SPI_SHADER_UINT16_ABGR;
      else if (ntype == V_028C70_NUMBER_SINT)
         alpha = blend = blend_alpha = normal = V_028714_SPI_SHADER_SINT16_ABGR;
      else
         alpha = blend = blend_alpha = normal = V_028714_SPI_SHADER_FP16_ABGR;

      if (!use_rbplus && format == V_028C70_COLOR_8 &&
          ntype != V_028C70_NUMBER_SRGB && swap == V_028C70_SWAP_STD) /* R */ {
         /* When RB+ is enabled, R8_UNORM should use FP16_ABGR for 2x
          * exporting performance. Otherwise, use 32_R to remove useless
          * instructions needed for 16-bit compressed exports.
          */
         blend = normal = V_028714_SPI_SHADER_32_R;
      }
      break;

   case V_028C70_COLOR_16:
   case V_028C70_COLOR_16_16:
   case V_028C70_COLOR_16_16_16_16:
      if (ntype == V_028C70_NUMBER_UNORM || ntype == V_028C70_NUMBER_SNORM) {
         /* UNORM16 and SNORM16 don't support blending */
         if (ntype == V_028C70_NUMBER_UNORM)
            normal = alpha = V_028714_SPI_SHADER_UNORM16_ABGR;
         else
            normal = alpha = V_028714_SPI_SHADER_SNORM16_ABGR;

         /* Use 32 bits per channel for blending. */
         if (format == V_028C70_COLOR_16) {
            if (swap == V_028C70_SWAP_STD) { /* R */
               blend = V_028714_SPI_SHADER_32_R;
               blend_alpha = V_028714_SPI_SHADER_32_AR;
            } else if (swap == V_028C70_SWAP_ALT_REV) /* A */
               blend = blend_alpha = V_028714_SPI_SHADER_32_AR;
            else
               assert(0);
         } else if (format == V_028C70_COLOR_16_16) {
            if (swap == V_028C70_SWAP_STD || swap == V_028C70_SWAP_STD_REV) { /* RG or GR */
               blend = V_028714_SPI_SHADER_32_GR;
               blend_alpha = V_028714_SPI_SHADER_32_ABGR;
            } else if (swap == V_028C70_SWAP_ALT) /* RA */
               blend = blend_alpha = V_028714_SPI_SHADER_32_AR;
            else
               assert(0);
         } else /* 16_16_16_16 */
            blend = blend_alpha = V_028714_SPI_SHADER_32_ABGR;
      } else if (ntype == V_028C70_NUMBER_UINT)
         alpha = blend = blend_alpha = normal = V_028714_SPI_SHADER_UINT16_ABGR;
      else if (ntype == V_028C70_NUMBER_SINT)
         alpha = blend = blend_alpha = normal = V_028714_SPI_SHADER_SINT16_ABGR;
      else if (ntype == V_028C70_NUMBER_FLOAT)
         alpha = blend = blend_alpha = normal = V_028714_SPI_SHADER_FP16_ABGR;
      else
         assert(0);
      break;

   case V_028C70_COLOR_32:
      if (swap == V_028C70_SWAP_STD) { /* R */
         blend = normal = V_028714_SPI_SHADER_32_R;
         alpha = blend_alpha = V_028714_SPI_SHADER_32_AR;
      } else if (swap == V_028C70_SWAP_ALT_REV) /* A */
         alpha = blend = blend_alpha = normal = V_028714_SPI_SHADER_32_AR;
      else
         assert(0);
      break;

   case V_028C70_COLOR_32_32:
      if (swap == V_028C70_SWAP_STD || swap == V_028C70_SWAP_STD_REV) { /* RG or GR */
         blend = normal = V_028714_SPI_SHADER_32_GR;
         alpha = blend_alpha = V_028714_SPI_SHADER_32_ABGR;
      } else if (swap == V_028C70_SWAP_ALT) /* RA */
         alpha = blend = blend_alpha = normal = V_028714_SPI_SHADER_32_AR;
      else
         assert(0);
      break;

   case V_028C70_COLOR_32_32_32_32:
   case V_028C70_COLOR_8_24:
   case V_028C70_COLOR_24_8:
   case V_028C70_COLOR_X24_8_32_FLOAT:
      alpha = blend = blend_alpha = normal = V_028714_SPI_SHADER_32_ABGR;
      break;

   default:
      assert(0);
      return;
   }

   /* The DB->CB copy needs 32_ABGR. */
   if (is_depth)
      alpha = blend = blend_alpha = normal = V_028714_SPI_SHADER_32_ABGR;

   formats->normal = normal;
   formats->alpha = alpha;
   formats->blend = blend;
   formats->blend_alpha = blend_alpha;
}

void ac_compute_late_alloc(const struct radeon_info *info, bool ngg, bool ngg_culling,
                           bool uses_scratch, unsigned *late_alloc_wave64, unsigned *cu_mask)
{
   *late_alloc_wave64 = 0; /* The limit is per SA. */
   *cu_mask = 0xffff;

   /* CU masking can decrease performance and cause a hang with <= 2 CUs per SA. */
   if (info->min_good_cu_per_sa <= 2)
      return;

   /* If scratch is used with late alloc, the GPU could deadlock if PS uses scratch too. A more
    * complicated computation is needed to enable late alloc with scratch (see PAL).
    */
   if (uses_scratch)
      return;

   /* Late alloc is not used for NGG on Navi14 due to a hw bug. */
   if (ngg && info->family == CHIP_NAVI14)
      return;

   if (info->gfx_level >= GFX10) {
      /* For Wave32, the hw will launch twice the number of late alloc waves, so 1 == 2x wave32.
       * These limits are estimated because they are all safe but they vary in performance.
       */
      if (ngg_culling)
         *late_alloc_wave64 = info->min_good_cu_per_sa * 10;
      else if (info->gfx_level >= GFX11)
         *late_alloc_wave64 = 63;
      else
         *late_alloc_wave64 = info->min_good_cu_per_sa * 4;

      /* Limit LATE_ALLOC_GS to prevent a hang (hw bug) on gfx10. */
      if (info->gfx_level == GFX10 && ngg)
         *late_alloc_wave64 = MIN2(*late_alloc_wave64, 64);

      /* Gfx10: CU2 & CU3 must be disabled to prevent a hw deadlock.
       * Others: CU1 must be disabled to prevent a hw deadlock.
       *
       * The deadlock is caused by late alloc, which usually increases performance.
       */
      *cu_mask &= info->gfx_level == GFX10 ? ~BITFIELD_RANGE(2, 2) :
                                              ~BITFIELD_RANGE(1, 1);
   } else {
      if (info->min_good_cu_per_sa <= 4) {
         /* Too few available compute units per SA. Disallowing VS to run on one CU could hurt us
          * more than late VS allocation would help.
          *
          * 2 is the highest safe number that allows us to keep all CUs enabled.
          */
         *late_alloc_wave64 = 2;
      } else {
         /* This is a good initial value, allowing 1 late_alloc wave per SIMD on num_cu - 2.
          */
         *late_alloc_wave64 = (info->min_good_cu_per_sa - 2) * 4;
      }

      /* VS can't execute on one CU if the limit is > 2. */
      if (*late_alloc_wave64 > 2)
         *cu_mask = 0xfffe; /* 1 CU disabled */
   }

   /* Max number that fits into the register field. */
   if (ngg) /* GS */
      *late_alloc_wave64 = MIN2(*late_alloc_wave64, G_00B204_SPI_SHADER_LATE_ALLOC_GS_GFX10(~0u));
   else /* VS */
      *late_alloc_wave64 = MIN2(*late_alloc_wave64, G_00B11C_LIMIT(~0u));
}

unsigned ac_compute_cs_workgroup_size(const uint16_t sizes[3], bool variable, unsigned max)
{
   if (variable)
      return max;

   return sizes[0] * sizes[1] * sizes[2];
}

unsigned ac_compute_lshs_workgroup_size(enum amd_gfx_level gfx_level, gl_shader_stage stage,
                                        unsigned tess_num_patches,
                                        unsigned tess_patch_in_vtx,
                                        unsigned tess_patch_out_vtx)
{
   /* When tessellation is used, API VS runs on HW LS, API TCS runs on HW HS.
    * These two HW stages are merged on GFX9+.
    */

   bool merged_shaders = gfx_level >= GFX9;
   unsigned ls_workgroup_size = tess_num_patches * tess_patch_in_vtx;
   unsigned hs_workgroup_size = tess_num_patches * tess_patch_out_vtx;

   if (merged_shaders)
      return MAX2(ls_workgroup_size, hs_workgroup_size);
   else if (stage == MESA_SHADER_VERTEX)
      return ls_workgroup_size;
   else if (stage == MESA_SHADER_TESS_CTRL)
      return hs_workgroup_size;
   else
      unreachable("invalid LSHS shader stage");
}

unsigned ac_compute_esgs_workgroup_size(enum amd_gfx_level gfx_level, unsigned wave_size,
                                        unsigned es_verts, unsigned gs_inst_prims)
{
   /* ESGS may operate in workgroups if on-chip GS (LDS rings) are enabled.
    *
    * GFX6: Not possible in the HW.
    * GFX7-8 (unmerged): possible in the HW, but not implemented in Mesa.
    * GFX9+ (merged): implemented in Mesa.
    */

   if (gfx_level <= GFX8)
      return wave_size;

   unsigned workgroup_size = MAX2(es_verts, gs_inst_prims);
   return CLAMP(workgroup_size, 1, 256);
}

unsigned ac_compute_ngg_workgroup_size(unsigned es_verts, unsigned gs_inst_prims,
                                       unsigned max_vtx_out, unsigned prim_amp_factor)
{
   /* NGG always operates in workgroups.
    *
    * For API VS/TES/GS:
    * - 1 invocation per input vertex
    * - 1 invocation per input primitive
    *
    * The same invocation can process both an input vertex and primitive,
    * however 1 invocation can only output up to 1 vertex and 1 primitive.
    */

   unsigned max_vtx_in = es_verts < 256 ? es_verts : 3 * gs_inst_prims;
   unsigned max_prim_in = gs_inst_prims;
   unsigned max_prim_out = gs_inst_prims * prim_amp_factor;
   unsigned workgroup_size = MAX4(max_vtx_in, max_vtx_out, max_prim_in, max_prim_out);

   return CLAMP(workgroup_size, 1, 256);
}

void ac_set_reg_cu_en(void *cs, unsigned reg_offset, uint32_t value, uint32_t clear_mask,
                      unsigned value_shift, const struct radeon_info *info,
                      void set_sh_reg(void*, unsigned, uint32_t))
{
   /* Register field position and mask. */
   uint32_t cu_en_mask = ~clear_mask;
   unsigned cu_en_shift = ffs(cu_en_mask) - 1;
   /* The value being set. */
   uint32_t cu_en = (value & cu_en_mask) >> cu_en_shift;

   /* AND the field by spi_cu_en. */
   uint32_t spi_cu_en = info->spi_cu_en >> value_shift;
   uint32_t new_value = (value & ~cu_en_mask) |
                        (((cu_en & spi_cu_en) << cu_en_shift) & cu_en_mask);

   set_sh_reg(cs, reg_offset, new_value);
}

/* Return the register value and tune bytes_per_wave to increase scratch performance. */
void ac_get_scratch_tmpring_size(const struct radeon_info *info,
                                 unsigned bytes_per_wave, unsigned *max_seen_bytes_per_wave,
                                 uint32_t *tmpring_size)
{
   /* SPI_TMPRING_SIZE and COMPUTE_TMPRING_SIZE are essentially scratch buffer descriptors.
    * WAVES means NUM_RECORDS. WAVESIZE is the size of each element, meaning STRIDE.
    * Thus, WAVESIZE must be constant while the scratch buffer is being used by the GPU.
    *
    * If you want to increase WAVESIZE without waiting for idle, you need to allocate a new
    * scratch buffer and use it instead. This will result in multiple scratch buffers being
    * used at the same time, each with a different WAVESIZE.
    *
    * If you want to decrease WAVESIZE, you don't have to. There is no advantage in decreasing
    * WAVESIZE after it's been increased.
    *
    * Shaders with SCRATCH_EN=0 don't allocate scratch space.
    */
   const unsigned size_shift = info->gfx_level >= GFX11 ? 8 : 10;
   const unsigned min_size_per_wave = BITFIELD_BIT(size_shift);

   /* The LLVM shader backend should be reporting aligned scratch_sizes. */
   assert((bytes_per_wave & BITFIELD_MASK(size_shift)) == 0 &&
          "scratch size per wave should be aligned");

   /* Add 1 scratch item to make the number of items odd. This should improve scratch
    * performance by more randomly distributing scratch waves among memory channels.
    */
   if (bytes_per_wave)
      bytes_per_wave |= min_size_per_wave;

   *max_seen_bytes_per_wave = MAX2(*max_seen_bytes_per_wave, bytes_per_wave);

   unsigned max_scratch_waves = info->max_scratch_waves;
   if (info->gfx_level >= GFX11)
      max_scratch_waves /= info->num_se; /* WAVES is per SE */

   /* TODO: We could decrease WAVES to make the whole buffer fit into the infinity cache. */
   *tmpring_size = S_0286E8_WAVES(max_scratch_waves) |
                   S_0286E8_WAVESIZE(*max_seen_bytes_per_wave >> size_shift);
}
