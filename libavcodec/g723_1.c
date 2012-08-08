/*
 * G.723.1 compatible decoder
 * Copyright (c) 2006 Benjamin Larsson
 * Copyright (c) 2010 Mohamed Naufal Basheer
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

/**
 * @file
 * G.723.1 compatible decoder
 */

#define BITSTREAM_READER_LE
#include "libavutil/audioconvert.h"
#include "libavutil/lzo.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "get_bits.h"
#include "acelp_vectors.h"
#include "celp_filters.h"
#include "celp_math.h"
#include "lsp.h"
#include "g723_1_data.h"

/**
 * G723.1 frame types
 */
enum FrameType {
    ACTIVE_FRAME,        ///< Active speech
    SID_FRAME,           ///< Silence Insertion Descriptor frame
    UNTRANSMITTED_FRAME
};

enum Rate {
    RATE_6300,
    RATE_5300
};

/**
 * G723.1 unpacked data subframe
 */
typedef struct {
    int ad_cb_lag;     ///< adaptive codebook lag
    int ad_cb_gain;
    int dirac_train;
    int pulse_sign;
    int grid_index;
    int amp_index;
    int pulse_pos;
} G723_1_Subframe;

/**
 * Pitch postfilter parameters
 */
typedef struct {
    int     index;    ///< postfilter backward/forward lag
    int16_t opt_gain; ///< optimal gain
    int16_t sc_gain;  ///< scaling gain
} PPFParam;

typedef struct g723_1_context {
    AVClass *class;
    AVFrame frame;

    G723_1_Subframe subframe[4];
    enum FrameType cur_frame_type;
    enum FrameType past_frame_type;
    enum Rate cur_rate;
    uint8_t lsp_index[LSP_BANDS];
    int pitch_lag[2];
    int erased_frames;

    int16_t prev_lsp[LPC_ORDER];
    int16_t prev_excitation[PITCH_MAX];
    int16_t excitation[PITCH_MAX + FRAME_LEN + 4];
    int16_t synth_mem[LPC_ORDER];
    int16_t fir_mem[LPC_ORDER];
    int     iir_mem[LPC_ORDER];

    int random_seed;
    int interp_index;
    int interp_gain;
    int sid_gain;
    int cur_gain;
    int reflection_coef;
    int pf_gain;
    int postfilter;

    int16_t audio[FRAME_LEN + LPC_ORDER];
} G723_1_Context;

static av_cold int g723_1_decode_init(AVCodecContext *avctx)
{
    G723_1_Context *p = avctx->priv_data;

    avctx->channel_layout = AV_CH_LAYOUT_MONO;
    avctx->sample_fmt     = AV_SAMPLE_FMT_S16;
    avctx->channels       = 1;
    avctx->sample_rate    = 8000;
    p->pf_gain            = 1 << 12;

    avcodec_get_frame_defaults(&p->frame);
    avctx->coded_frame    = &p->frame;

    memcpy(p->prev_lsp, dc_lsp, LPC_ORDER * sizeof(*p->prev_lsp));

    return 0;
}

/**
 * Unpack the frame into parameters.
 *
 * @param p           the context
 * @param buf         pointer to the input buffer
 * @param buf_size    size of the input buffer
 */
