/*
 * Copyright (c) 2013 Seppo Tomperi <seppo.tomperi@vtt.fi>
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/cpu.h"
#include "libavutil/x86/asm.h"
#include "libavutil/x86/cpu.h"
#include "libavcodec/hevcdsp.h"

#if HAVE_YASM

#define LFC_FUNC(DIR, DEPTH, OPT)                                                \
void ff_hevc_ ## DIR ## _loop_filter_chroma_ ## DEPTH ## _ ## OPT(uint8_t *_pix, \
    ptrdiff_t _stride, int *_tc, uint8_t *_no_p, uint8_t *_no_q);

#define LFL_FUNC(DIR, DEPTH, OPT)                                              \
void ff_hevc_ ## DIR ## _loop_filter_luma_ ## DEPTH ## _ ## OPT(uint8_t *_pix, \
    ptrdiff_t stride, int *_beta, int *_tc, uint8_t *_no_p, uint8_t *_no_q);

#define LFC_FUNCS(type, depth) \
    LFC_FUNC(h, depth, sse2)   \
    LFC_FUNC(v, depth, sse2)

#define LFL_FUNCS(type, depth) \
    LFL_FUNC(h, depth, ssse3)  \
    LFL_FUNC(v, depth, ssse3)

LFC_FUNCS(uint8_t, 8)
LFL_FUNCS(uint8_t, 8)

#endif /* HAVE_YASM */

av_cold void ff_hevcdsp_init_x86(HEVCDSPContext *c, int bit_depth)
{
#if HAVE_YASM
    int cpu_flags = av_get_cpu_flags();

    if (bit_depth == 8) {
      if (EXTERNAL_SSE2(cpu_flags)) {
        c->hevc_v_loop_filter_chroma = ff_hevc_v_loop_filter_chroma_8_sse2;
        c->hevc_h_loop_filter_chroma = ff_hevc_h_loop_filter_chroma_8_sse2;
      }
      if (EXTERNAL_SSSE3(cpu_flags)) {
#if ARCH_X86_64
        c->hevc_v_loop_filter_luma = ff_hevc_v_loop_filter_luma_8_ssse3;
        c->hevc_h_loop_filter_luma = ff_hevc_h_loop_filter_luma_8_ssse3;
#endif
      }
    }
#endif /* HAVE_YASM */
}