static int unpack_bitstream(G723_1_Context *p, const uint8_t *buf,
                            int buf_size)
{
    GetBitContext gb;
    int ad_cb_len;
    int temp, info_bits, i;

    init_get_bits(&gb, buf, buf_size * 8);

    /* Extract frame type and rate info */
    info_bits = get_bits(&gb, 2);

    if (info_bits == 3) {
        p->cur_frame_type = UNTRANSMITTED_FRAME;
        return 0;
    }

    /* Extract 24 bit lsp indices, 8 bit for each band */
    p->lsp_index[2] = get_bits(&gb, 8);
    p->lsp_index[1] = get_bits(&gb, 8);
    p->lsp_index[0] = get_bits(&gb, 8);

    if (info_bits == 2) {
        p->cur_frame_type = SID_FRAME;
        p->subframe[0].amp_index = get_bits(&gb, 6);
        return 0;
    }

    /* Extract the info common to both rates */
    p->cur_rate       = info_bits ? RATE_5300 : RATE_6300;
    p->cur_frame_type = ACTIVE_FRAME;

    p->pitch_lag[0] = get_bits(&gb, 7);
    if (p->pitch_lag[0] > 123)       /* test if forbidden code */
        return -1;
    p->pitch_lag[0] += PITCH_MIN;
    p->subframe[1].ad_cb_lag = get_bits(&gb, 2);

    p->pitch_lag[1] = get_bits(&gb, 7);
    if (p->pitch_lag[1] > 123)
        return -1;
    p->pitch_lag[1] += PITCH_MIN;
    p->subframe[3].ad_cb_lag = get_bits(&gb, 2);
    p->subframe[0].ad_cb_lag = 1;
    p->subframe[2].ad_cb_lag = 1;

    for (i = 0; i < SUBFRAMES; i++) {
        /* Extract combined gain */
        temp = get_bits(&gb, 12);
        ad_cb_len = 170;
        p->subframe[i].dirac_train = 0;
        if (p->cur_rate == RATE_6300 && p->pitch_lag[i >> 1] < SUBFRAME_LEN - 2) {
            p->subframe[i].dirac_train = temp >> 11;
            temp &= 0x7FF;
            ad_cb_len = 85;
        }
        p->subframe[i].ad_cb_gain = FASTDIV(temp, GAIN_LEVELS);
        if (p->subframe[i].ad_cb_gain < ad_cb_len) {
            p->subframe[i].amp_index = temp - p->subframe[i].ad_cb_gain *
                                       GAIN_LEVELS;
        } else {
            return -1;
        }
    }

    p->subframe[0].grid_index = get_bits(&gb, 1);
    p->subframe[1].grid_index = get_bits(&gb, 1);
    p->subframe[2].grid_index = get_bits(&gb, 1);
    p->subframe[3].grid_index = get_bits(&gb, 1);

    if (p->cur_rate == RATE_6300) {
        skip_bits(&gb, 1);  /* skip reserved bit */

        /* Compute pulse_pos index using the 13-bit combined position index */
        temp = get_bits(&gb, 13);
        p->subframe[0].pulse_pos = temp / 810;

        temp -= p->subframe[0].pulse_pos * 810;
        p->subframe[1].pulse_pos = FASTDIV(temp, 90);

        temp -= p->subframe[1].pulse_pos * 90;
        p->subframe[2].pulse_pos = FASTDIV(temp, 9);
        p->subframe[3].pulse_pos = temp - p->subframe[2].pulse_pos * 9;

        p->subframe[0].pulse_pos = (p->subframe[0].pulse_pos << 16) +
                                   get_bits(&gb, 16);
        p->subframe[1].pulse_pos = (p->subframe[1].pulse_pos << 14) +
                                   get_bits(&gb, 14);
        p->subframe[2].pulse_pos = (p->subframe[2].pulse_pos << 16) +
                                   get_bits(&gb, 16);
        p->subframe[3].pulse_pos = (p->subframe[3].pulse_pos << 14) +
                                   get_bits(&gb, 14);

        p->subframe[0].pulse_sign = get_bits(&gb, 6);
        p->subframe[1].pulse_sign = get_bits(&gb, 5);
        p->subframe[2].pulse_sign = get_bits(&gb, 6);
        p->subframe[3].pulse_sign = get_bits(&gb, 5);
    } else { /* 5300 bps */
        p->subframe[0].pulse_pos  = get_bits(&gb, 12);
        p->subframe[1].pulse_pos  = get_bits(&gb, 12);
        p->subframe[2].pulse_pos  = get_bits(&gb, 12);
        p->subframe[3].pulse_pos  = get_bits(&gb, 12);

        p->subframe[0].pulse_sign = get_bits(&gb, 4);
        p->subframe[1].pulse_sign = get_bits(&gb, 4);
        p->subframe[2].pulse_sign = get_bits(&gb, 4);
        p->subframe[3].pulse_sign = get_bits(&gb, 4);
    }

    return 0;
}

/**
 * Bitexact implementation of sqrt(val/2).
 */
static int16_t square_root(int val)
{
    int16_t res = 0;
    int16_t exp = 0x4000;
    int i;

    for (i = 0; i < 14; i ++) {
        int res_exp = res + exp;
        if (val >= res_exp * res_exp << 1)
            res += exp;
        exp >>= 1;
    }
    return res;
}

/**
 * Calculate the number of left-shifts required for normalizing the input.
 *
 * @param num   input number
 * @param width width of the input, 16 bits(0) / 32 bits(1)
 */
static int normalize_bits(int num, int width)
{
    if (!num)
        return 0;
    if (num == -1)
        return width;
    if (num < 0)
        num = ~num;

    return width - av_log2(num) - 1;
}

/**
 * Scale vector contents based on the largest of their absolutes.
 */
static int scale_vector(int16_t *vector, int length)
{
    int bits, max = 0;
    int64_t scale;
    int i;


    for (i = 0; i < length; i++)
        max = FFMAX(max, FFABS(vector[i]));

    max   = FFMIN(max, 0x7FFF);
    bits  = normalize_bits(max, 15);
    scale = (bits == 15) ? 0x7FFF : (1 << bits);

    for (i = 0; i < length; i++)
        vector[i] = av_clipl_int32(vector[i] * scale << 1) >> 4;

    return bits - 3;
}

/**
 * Perform inverse quantization of LSP frequencies.
 *
 * @param cur_lsp    the current LSP vector
 * @param prev_lsp   the previous LSP vector
 * @param lsp_index  VQ indices
 * @param bad_frame  bad frame flag
 */
static void inverse_quant(int16_t *cur_lsp, int16_t *prev_lsp,
                          uint8_t *lsp_index, int bad_frame)
{
    int min_dist, pred;
    int i, j, temp, stable;

    /* Check for frame erasure */
    if (!bad_frame) {
        min_dist     = 0x100;
        pred         = 12288;
    } else {
        min_dist     = 0x200;
        pred         = 23552;
        lsp_index[0] = lsp_index[1] = lsp_index[2] = 0;
    }

    /* Get the VQ table entry corresponding to the transmitted index */
    cur_lsp[0] = lsp_band0[lsp_index[0]][0];
    cur_lsp[1] = lsp_band0[lsp_index[0]][1];
    cur_lsp[2] = lsp_band0[lsp_index[0]][2];
    cur_lsp[3] = lsp_band1[lsp_index[1]][0];
    cur_lsp[4] = lsp_band1[lsp_index[1]][1];
    cur_lsp[5] = lsp_band1[lsp_index[1]][2];
    cur_lsp[6] = lsp_band2[lsp_index[2]][0];
    cur_lsp[7] = lsp_band2[lsp_index[2]][1];
    cur_lsp[8] = lsp_band2[lsp_index[2]][2];
    cur_lsp[9] = lsp_band2[lsp_index[2]][3];

    /* Add predicted vector & DC component to the previously quantized vector */
    for (i = 0; i < LPC_ORDER; i++) {
        temp        = ((prev_lsp[i] - dc_lsp[i]) * pred + (1 << 14)) >> 15;
        cur_lsp[i] += dc_lsp[i] + temp;
    }

    for (i = 0; i < LPC_ORDER; i++) {
        cur_lsp[0]             = FFMAX(cur_lsp[0],  0x180);
        cur_lsp[LPC_ORDER - 1] = FFMIN(cur_lsp[LPC_ORDER - 1], 0x7e00);

        /* Stability check */
        for (j = 1; j < LPC_ORDER; j++) {
            temp = min_dist + cur_lsp[j - 1] - cur_lsp[j];
            if (temp > 0) {
                temp >>= 1;
                cur_lsp[j - 1] -= temp;
                cur_lsp[j]     += temp;
            }
        }
        stable = 1;
        for (j = 1; j < LPC_ORDER; j++) {
            temp = cur_lsp[j - 1] + min_dist - cur_lsp[j] - 4;
            if (temp > 0) {
                stable = 0;
                break;
            }
        }
        if (stable)
            break;
    }
    if (!stable)
        memcpy(cur_lsp, prev_lsp, LPC_ORDER * sizeof(*cur_lsp));
}

/**
 * Bitexact implementation of 2ab scaled by 1/2^16.
 *
 * @param a 32 bit multiplicand
 * @param b 16 bit multiplier
 */
#define MULL2(a, b) \
        ((((a) >> 16) * (b) << 1) + (((a) & 0xffff) * (b) >> 15))

/**
 * Convert LSP frequencies to LPC coefficients.
 *
 * @param lpc buffer for LPC coefficients
 */
static void lsp2lpc(int16_t *lpc)
{
    int f1[LPC_ORDER / 2 + 1];
    int f2[LPC_ORDER / 2 + 1];
    int i, j;

    /* Calculate negative cosine */
    for (j = 0; j < LPC_ORDER; j++) {
        int index     = lpc[j] >> 7;
        int offset    = lpc[j] & 0x7f;
        int64_t temp1 = cos_tab[index] << 16;
        int temp2     = (cos_tab[index + 1] - cos_tab[index]) *
                          ((offset << 8) + 0x80) << 1;

        lpc[j] = -(av_clipl_int32(((temp1 + temp2) << 1) + (1 << 15)) >> 16);
    }

    /*
     * Compute sum and difference polynomial coefficients
     * (bitexact alternative to lsp2poly() in lsp.c)
     */
    /* Initialize with values in Q28 */
    f1[0] = 1 << 28;
    f1[1] = (lpc[0] << 14) + (lpc[2] << 14);
    f1[2] = lpc[0] * lpc[2] + (2 << 28);

    f2[0] = 1 << 28;
    f2[1] = (lpc[1] << 14) + (lpc[3] << 14);
    f2[2] = lpc[1] * lpc[3] + (2 << 28);

    /*
     * Calculate and scale the coefficients by 1/2 in
     * each iteration for a final scaling factor of Q25
     */
    for (i = 2; i < LPC_ORDER / 2; i++) {
        f1[i + 1] = f1[i - 1] + MULL2(f1[i], lpc[2 * i]);
        f2[i + 1] = f2[i - 1] + MULL2(f2[i], lpc[2 * i + 1]);

        for (j = i; j >= 2; j--) {
            f1[j] = MULL2(f1[j - 1], lpc[2 * i]) +
                    (f1[j] >> 1) + (f1[j - 2] >> 1);
            f2[j] = MULL2(f2[j - 1], lpc[2 * i + 1]) +
                    (f2[j] >> 1) + (f2[j - 2] >> 1);
        }

        f1[0] >>= 1;
        f2[0] >>= 1;
        f1[1] = ((lpc[2 * i]     << 16 >> i) + f1[1]) >> 1;
        f2[1] = ((lpc[2 * i + 1] << 16 >> i) + f2[1]) >> 1;
    }

    /* Convert polynomial coefficients to LPC coefficients */
    for (i = 0; i < LPC_ORDER / 2; i++) {
        int64_t ff1 = f1[i + 1] + f1[i];
        int64_t ff2 = f2[i + 1] - f2[i];

        lpc[i] = av_clipl_int32(((ff1 + ff2) << 3) + (1 << 15)) >> 16;
        lpc[LPC_ORDER - i - 1] = av_clipl_int32(((ff1 - ff2) << 3) +
                                                (1 << 15)) >> 16;
    }
}

/**
 * Quantize LSP frequencies by interpolation and convert them to
 * the corresponding LPC coefficients.
 *
 * @param lpc      buffer for LPC coefficients
 * @param cur_lsp  the current LSP vector
 * @param prev_lsp the previous LSP vector
 */
static void lsp_interpolate(int16_t *lpc, int16_t *cur_lsp, int16_t *prev_lsp)
{
    int i;
    int16_t *lpc_ptr = lpc;

    /* cur_lsp * 0.25 + prev_lsp * 0.75 */
    ff_acelp_weighted_vector_sum(lpc, cur_lsp, prev_lsp,
                                 4096, 12288, 1 << 13, 14, LPC_ORDER);
    ff_acelp_weighted_vector_sum(lpc + LPC_ORDER, cur_lsp, prev_lsp,
                                 8192, 8192, 1 << 13, 14, LPC_ORDER);
    ff_acelp_weighted_vector_sum(lpc + 2 * LPC_ORDER, cur_lsp, prev_lsp,
                                 12288, 4096, 1 << 13, 14, LPC_ORDER);
    memcpy(lpc + 3 * LPC_ORDER, cur_lsp, LPC_ORDER * sizeof(*lpc));

    for (i = 0; i < SUBFRAMES; i++) {
        lsp2lpc(lpc_ptr);
        lpc_ptr += LPC_ORDER;
    }
}

/**
 * Generate a train of dirac functions with period as pitch lag.
 */
static void gen_dirac_train(int16_t *buf, int pitch_lag)
{
    int16_t vector[SUBFRAME_LEN];
    int i, j;

    memcpy(vector, buf, SUBFRAME_LEN * sizeof(*vector));
    for (i = pitch_lag; i < SUBFRAME_LEN; i += pitch_lag) {
        for (j = 0; j < SUBFRAME_LEN - i; j++)
            buf[i + j] += vector[j];
    }
}

/**
 * Generate fixed codebook excitation vector.
 *
 * @param vector    decoded excitation vector
 * @param subfrm    current subframe
 * @param cur_rate  current bitrate
 * @param pitch_lag closed loop pitch lag
 * @param index     current subframe index
 */
static void gen_fcb_excitation(int16_t *vector, G723_1_Subframe subfrm,
                               enum Rate cur_rate, int pitch_lag, int index)
{
    int temp, i, j;

    memset(vector, 0, SUBFRAME_LEN * sizeof(*vector));

    if (cur_rate == RATE_6300) {
        if (subfrm.pulse_pos >= max_pos[index])
            return;

        /* Decode amplitudes and positions */
        j = PULSE_MAX - pulses[index];
        temp = subfrm.pulse_pos;
        for (i = 0; i < SUBFRAME_LEN / GRID_SIZE; i++) {
            temp -= combinatorial_table[j][i];
            if (temp >= 0)
                continue;
            temp += combinatorial_table[j++][i];
            if (subfrm.pulse_sign & (1 << (PULSE_MAX - j))) {
                vector[subfrm.grid_index + GRID_SIZE * i] =
                                        -fixed_cb_gain[subfrm.amp_index];
            } else {
                vector[subfrm.grid_index + GRID_SIZE * i] =
                                         fixed_cb_gain[subfrm.amp_index];
            }
            if (j == PULSE_MAX)
                break;
        }
        if (subfrm.dirac_train == 1)
            gen_dirac_train(vector, pitch_lag);
    } else { /* 5300 bps */
        int cb_gain  = fixed_cb_gain[subfrm.amp_index];
        int cb_shift = subfrm.grid_index;
        int cb_sign  = subfrm.pulse_sign;
        int cb_pos   = subfrm.pulse_pos;
        int offset, beta, lag;

        for (i = 0; i < 8; i += 2) {
            offset         = ((cb_pos & 7) << 3) + cb_shift + i;
            vector[offset] = (cb_sign & 1) ? cb_gain : -cb_gain;
            cb_pos  >>= 3;
            cb_sign >>= 1;
        }

        /* Enhance harmonic components */
        lag  = pitch_contrib[subfrm.ad_cb_gain << 1] + pitch_lag +
               subfrm.ad_cb_lag - 1;
        beta = pitch_contrib[(subfrm.ad_cb_gain << 1) + 1];

        if (lag < SUBFRAME_LEN - 2) {
            for (i = lag; i < SUBFRAME_LEN; i++)
                vector[i] += beta * vector[i - lag] >> 15;
        }
    }
}

/**
 * Get delayed contribution from the previous excitation vector.
 */
static void get_residual(int16_t *residual, int16_t *prev_excitation, int lag)
{
    int offset = PITCH_MAX - PITCH_ORDER / 2 - lag;
    int i;

    residual[0] = prev_excitation[offset];
    residual[1] = prev_excitation[offset + 1];

    offset += 2;
    for (i = 2; i < SUBFRAME_LEN + PITCH_ORDER - 1; i++)
        residual[i] = prev_excitation[offset + (i - 2) % lag];
}

static int dot_product(const int16_t *a, const int16_t *b, int length,
                       int shift)
{
    int i, sum = 0;

    for (i = 0; i < length; i++) {
        int64_t prod = av_clipl_int32(MUL64(a[i], b[i]) << shift);
        sum = av_clipl_int32(sum + prod);
    }
    return sum;
}

/**
 * Generate adaptive codebook excitation.
 */
static void gen_acb_excitation(int16_t *vector, int16_t *prev_excitation,
                               int pitch_lag, G723_1_Subframe subfrm,
                               enum Rate cur_rate)
{
    int16_t residual[SUBFRAME_LEN + PITCH_ORDER - 1];
    const int16_t *cb_ptr;
    int lag = pitch_lag + subfrm.ad_cb_lag - 1;

    int i;
    int64_t sum;

    get_residual(residual, prev_excitation, lag);

    /* Select quantization table */
    if (cur_rate == RATE_6300 && pitch_lag < SUBFRAME_LEN - 2)
        cb_ptr = adaptive_cb_gain85;
    else
        cb_ptr = adaptive_cb_gain170;

    /* Calculate adaptive vector */
    cb_ptr += subfrm.ad_cb_gain * 20;
    for (i = 0; i < SUBFRAME_LEN; i++) {
        sum = dot_product(residual + i, cb_ptr, PITCH_ORDER, 1);
        vector[i] = av_clipl_int32((sum << 1) + (1 << 15)) >> 16;
    }
}

/**
 * Estimate maximum auto-correlation around pitch lag.
 *
 * @param p         the context
 * @param offset    offset of the excitation vector
 * @param ccr_max   pointer to the maximum auto-correlation
 * @param pitch_lag decoded pitch lag
 * @param length    length of autocorrelation
 * @param dir       forward lag(1) / backward lag(-1)
 */
static int autocorr_max(G723_1_Context *p, int offset, int *ccr_max,
                        int pitch_lag, int length, int dir)
{
    int limit, ccr, lag = 0;
    int16_t *buf = p->excitation + offset;
    int i;

    pitch_lag = FFMIN(PITCH_MAX - 3, pitch_lag);
    if (dir > 0)
        limit = FFMIN(FRAME_LEN + PITCH_MAX - offset - length, pitch_lag + 3);
    else
        limit = pitch_lag + 3;

    for (i = pitch_lag - 3; i <= limit; i++) {
        ccr = dot_product(buf, buf + dir * i, length, 1);

        if (ccr > *ccr_max) {
            *ccr_max = ccr;
            lag = i;
        }
    }
    return lag;
}

/**
 * Calculate pitch postfilter optimal and scaling gains.
 *
 * @param lag      pitch postfilter forward/backward lag
 * @param ppf      pitch postfilter parameters
 * @param cur_rate current bitrate
 * @param tgt_eng  target energy
 * @param ccr      cross-correlation
 * @param res_eng  residual energy
 */
static void comp_ppf_gains(int lag, PPFParam *ppf, enum Rate cur_rate,
                           int tgt_eng, int ccr, int res_eng)
{
    int pf_residual;     /* square of postfiltered residual */
    int64_t temp1, temp2;

    ppf->index = lag;

    temp1 = tgt_eng * res_eng >> 1;
    temp2 = ccr * ccr << 1;

    if (temp2 > temp1) {
        if (ccr >= res_eng) {
            ppf->opt_gain = ppf_gain_weight[cur_rate];
        } else {
            ppf->opt_gain = (ccr << 15) / res_eng *
                            ppf_gain_weight[cur_rate] >> 15;
        }
        /* pf_res^2 = tgt_eng + 2*ccr*gain + res_eng*gain^2 */
        temp1       = (tgt_eng << 15) + (ccr * ppf->opt_gain << 1);
        temp2       = (ppf->opt_gain * ppf->opt_gain >> 15) * res_eng;
        pf_residual = av_clipl_int32(temp1 + temp2 + (1 << 15)) >> 16;

        if (tgt_eng >= pf_residual << 1) {
            temp1 = 0x7fff;
        } else {
            temp1 = (tgt_eng << 14) / pf_residual;
        }

        /* scaling_gain = sqrt(tgt_eng/pf_res^2) */
        ppf->sc_gain = square_root(temp1 << 16);
    } else {
        ppf->opt_gain = 0;
        ppf->sc_gain  = 0x7fff;
    }

    ppf->opt_gain = av_clip_int16(ppf->opt_gain * ppf->sc_gain >> 15);
}

/**
 * Calculate pitch postfilter parameters.
 *
 * @param p         the context
 * @param offset    offset of the excitation vector
 * @param pitch_lag decoded pitch lag
 * @param ppf       pitch postfilter parameters
 * @param cur_rate  current bitrate
 */
static void comp_ppf_coeff(G723_1_Context *p, int offset, int pitch_lag,
                           PPFParam *ppf, enum Rate cur_rate)
{

    int16_t scale;
    int i;
    int64_t temp1, temp2;

    /*
     * 0 - target energy
     * 1 - forward cross-correlation
     * 2 - forward residual energy
     * 3 - backward cross-correlation
     * 4 - backward residual energy
     */
    int energy[5] = {0, 0, 0, 0, 0};
    int16_t *buf  = p->excitation + offset;
    int fwd_lag   = autocorr_max(p, offset, &energy[1], pitch_lag,
                                 SUBFRAME_LEN, 1);
    int back_lag  = autocorr_max(p, offset, &energy[3], pitch_lag,
                                 SUBFRAME_LEN, -1);

    ppf->index    = 0;
    ppf->opt_gain = 0;
    ppf->sc_gain  = 0x7fff;

    /* Case 0, Section 3.6 */
    if (!back_lag && !fwd_lag)
        return;

    /* Compute target energy */
    energy[0] = dot_product(buf, buf, SUBFRAME_LEN, 1);

    /* Compute forward residual energy */
    if (fwd_lag)
        energy[2] = dot_product(buf + fwd_lag, buf + fwd_lag,
                                SUBFRAME_LEN, 1);

    /* Compute backward residual energy */
    if (back_lag)
        energy[4] = dot_product(buf - back_lag, buf - back_lag,
                                SUBFRAME_LEN, 1);

    /* Normalize and shorten */
    temp1 = 0;
    for (i = 0; i < 5; i++)
        temp1 = FFMAX(energy[i], temp1);

    scale = normalize_bits(temp1, 31);
    for (i = 0; i < 5; i++)
        energy[i] = (energy[i] << scale) >> 16;

    if (fwd_lag && !back_lag) {  /* Case 1 */
        comp_ppf_gains(fwd_lag,  ppf, cur_rate, energy[0], energy[1],
                       energy[2]);
    } else if (!fwd_lag) {       /* Case 2 */
        comp_ppf_gains(-back_lag, ppf, cur_rate, energy[0], energy[3],
                       energy[4]);
    } else {                     /* Case 3 */

        /*
         * Select the largest of energy[1]^2/energy[2]
         * and energy[3]^2/energy[4]
         */
        temp1 = energy[4] * ((energy[1] * energy[1] + (1 << 14)) >> 15);
        temp2 = energy[2] * ((energy[3] * energy[3] + (1 << 14)) >> 15);
        if (temp1 >= temp2) {
            comp_ppf_gains(fwd_lag, ppf, cur_rate, energy[0], energy[1],
                           energy[2]);
        } else {
            comp_ppf_gains(-back_lag, ppf, cur_rate, energy[0], energy[3],
                           energy[4]);
        }
    }
}

/**
 * Classify frames as voiced/unvoiced.
 *
 * @param p         the context
 * @param pitch_lag decoded pitch_lag
 * @param exc_eng   excitation energy estimation
 * @param scale     scaling factor of exc_eng
 *
 * @return residual interpolation index if voiced, 0 otherwise
 */
static int comp_interp_index(G723_1_Context *p, int pitch_lag,
                             int *exc_eng, int *scale)
{
    int offset = PITCH_MAX + 2 * SUBFRAME_LEN;
    int16_t *buf = p->excitation + offset;

    int index, ccr, tgt_eng, best_eng, temp;

    *scale = scale_vector(p->excitation, FRAME_LEN + PITCH_MAX);

    /* Compute maximum backward cross-correlation */
    ccr   = 0;
    index = autocorr_max(p, offset, &ccr, pitch_lag, SUBFRAME_LEN * 2, -1);
    ccr   = av_clipl_int32((int64_t)ccr + (1 << 15)) >> 16;

    /* Compute target energy */
    tgt_eng  = dot_product(buf, buf, SUBFRAME_LEN * 2, 1);
    *exc_eng = av_clipl_int32((int64_t)tgt_eng + (1 << 15)) >> 16;

    if (ccr <= 0)
        return 0;

    /* Compute best energy */
    best_eng = dot_product(buf - index, buf - index,
                           SUBFRAME_LEN * 2, 1);
    best_eng = av_clipl_int32((int64_t)best_eng + (1 << 15)) >> 16;

    temp = best_eng * *exc_eng >> 3;

    if (temp < ccr * ccr)
        return index;
    else
        return 0;
}

/**
 * Peform residual interpolation based on frame classification.
 *
 * @param buf   decoded excitation vector
 * @param out   output vector
 * @param lag   decoded pitch lag
 * @param gain  interpolated gain
 * @param rseed seed for random number generator
 */
static void residual_interp(int16_t *buf, int16_t *out, int lag,
                            int gain, int *rseed)
{
    int i;
    if (lag) { /* Voiced */
        int16_t *vector_ptr = buf + PITCH_MAX;
        /* Attenuate */
        for (i = 0; i < lag; i++)
            vector_ptr[i - lag] = vector_ptr[i - lag] * 3 >> 2;
        av_memcpy_backptr((uint8_t*)vector_ptr, lag * sizeof(*vector_ptr),
                          FRAME_LEN * sizeof(*vector_ptr));
        memcpy(out, vector_ptr, FRAME_LEN * sizeof(*vector_ptr));
    } else {  /* Unvoiced */
        for (i = 0; i < FRAME_LEN; i++) {
            *rseed = *rseed * 521 + 259;
            out[i] = gain * *rseed >> 15;
        }
        memset(buf, 0, (FRAME_LEN + PITCH_MAX) * sizeof(*buf));
    }
}

/**
 * Perform IIR filtering.
 *
 * @param fir_coef FIR coefficients
 * @param iir_coef IIR coefficients
 * @param src      source vector
 * @param dest     destination vector
 */
static inline void iir_filter(int16_t *fir_coef, int16_t *iir_coef,
                              int16_t *src, int *dest)
{
    int m, n;

    for (m = 0; m < SUBFRAME_LEN; m++) {
        int64_t filter = 0;
        for (n = 1; n <= LPC_ORDER; n++) {
            filter -= fir_coef[n - 1] * src[m - n] -
                      iir_coef[n - 1] * (dest[m - n] >> 16);
        }

        dest[m] = av_clipl_int32((src[m] << 16) + (filter << 3) + (1 << 15));
    }
}

/**
 * Adjust gain of postfiltered signal.
 *
 * @param p      the context
 * @param buf    postfiltered output vector
 * @param energy input energy coefficient
 */
static void gain_scale(G723_1_Context *p, int16_t * buf, int energy)
{
    int num, denom, gain, bits1, bits2;
    int i;

    num   = energy;
    denom = 0;
    for (i = 0; i < SUBFRAME_LEN; i++) {
        int64_t temp = buf[i] >> 2;
        temp  = av_clipl_int32(MUL64(temp, temp) << 1);
        denom = av_clipl_int32(denom + temp);
    }

    if (num && denom) {
        bits1   = normalize_bits(num,   31);
        bits2   = normalize_bits(denom, 31);
        num     = num << bits1 >> 1;
        denom <<= bits2;

        bits2 = 5 + bits1 - bits2;
        bits2 = FFMAX(0, bits2);

        gain = (num >> 1) / (denom >> 16);
        gain = square_root(gain << 16 >> bits2);
    } else {
        gain = 1 << 12;
    }

    for (i = 0; i < SUBFRAME_LEN; i++) {
        p->pf_gain = ((p->pf_gain << 4) - p->pf_gain + gain + (1 << 3)) >> 4;
        buf[i]     = av_clip_int16((buf[i] * (p->pf_gain + (p->pf_gain >> 4)) +
                                   (1 << 10)) >> 11);
    }
}

/**
 * Perform formant filtering.
 *
 * @param p   the context
 * @param lpc quantized lpc coefficients
 * @param buf output buffer
 */
static void formant_postfilter(G723_1_Context *p, int16_t *lpc, int16_t *buf)
{
    int16_t filter_coef[2][LPC_ORDER], *buf_ptr;
    int filter_signal[LPC_ORDER + FRAME_LEN], *signal_ptr;
    int i, j, k;

    memcpy(buf, p->fir_mem, LPC_ORDER * sizeof(*buf));
    memcpy(filter_signal, p->iir_mem, LPC_ORDER * sizeof(*filter_signal));

    for (i = LPC_ORDER, j = 0; j < SUBFRAMES; i += SUBFRAME_LEN, j++) {
        for (k = 0; k < LPC_ORDER; k++) {
            filter_coef[0][k] = (-lpc[k] * postfilter_tbl[0][k] +
                                 (1 << 14)) >> 15;
            filter_coef[1][k] = (-lpc[k] * postfilter_tbl[1][k] +
                                 (1 << 14)) >> 15;
        }
        iir_filter(filter_coef[0], filter_coef[1], buf + i,
                   filter_signal + i);
        lpc += LPC_ORDER;
    }

    memcpy(p->fir_mem, buf + FRAME_LEN, LPC_ORDER * sizeof(*p->fir_mem));
    memcpy(p->iir_mem, filter_signal + FRAME_LEN,
           LPC_ORDER * sizeof(*p->iir_mem));

    buf_ptr    = buf + LPC_ORDER;
    signal_ptr = filter_signal + LPC_ORDER;
    for (i = 0; i < SUBFRAMES; i++) {
        int16_t temp_vector[SUBFRAME_LEN];
        int16_t temp;
        int auto_corr[2];
        int scale, energy;

        /* Normalize */
        memcpy(temp_vector, buf_ptr, SUBFRAME_LEN * sizeof(*temp_vector));
        scale = scale_vector(temp_vector, SUBFRAME_LEN);

        /* Compute auto correlation coefficients */
        auto_corr[0] = dot_product(temp_vector, temp_vector + 1,
                                   SUBFRAME_LEN - 1, 1);
        auto_corr[1] = dot_product(temp_vector, temp_vector, SUBFRAME_LEN, 1);

        /* Compute reflection coefficient */
        temp = auto_corr[1] >> 16;
        if (temp) {
            temp = (auto_corr[0] >> 2) / temp;
        }
        p->reflection_coef = ((p->reflection_coef << 2) - p->reflection_coef +
                              temp + 2) >> 2;
        temp = (p->reflection_coef * 0xffffc >> 3) & 0xfffc;

        /* Compensation filter */
        for (j = 0; j < SUBFRAME_LEN; j++) {
            buf_ptr[j] = av_clipl_int32(signal_ptr[j] +
                                        ((signal_ptr[j - 1] >> 16) *
                                         temp << 1)) >> 16;
        }

        /* Compute normalized signal energy */
        temp = 2 * scale + 4;
        if (temp < 0) {
            energy = av_clipl_int32((int64_t)auto_corr[1] << -temp);
        } else
            energy = auto_corr[1] >> temp;

        gain_scale(p, buf_ptr, energy);

        buf_ptr    += SUBFRAME_LEN;
        signal_ptr += SUBFRAME_LEN;
    }
}

static int g723_1_decode_frame(AVCodecContext *avctx, void *data,
                               int *got_frame_ptr, AVPacket *avpkt)
{
    G723_1_Context *p  = avctx->priv_data;
    const uint8_t *buf = avpkt->data;
    int buf_size       = avpkt->size;
    int dec_mode       = buf[0] & 3;

    PPFParam ppf[SUBFRAMES];
    int16_t cur_lsp[LPC_ORDER];
    int16_t lpc[SUBFRAMES * LPC_ORDER];
    int16_t acb_vector[SUBFRAME_LEN];
    int16_t *vector_ptr;
    int16_t *out;
    int bad_frame = 0, i, j, ret;

    if (buf_size < frame_size[dec_mode]) {
        if (buf_size)
            av_log(avctx, AV_LOG_WARNING,
                   "Expected %d bytes, got %d - skipping packet\n",
                   frame_size[dec_mode], buf_size);
        *got_frame_ptr = 0;
        return buf_size;
    }

    if (unpack_bitstream(p, buf, buf_size) < 0) {
        bad_frame = 1;
        if (p->past_frame_type == ACTIVE_FRAME)
            p->cur_frame_type = ACTIVE_FRAME;
        else
            p->cur_frame_type = UNTRANSMITTED_FRAME;
    }

    p->frame.nb_samples = FRAME_LEN;
    if ((ret = avctx->get_buffer(avctx, &p->frame)) < 0) {
         av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
         return ret;
    }

    out = (int16_t *)p->frame.data[0];

    if (p->cur_frame_type == ACTIVE_FRAME) {
        if (!bad_frame)
            p->erased_frames = 0;
        else if (p->erased_frames != 3)
            p->erased_frames++;

        inverse_quant(cur_lsp, p->prev_lsp, p->lsp_index, bad_frame);
        lsp_interpolate(lpc, cur_lsp, p->prev_lsp);

        /* Save the lsp_vector for the next frame */
        memcpy(p->prev_lsp, cur_lsp, LPC_ORDER * sizeof(*p->prev_lsp));

        /* Generate the excitation for the frame */
        memcpy(p->excitation, p->prev_excitation,
               PITCH_MAX * sizeof(*p->excitation));
        vector_ptr = p->excitation + PITCH_MAX;
        if (!p->erased_frames) {
            /* Update interpolation gain memory */
            p->interp_gain = fixed_cb_gain[(p->subframe[2].amp_index +
                                            p->subframe[3].amp_index) >> 1];
            for (i = 0; i < SUBFRAMES; i++) {
                gen_fcb_excitation(vector_ptr, p->subframe[i], p->cur_rate,
                                   p->pitch_lag[i >> 1], i);
                gen_acb_excitation(acb_vector, &p->excitation[SUBFRAME_LEN * i],
                                   p->pitch_lag[i >> 1], p->subframe[i],
                                   p->cur_rate);
                /* Get the total excitation */
                for (j = 0; j < SUBFRAME_LEN; j++) {
                    vector_ptr[j] = av_clip_int16(vector_ptr[j] << 1);
                    vector_ptr[j] = av_clip_int16(vector_ptr[j] +
                                                  acb_vector[j]);
                }
                vector_ptr += SUBFRAME_LEN;
            }

            vector_ptr = p->excitation + PITCH_MAX;

            /* Save the excitation */
            memcpy(p->audio + LPC_ORDER, vector_ptr, FRAME_LEN * sizeof(*p->audio));

            p->interp_index = comp_interp_index(p, p->pitch_lag[1],
                                                &p->sid_gain, &p->cur_gain);

            if (p->postfilter) {
                i = PITCH_MAX;
                for (j = 0; j < SUBFRAMES; i += SUBFRAME_LEN, j++)
                    comp_ppf_coeff(p, i, p->pitch_lag[j >> 1],
                                   ppf + j, p->cur_rate);
            }

            /* Restore the original excitation */
            memcpy(p->excitation, p->prev_excitation,
                   PITCH_MAX * sizeof(*p->excitation));
            memcpy(vector_ptr, p->audio + LPC_ORDER, FRAME_LEN * sizeof(*vector_ptr));

            /* Peform pitch postfiltering */
            if (p->postfilter)
                for (i = 0, j = 0; j < SUBFRAMES; i += SUBFRAME_LEN, j++)
                    ff_acelp_weighted_vector_sum(p->audio + LPC_ORDER + i,
                                                 vector_ptr + i,
                                                 vector_ptr + i + ppf[j].index,
                                                 ppf[j].sc_gain,
                                                 ppf[j].opt_gain,
                                                 1 << 14, 15, SUBFRAME_LEN);

        } else {
            p->interp_gain = (p->interp_gain * 3 + 2) >> 2;
            if (p->erased_frames == 3) {
                /* Mute output */
                memset(p->excitation, 0,
                       (FRAME_LEN + PITCH_MAX) * sizeof(*p->excitation));
                memset(p->frame.data[0], 0,
                       (FRAME_LEN + LPC_ORDER) * sizeof(int16_t));
            } else {
                /* Regenerate frame */
                residual_interp(p->excitation, p->audio + LPC_ORDER, p->interp_index,
                                p->interp_gain, &p->random_seed);
            }
        }
        /* Save the excitation for the next frame */
        memcpy(p->prev_excitation, p->excitation + FRAME_LEN,
               PITCH_MAX * sizeof(*p->excitation));
    } else {
        memset(out, 0, FRAME_LEN * 2);
        av_log(avctx, AV_LOG_WARNING,
               "G.723.1: Comfort noise generation not supported yet\n");

        *got_frame_ptr   = 1;
        *(AVFrame *)data = p->frame;
        return frame_size[dec_mode];
    }

    p->past_frame_type = p->cur_frame_type;

    memcpy(p->audio, p->synth_mem, LPC_ORDER * sizeof(*p->audio));
    for (i = LPC_ORDER, j = 0; j < SUBFRAMES; i += SUBFRAME_LEN, j++)
        ff_celp_lp_synthesis_filter(p->audio + i, &lpc[j * LPC_ORDER],
                                    p->audio + i, SUBFRAME_LEN, LPC_ORDER,
                                    0, 1, 1 << 12);
    memcpy(p->synth_mem, p->audio + FRAME_LEN, LPC_ORDER * sizeof(*p->audio));

    if (p->postfilter) {
        formant_postfilter(p, lpc, p->audio);
        memcpy(p->frame.data[0], p->audio + LPC_ORDER, FRAME_LEN * 2);
    } else { // if output is not postfiltered it should be scaled by 2
        for (i = 0; i < FRAME_LEN; i++)
            out[i] = av_clip_int16(p->audio[LPC_ORDER + i] << 1);
    }

    *got_frame_ptr   = 1;
    *(AVFrame *)data = p->frame;

    return frame_size[dec_mode];
}

#define OFFSET(x) offsetof(G723_1_Context, x)
#define AD     AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    { "postfilter", "postfilter on/off", OFFSET(postfilter), AV_OPT_TYPE_INT,
      { 1 }, 0, 1, AD },
    { NULL }
};


static const AVClass g723_1dec_class = {
    .class_name = "G.723.1 decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_g723_1_decoder = {
    .name           = "g723_1",
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_G723_1,
    .priv_data_size = sizeof(G723_1_Context),
    .init           = g723_1_decode_init,
    .decode         = g723_1_decode_frame,
    .long_name      = NULL_IF_CONFIG_SMALL("G.723.1"),
    .capabilities   = CODEC_CAP_SUBFRAMES,
    .priv_class     = &g723_1dec_class,
};