/*
 * Duck/ON2 TrueMotion 2 Decoder
 * Copyright (c) 2005 Konstantin Shishkov
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
 * Duck TrueMotion2 decoder.
 */

#include "avcodec.h"
#include "bytestream.h"
#include "get_bits.h"
#include "dsputil.h"
#include "internal.h"

#define TM2_ESCAPE 0x80000000
#define TM2_DELTAS 64

/* Huffman-coded streams of different types of blocks */
enum TM2_STREAMS {
    TM2_C_HI = 0,
    TM2_C_LO,
    TM2_L_HI,
    TM2_L_LO,
    TM2_UPD,
    TM2_MOT,
    TM2_TYPE,
    TM2_NUM_STREAMS
};

/* Block types */
enum TM2_BLOCKS {
    TM2_HI_RES = 0,
    TM2_MED_RES,
    TM2_LOW_RES,
    TM2_NULL_RES,
    TM2_UPDATE,
    TM2_STILL,
    TM2_MOTION
};

typedef struct TM2Context {
    AVCodecContext *avctx;
    AVFrame pic;

    GetBitContext gb;
    DSPContext dsp;

    /* TM2 streams */
    int *tokens[TM2_NUM_STREAMS];
    int tok_lens[TM2_NUM_STREAMS];
    int tok_ptrs[TM2_NUM_STREAMS];
    int deltas[TM2_NUM_STREAMS][TM2_DELTAS];
    /* for blocks decoding */
    int D[4];
    int CD[4];
    int *last;
    int *clast;

    /* data for current and previous frame */
    int *Y1_base, *U1_base, *V1_base, *Y2_base, *U2_base, *V2_base;
    int *Y1, *U1, *V1, *Y2, *U2, *V2;
    int y_stride, uv_stride;
    int cur;
} TM2Context;

/**
* Huffman codes for each of streams
*/
typedef struct TM2Codes {
    VLC vlc; ///< table for Libav bitstream reader
    int bits;
    int *recode; ///< table for converting from code indexes to values
    int length;
} TM2Codes;

/**
* structure for gathering Huffman codes information
*/
typedef struct TM2Huff {
    int val_bits; ///< length of literal
    int max_bits; ///< maximum length of code
    int min_bits; ///< minimum length of code
    int nodes; ///< total number of nodes in tree
    int num; ///< current number filled
    int max_num; ///< total number of codes
    int *nums; ///< literals
    uint32_t *bits; ///< codes
    int *lens; ///< codelengths
} TM2Huff;

static int tm2_read_tree(TM2Context *ctx, uint32_t prefix, int length, TM2Huff *huff)
{
    int ret;
    if (length > huff->max_bits) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Tree exceeded its given depth (%i)\n",
               huff->max_bits);
        return AVERROR_INVALIDDATA;
    }

    if (!get_bits1(&ctx->gb)) { /* literal */
        if (length == 0) {
            length = 1;
        }
        if (huff->num >= huff->max_num) {
            av_log(ctx->avctx, AV_LOG_DEBUG, "Too many literals\n");
            return AVERROR_INVALIDDATA;
        }
        huff->nums[huff->num] = get_bits_long(&ctx->gb, huff->val_bits);
        huff->bits[huff->num] = prefix;
        huff->lens[huff->num] = length;
        huff->num++;
        return 0;
    } else { /* non-terminal node */
        if ((ret = tm2_read_tree(ctx, prefix << 1, length + 1, huff)) < 0)
            return ret;
        if ((ret = tm2_read_tree(ctx, (prefix << 1) | 1, length + 1, huff)) < 0)
            return ret;
    }
    return 0;
}

static int tm2_build_huff_table(TM2Context *ctx, TM2Codes *code)
{
    TM2Huff huff;
    int res = 0;

    huff.val_bits = get_bits(&ctx->gb, 5);
    huff.max_bits = get_bits(&ctx->gb, 5);
    huff.min_bits = get_bits(&ctx->gb, 5);
    huff.nodes    = get_bits_long(&ctx->gb, 17);
    huff.num      = 0;

    /* check for correct codes parameters */
    if ((huff.val_bits < 1) || (huff.val_bits > 32) ||
        (huff.max_bits < 0) || (huff.max_bits > 25)) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Incorrect tree parameters - literal "
               "length: %i, max code length: %i\n", huff.val_bits, huff.max_bits);
        return AVERROR_INVALIDDATA;
    }
    if ((huff.nodes <= 0) || (huff.nodes > 0x10000)) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Incorrect number of Huffman tree "
               "nodes: %i\n", huff.nodes);
        return AVERROR_INVALIDDATA;
    }
    /* one-node tree */
    if (huff.max_bits == 0)
        huff.max_bits = 1;

    /* allocate space for codes - it is exactly ceil(nodes / 2) entries */
    huff.max_num = (huff.nodes + 1) >> 1;
    huff.nums    = av_mallocz(huff.max_num * sizeof(int));
    huff.bits    = av_mallocz(huff.max_num * sizeof(uint32_t));
    huff.lens    = av_mallocz(huff.max_num * sizeof(int));

    res = tm2_read_tree(ctx, 0, 0, &huff);

    if (huff.num != huff.max_num) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Got less codes than expected: %i of %i\n",
               huff.num, huff.max_num);
        res = AVERROR_INVALIDDATA;
    }

    /* convert codes to vlc_table */
    if (res >= 0) {
        int i;

        res = init_vlc(&code->vlc, huff.max_bits, huff.max_num,
                       huff.lens, sizeof(int), sizeof(int),
                       huff.bits, sizeof(uint32_t), sizeof(uint32_t), 0);
        if (res < 0)
            av_log(ctx->avctx, AV_LOG_ERROR, "Cannot build VLC table\n");
        else {
            code->bits = huff.max_bits;
            code->length = huff.max_num;
            code->recode = av_malloc(code->length * sizeof(int));
            for (i = 0; i < code->length; i++)
                code->recode[i] = huff.nums[i];
        }
    }
    /* free allocated memory */
    av_free(huff.nums);
    av_free(huff.bits);
    av_free(huff.lens);

    return res;
}

static void tm2_free_codes(TM2Codes *code)
{
    av_free(code->recode);
    if (code->vlc.table)
        ff_free_vlc(&code->vlc);
}

static inline int tm2_get_token(GetBitContext *gb, TM2Codes *code)
{
    int val;
    val = get_vlc2(gb, code->vlc.table, code->bits, 1);
    return code->recode[val];
}

#define TM2_OLD_HEADER_MAGIC 0x00000100
#define TM2_NEW_HEADER_MAGIC 0x00000101

static inline int tm2_read_header(TM2Context *ctx, const uint8_t *buf)
{
    uint32_t magic = AV_RL32(buf);

    switch (magic) {
    case TM2_OLD_HEADER_MAGIC:
        av_log_missing_feature(ctx->avctx, "TM2 old header", 1);
        return 0;
    case TM2_NEW_HEADER_MAGIC:
        return 0;
    default:
        av_log(ctx->avctx, AV_LOG_ERROR, "Not a TM2 header: 0x%08X\n", magic);
        return AVERROR_INVALIDDATA;
    }
}

static int tm2_read_deltas(TM2Context *ctx, int stream_id)
{
    int d, mb;
    int i, v;

    d  = get_bits(&ctx->gb, 9);
    mb = get_bits(&ctx->gb, 5);

    if ((d < 1) || (d > TM2_DELTAS) || (mb < 1) || (mb > 32)) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Incorrect delta table: %i deltas x %i bits\n", d, mb);
        return AVERROR_INVALIDDATA;
    }

    for (i = 0; i < d; i++) {
        v = get_bits_long(&ctx->gb, mb);
        if (v & (1 << (mb - 1)))
            ctx->deltas[stream_id][i] = v - (1 << mb);
        else
            ctx->deltas[stream_id][i] = v;
    }
    for (; i < TM2_DELTAS; i++)
        ctx->deltas[stream_id][i] = 0;

    return 0;
}

static int tm2_read_stream(TM2Context *ctx, const uint8_t *buf, int stream_id, int buf_size)
{
    int i, ret;
    int skip = 0;
    int len, toks, pos;
    TM2Codes codes;
    GetByteContext gb;

    /* get stream length in dwords */
    bytestream2_init(&gb, buf, buf_size);
    len  = bytestream2_get_be32(&gb);
    skip = len * 4 + 4;

    if (len == 0)
        return 4;

    if (len >= INT_MAX/4-1 || len < 0 || len > buf_size) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Error, invalid stream size.\n");
        return AVERROR_INVALIDDATA;
    }

    toks = bytestream2_get_be32(&gb);
    if (toks & 1) {
        len = bytestream2_get_be32(&gb);
        if (len == TM2_ESCAPE) {
            len = bytestream2_get_be32(&gb);
        }
        if (len > 0) {
            pos = bytestream2_tell(&gb);
            if (skip <= pos)
                return AVERROR_INVALIDDATA;
            init_get_bits(&ctx->gb, buf + pos, (skip - pos) * 8);
            if ((ret = tm2_read_deltas(ctx, stream_id)) < 0)
                return ret;
            bytestream2_skip(&gb, ((get_bits_count(&ctx->gb) + 31) >> 5) << 2);
        }
    }
    /* skip unused fields */
    len = bytestream2_get_be32(&gb);
    if (len == TM2_ESCAPE) { /* some unknown length - could be escaped too */
        bytestream2_skip(&gb, 8); /* unused by decoder */
    } else {
        bytestream2_skip(&gb, 4); /* unused by decoder */
    }

    pos = bytestream2_tell(&gb);
    if (skip <= pos)
        return AVERROR_INVALIDDATA;
    init_get_bits(&ctx->gb, buf + pos, (skip - pos) * 8);
    if ((ret = tm2_build_huff_table(ctx, &codes)) < 0)
        return ret;
    bytestream2_skip(&gb, ((get_bits_count(&ctx->gb) + 31) >> 5) << 2);

    toks >>= 1;
    /* check if we have sane number of tokens */
    if ((toks < 0) || (toks > 0xFFFFFF)) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Incorrect number of tokens: %i\n", toks);
        tm2_free_codes(&codes);
        return AVERROR_INVALIDDATA;
    }
    ctx->tokens[stream_id]   = av_realloc(ctx->tokens[stream_id], toks * sizeof(int));
    ctx->tok_lens[stream_id] = toks;
    len = bytestream2_get_be32(&gb);
    if (len > 0) {
        pos = bytestream2_tell(&gb);
        if (skip <= pos)
            return AVERROR_INVALIDDATA;
        init_get_bits(&ctx->gb, buf + pos, (skip - pos) * 8);
        for (i = 0; i < toks; i++) {
            if (get_bits_left(&ctx->gb) <= 0) {
                av_log(ctx->avctx, AV_LOG_ERROR, "Incorrect number of tokens: %i\n", toks);
                return AVERROR_INVALIDDATA;
            }
            ctx->tokens[stream_id][i] = tm2_get_token(&ctx->gb, &codes);
            if (stream_id <= TM2_MOT && ctx->tokens[stream_id][i] >= TM2_DELTAS) {
                av_log(ctx->avctx, AV_LOG_ERROR, "Invalid delta token index %d for type %d, n=%d\n",
                       ctx->tokens[stream_id][i], stream_id, i);
                return AVERROR_INVALIDDATA;
            }
        }
    } else {
        for (i = 0; i < toks; i++) {
            ctx->tokens[stream_id][i] = codes.recode[0];
            if (stream_id <= TM2_MOT && ctx->tokens[stream_id][i] >= TM2_DELTAS) {
                av_log(ctx->avctx, AV_LOG_ERROR, "Invalid delta token index %d for type %d, n=%d\n",
                       ctx->tokens[stream_id][i], stream_id, i);
                return AVERROR_INVALIDDATA;
            }
        }
    }
    tm2_free_codes(&codes);

    return skip;
}

static inline int GET_TOK(TM2Context *ctx,int type)
{
    if (ctx->tok_ptrs[type] >= ctx->tok_lens[type]) {
        av_log(ctx->avctx, AV_LOG_ERROR, "Read token from stream %i out of bounds (%i>=%i)\n", type, ctx->tok_ptrs[type], ctx->tok_lens[type]);
        return 0;
    }
    if (type <= TM2_MOT)
        return ctx->deltas[type][ctx->tokens[type][ctx->tok_ptrs[type]++]];
    return ctx->tokens[type][ctx->tok_ptrs[type]++];
}

/* blocks decoding routines */

/* common Y, U, V pointers initialisation */
#define TM2_INIT_POINTERS() \
    int *last, *clast; \
    int *Y, *U, *V;\
    int Ystride, Ustride, Vstride;\
\
    Ystride = ctx->y_stride;\
    Vstride = ctx->uv_stride;\
    Ustride = ctx->uv_stride;\
    Y = (ctx->cur?ctx->Y2:ctx->Y1) + by * 4 * Ystride + bx * 4;\
    V = (ctx->cur?ctx->V2:ctx->V1) + by * 2 * Vstride + bx * 2;\
    U = (ctx->cur?ctx->U2:ctx->U1) + by * 2 * Ustride + bx * 2;\
    last = ctx->last + bx * 4;\
    clast = ctx->clast + bx * 4;

#define TM2_INIT_POINTERS_2() \
    int *Yo, *Uo, *Vo;\
    int oYstride, oUstride, oVstride;\
\
    TM2_INIT_POINTERS();\
    oYstride = Ystride;\
    oVstride = Vstride;\
    oUstride = Ustride;\
    Yo = (ctx->cur?ctx->Y1:ctx->Y2) + by * 4 * oYstride + bx * 4;\
    Vo = (ctx->cur?ctx->V1:ctx->V2) + by * 2 * oVstride + bx * 2;\
    Uo = (ctx->cur?ctx->U1:ctx->U2) + by * 2 * oUstride + bx * 2;

/* recalculate last and delta values for next blocks */
#define TM2_RECALC_BLOCK(CHR, stride, last, CD) {\
    CD[0] = CHR[1] - last[1];\
    CD[1] = (int)CHR[stride + 1] - (int)CHR[1];\
    last[0] = (int)CHR[stride + 0];\
    last[1] = (int)CHR[stride + 1];}

/* common operations - add deltas to 4x4 block of luma or 2x2 blocks of chroma */
static inline void tm2_apply_deltas(TM2Context *ctx, int* Y, int stride, int *deltas, int *last)
{
    int ct, d;
    int i, j;

    for (j = 0; j < 4; j++){
        ct = ctx->D[j];
        for (i = 0; i < 4; i++){
            d        = deltas[i + j * 4];
            ct      += d;
            last[i] += ct;
            Y[i]     = av_clip_uint8(last[i]);
        }
        Y        += stride;
        ctx->D[j] = ct;
    }
}

static inline void tm2_high_chroma(int *data, int stride, int *last, int *CD, int *deltas)
{
    int i, j;
    for (j = 0; j < 2; j++) {
        for (i = 0; i < 2; i++)  {
            CD[j]   += deltas[i + j * 2];
            last[i] += CD[j];
            data[i]  = last[i];
        }
        data += stride;
    }
}

static inline void tm2_low_chroma(int *data, int stride, int *clast, int *CD, int *deltas, int bx)
{
    int t;
    int l;
    int prev;

    if (bx > 0)
        prev = clast[-3];
    else
        prev = 0;
    t        = (CD[0] + CD[1]) >> 1;
    l        = (prev - CD[0] - CD[1] + clast[1]) >> 1;
    CD[1]    = CD[0] + CD[1] - t;
    CD[0]    = t;
    clast[0] = l;

    tm2_high_chroma(data, stride, clast, CD, deltas);
}

static inline void tm2_hi_res_block(TM2Context *ctx, AVFrame *pic, int bx, int by)
{
    int i;
    int deltas[16];
    TM2_INIT_POINTERS();

    /* hi-res chroma */
    for (i = 0; i < 4; i++) {
        deltas[i]     = GET_TOK(ctx, TM2_C_HI);
        deltas[i + 4] = GET_TOK(ctx, TM2_C_HI);
    }
    tm2_high_chroma(U, Ustride, clast,     ctx->CD,     deltas);
    tm2_high_chroma(V, Vstride, clast + 2, ctx->CD + 2, deltas + 4);

    /* hi-res luma */
    for (i = 0; i < 16; i++)
        deltas[i] = GET_TOK(ctx, TM2_L_HI);

    tm2_apply_deltas(ctx, Y, Ystride, deltas, last);
}

static inline void tm2_med_res_block(TM2Context *ctx, AVFrame *pic, int bx, int by)
{
    int i;
    int deltas[16];
    TM2_INIT_POINTERS();

    /* low-res chroma */
    deltas[0] = GET_TOK(ctx, TM2_C_LO);
    deltas[1] = deltas[2] = deltas[3] = 0;
    tm2_low_chroma(U, Ustride, clast, ctx->CD, deltas, bx);

    deltas[0] = GET_TOK(ctx, TM2_C_LO);
    deltas[1] = deltas[2] = deltas[3] = 0;
    tm2_low_chroma(V, Vstride, clast + 2, ctx->CD + 2, deltas, bx);

    /* hi-res luma */
    for (i = 0; i < 16; i++)
        deltas[i] = GET_TOK(ctx, TM2_L_HI);

    tm2_apply_deltas(ctx, Y, Ystride, deltas, last);
}

static inline void tm2_low_res_block(TM2Context *ctx, AVFrame *pic, int bx, int by)
{
    int i;
    int t1, t2;
    int deltas[16];
    TM2_INIT_POINTERS();

    /* low-res chroma */
    deltas[0] = GET_TOK(ctx, TM2_C_LO);
    deltas[1] = deltas[2] = deltas[3] = 0;
    tm2_low_chroma(U, Ustride, clast, ctx->CD, deltas, bx);

    deltas[0] = GET_TOK(ctx, TM2_C_LO);
    deltas[1] = deltas[2] = deltas[3] = 0;
    tm2_low_chroma(V, Vstride, clast + 2, ctx->CD + 2, deltas, bx);

    /* low-res luma */
    for (i = 0; i < 16; i++)
        deltas[i] = 0;

    deltas[ 0] = GET_TOK(ctx, TM2_L_LO);
    deltas[ 2] = GET_TOK(ctx, TM2_L_LO);
    deltas[ 8] = GET_TOK(ctx, TM2_L_LO);
    deltas[10] = GET_TOK(ctx, TM2_L_LO);

    if (bx > 0)
        last[0] = (last[-1] - ctx->D[0] - ctx->D[1] - ctx->D[2] - ctx->D[3] + last[1]) >> 1;
    else
        last[0] = (last[1]  - ctx->D[0] - ctx->D[1] - ctx->D[2] - ctx->D[3])>> 1;
    last[2] = (last[1] + last[3]) >> 1;

    t1 = ctx->D[0] + ctx->D[1];
    ctx->D[0] = t1 >> 1;
    ctx->D[1] = t1 - (t1 >> 1);
    t2 = ctx->D[2] + ctx->D[3];
    ctx->D[2] = t2 >> 1;
    ctx->D[3] = t2 - (t2 >> 1);

    tm2_apply_deltas(ctx, Y, Ystride, deltas, last);
}

static inline void tm2_null_res_block(TM2Context *ctx, AVFrame *pic, int bx, int by)
{
    int i;
    int ct;
    int left, right, diff;
    int deltas[16];
    TM2_INIT_POINTERS();

    /* null chroma */
    deltas[0] = deltas[1] = deltas[2] = deltas[3] = 0;
    tm2_low_chroma(U, Ustride, clast, ctx->CD, deltas, bx);

    deltas[0] = deltas[1] = deltas[2] = deltas[3] = 0;
    tm2_low_chroma(V, Vstride, clast + 2, ctx->CD + 2, deltas, bx);

    /* null luma */
    for (i = 0; i < 16; i++)
        deltas[i] = 0;

    ct = ctx->D[0] + ctx->D[1] + ctx->D[2] + ctx->D[3];

    if (bx > 0)
        left = last[-1] - ct;
    else
        left = 0;

    right   = last[3];
    diff    = right - left;
    last[0] = left + (diff >> 2);
    last[1] = left + (diff >> 1);
    last[2] = right - (diff >> 2);
    last[3] = right;
    {
        int tp = left;

        ctx->D[0] = (tp + (ct >> 2)) - left;
        left     += ctx->D[0];
        ctx->D[1] = (tp + (ct >> 1)) - left;
        left     += ctx->D[1];
        ctx->D[2] = ((tp + ct) - (ct >> 2)) - left;
        left     += ctx->D[2];
        ctx->D[3] = (tp + ct) - left;
    }
    tm2_apply_deltas(ctx, Y, Ystride, deltas, last);
}

static inline void tm2_still_block(TM2Context *ctx, AVFrame *pic, int bx, int by)
{
    int i, j;
    TM2_INIT_POINTERS_2();

    /* update chroma */
    for (j = 0; j < 2; j++) {
        for (i = 0; i < 2; i++){
            U[i] = Uo[i];
            V[i] = Vo[i];
        }
        U  += Ustride; V += Vstride;
        Uo += oUstride; Vo += oVstride;
    }
    U -= Ustride * 2;
    V -= Vstride * 2;
    TM2_RECALC_BLOCK(U, Ustride, clast, ctx->CD);
    TM2_RECALC_BLOCK(V, Vstride, (clast + 2), (ctx->CD + 2));

    /* update deltas */
    ctx->D[0] = Yo[3] - last[3];
    ctx->D[1] = Yo[3 + oYstride] - Yo[3];
    ctx->D[2] = Yo[3 + oYstride * 2] - Yo[3 + oYstride];
    ctx->D[3] = Yo[3 + oYstride * 3] - Yo[3 + oYstride * 2];

    for (j = 0; j < 4; j++) {
        for (i = 0; i < 4; i++) {
            Y[i]    = Yo[i];
            last[i] = Yo[i];
        }
        Y  += Ystride;
        Yo += oYstride;
    }
}

static inline void tm2_update_block(TM2Context *ctx, AVFrame *pic, int bx, int by)
{
    int i, j;
    int d;
    TM2_INIT_POINTERS_2();

    /* update chroma */
    for (j = 0; j < 2; j++) {
        for (i = 0; i < 2; i++) {
            U[i] = Uo[i] + GET_TOK(ctx, TM2_UPD);
            V[i] = Vo[i] + GET_TOK(ctx, TM2_UPD);
        }
        U  += Ustride;
        V  += Vstride;
        Uo += oUstride;
        Vo += oVstride;
    }
    U -= Ustride * 2;
    V -= Vstride * 2;
    TM2_RECALC_BLOCK(U, Ustride, clast, ctx->CD);
    TM2_RECALC_BLOCK(V, Vstride, (clast + 2), (ctx->CD + 2));

    /* update deltas */
    ctx->D[0] = Yo[3] - last[3];
    ctx->D[1] = Yo[3 + oYstride] - Yo[3];
    ctx->D[2] = Yo[3 + oYstride * 2] - Yo[3 + oYstride];
    ctx->D[3] = Yo[3 + oYstride * 3] - Yo[3 + oYstride * 2];

    for (j = 0; j < 4; j++) {
        d = last[3];
        for (i = 0; i < 4; i++) {
            Y[i]    = Yo[i] + GET_TOK(ctx, TM2_UPD);
            last[i] = Y[i];
        }
        ctx->D[j] = last[3] - d;
        Y  += Ystride;
        Yo += oYstride;
    }
}

static inline void tm2_motion_block(TM2Context *ctx, AVFrame *pic, int bx, int by)
{
    int i, j;
    int mx, my;
    TM2_INIT_POINTERS_2();

    mx = GET_TOK(ctx, TM2_MOT);
    my = GET_TOK(ctx, TM2_MOT);
    mx = av_clip(mx, -(bx * 4 + 4), ctx->avctx->width  - bx * 4);
    my = av_clip(my, -(by * 4 + 4), ctx->avctx->height - by * 4);

    Yo += my * oYstride + mx;
    Uo += (my >> 1) * oUstride + (mx >> 1);
    Vo += (my >> 1) * oVstride + (mx >> 1);

    /* copy chroma */
    for (j = 0; j < 2; j++) {
        for (i = 0; i < 2; i++) {
            U[i] = Uo[i];
            V[i] = Vo[i];
        }
        U  += Ustride;
        V  += Vstride;
        Uo += oUstride;
        Vo += oVstride;
    }
    U -= Ustride * 2;
    V -= Vstride * 2;
    TM2_RECALC_BLOCK(U, Ustride, clast, ctx->CD);
    TM2_RECALC_BLOCK(V, Vstride, (clast + 2), (ctx->CD + 2));

    /* copy luma */
    for (j = 0; j < 4; j++) {
        for (i = 0; i < 4; i++) {
            Y[i] = Yo[i];
        }
        Y  += Ystride;
        Yo += oYstride;
    }
    /* calculate deltas */
    Y -= Ystride * 4;
    ctx->D[0] = Y[3] - last[3];
    ctx->D[1] = Y[3 + Ystride] - Y[3];
    ctx->D[2] = Y[3 + Ystride * 2] - Y[3 + Ystride];
    ctx->D[3] = Y[3 + Ystride * 3] - Y[3 + Ystride * 2];
    for (i = 0; i < 4; i++)
        last[i] = Y[i + Ystride * 3];
}

static int tm2_decode_blocks(TM2Context *ctx, AVFrame *p)
{
    int i, j;
    int w = ctx->avctx->width, h = ctx->avctx->height, bw = w >> 2, bh = h >> 2, cw = w >> 1;
    int type;
    int keyframe = 1;
    int *Y, *U, *V;
    uint8_t *dst;

    for (i = 0; i < TM2_NUM_STREAMS; i++)
        ctx->tok_ptrs[i] = 0;

    if (ctx->tok_lens[TM2_TYPE]<bw*bh) {
        av_log(ctx->avctx,AV_LOG_ERROR,"Got %i tokens for %i blocks\n",ctx->tok_lens[TM2_TYPE],bw*bh);
        return AVERROR_INVALIDDATA;
    }

    memset(ctx->last, 0, 4 * bw * sizeof(int));
    memset(ctx->clast, 0, 4 * bw * sizeof(int));

    for (j = 0; j < bh; j++) {
        memset(ctx->D, 0, 4 * sizeof(int));
        memset(ctx->CD, 0, 4 * sizeof(int));
        for (i = 0; i < bw; i++) {
            type = GET_TOK(ctx, TM2_TYPE);
            switch(type) {
            case TM2_HI_RES:
                tm2_hi_res_block(ctx, p, i, j);
                break;
            case TM2_MED_RES:
                tm2_med_res_block(ctx, p, i, j);
                break;
            case TM2_LOW_RES:
                tm2_low_res_block(ctx, p, i, j);
                break;
            case TM2_NULL_RES:
                tm2_null_res_block(ctx, p, i, j);
                break;
            case TM2_UPDATE:
                tm2_update_block(ctx, p, i, j);
                keyframe = 0;
                break;
            case TM2_STILL:
                tm2_still_block(ctx, p, i, j);
                keyframe = 0;
                break;
            case TM2_MOTION:
                tm2_motion_block(ctx, p, i, j);
                keyframe = 0;
                break;
            default:
                av_log(ctx->avctx, AV_LOG_ERROR, "Skipping unknown block type %i\n", type);
            }
        }
    }

    /* copy data from our buffer to AVFrame */
    Y = (ctx->cur?ctx->Y2:ctx->Y1);
    U = (ctx->cur?ctx->U2:ctx->U1);
    V = (ctx->cur?ctx->V2:ctx->V1);
    dst = p->data[0];
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            int y = Y[i], u = U[i >> 1], v = V[i >> 1];
            dst[3*i+0] = av_clip_uint8(y + v);
            dst[3*i+1] = av_clip_uint8(y);
            dst[3*i+2] = av_clip_uint8(y + u);
        }

        /* horizontal edge extension */
        Y[-4]    = Y[-3]    = Y[-2]    = Y[-1] = Y[0];
        Y[w + 3] = Y[w + 2] = Y[w + 1] = Y[w]  = Y[w - 1];

        /* vertical edge extension */
        if (j == 0) {
            memcpy(Y - 4 - 1 * ctx->y_stride, Y - 4, ctx->y_stride);
            memcpy(Y - 4 - 2 * ctx->y_stride, Y - 4, ctx->y_stride);
            memcpy(Y - 4 - 3 * ctx->y_stride, Y - 4, ctx->y_stride);
            memcpy(Y - 4 - 4 * ctx->y_stride, Y - 4, ctx->y_stride);
        } else if (j == h - 1) {
            memcpy(Y - 4 + 1 * ctx->y_stride, Y - 4, ctx->y_stride);
            memcpy(Y - 4 + 2 * ctx->y_stride, Y - 4, ctx->y_stride);
            memcpy(Y - 4 + 3 * ctx->y_stride, Y - 4, ctx->y_stride);
            memcpy(Y - 4 + 4 * ctx->y_stride, Y - 4, ctx->y_stride);
        }

        Y += ctx->y_stride;
        if (j & 1) {
            /* horizontal edge extension */
            U[-2]     = U[-1] = U[0];
            V[-2]     = V[-1] = V[0];
            U[cw + 1] = U[cw] = U[cw - 1];
            V[cw + 1] = V[cw] = V[cw - 1];

            /* vertical edge extension */
            if (j == 1) {
                memcpy(U - 2 - 1 * ctx->uv_stride, U - 2, ctx->uv_stride);
                memcpy(V - 2 - 1 * ctx->uv_stride, V - 2, ctx->uv_stride);
                memcpy(U - 2 - 2 * ctx->uv_stride, U - 2, ctx->uv_stride);
                memcpy(V - 2 - 2 * ctx->uv_stride, V - 2, ctx->uv_stride);
            } else if (j == h - 1) {
                memcpy(U - 2 + 1 * ctx->uv_stride, U - 2, ctx->uv_stride);
                memcpy(V - 2 + 1 * ctx->uv_stride, V - 2, ctx->uv_stride);
                memcpy(U - 2 + 2 * ctx->uv_stride, U - 2, ctx->uv_stride);
                memcpy(V - 2 + 2 * ctx->uv_stride, V - 2, ctx->uv_stride);
            }

            U += ctx->uv_stride;
            V += ctx->uv_stride;
        }
        dst += p->linesize[0];
    }

    return keyframe;
}

static const int tm2_stream_order[TM2_NUM_STREAMS] = {
    TM2_C_HI, TM2_C_LO, TM2_L_HI, TM2_L_LO, TM2_UPD, TM2_MOT, TM2_TYPE
};

#define TM2_HEADER_SIZE 40

static int decode_frame(AVCodecContext *avctx,
                        void *data, int *got_frame,
                        AVPacket *avpkt)
{
    TM2Context * const l = avctx->priv_data;
    const uint8_t *buf   = avpkt->data;
    int buf_size         = avpkt->size & ~3;
    AVFrame * const p    = &l->pic;
    int offset           = TM2_HEADER_SIZE;
    int i, t, ret;
    uint8_t *swbuf;

    swbuf = av_malloc(buf_size + FF_INPUT_BUFFER_PADDING_SIZE);
    if (!swbuf) {
        av_log(avctx, AV_LOG_ERROR, "Cannot allocate temporary buffer\n");
        return AVERROR(ENOMEM);
    }

    if ((ret = ff_reget_buffer(avctx, p)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
        av_free(swbuf);
        return ret;
    }

    l->dsp.bswap_buf((uint32_t*)swbuf, (const uint32_t*)buf, buf_size >> 2);

    if ((ret = tm2_read_header(l, swbuf)) < 0) {
        av_free(swbuf);
        return ret;
    }

    for (i = 0; i < TM2_NUM_STREAMS; i++) {
        if (offset >= buf_size) {
            av_free(swbuf);
            return AVERROR_INVALIDDATA;
        }
        t = tm2_read_stream(l, swbuf + offset, tm2_stream_order[i],
                            buf_size - offset);
        if (t < 0) {
            av_free(swbuf);
            return t;
        }
        offset += t;
    }
    p->key_frame = tm2_decode_blocks(l, p);
    if (p->key_frame)
        p->pict_type = AV_PICTURE_TYPE_I;
    else
        p->pict_type = AV_PICTURE_TYPE_P;

    l->cur = !l->cur;
    *got_frame      = 1;
    ret = av_frame_ref(data, &l->pic);
    av_free(swbuf);

    return (ret < 0) ? ret : buf_size;
}

static av_cold int decode_init(AVCodecContext *avctx)
{
    TM2Context * const l = avctx->priv_data;
    int i, w = avctx->width, h = avctx->height;

    if ((avctx->width & 3) || (avctx->height & 3)) {
        av_log(avctx, AV_LOG_ERROR, "Width and height must be multiple of 4\n");
        return AVERROR(EINVAL);
    }

    l->avctx       = avctx;
    avcodec_get_frame_defaults(&l->pic);
    avctx->pix_fmt = AV_PIX_FMT_BGR24;

    ff_dsputil_init(&l->dsp, avctx);

    l->last  = av_malloc(4 * sizeof(*l->last)  * (w >> 2));
    l->clast = av_malloc(4 * sizeof(*l->clast) * (w >> 2));

    for (i = 0; i < TM2_NUM_STREAMS; i++) {
        l->tokens[i] = NULL;
        l->tok_lens[i] = 0;
    }

    w += 8;
    h += 8;
    l->Y1_base = av_malloc(sizeof(*l->Y1_base) * w * h);
    l->Y2_base = av_malloc(sizeof(*l->Y2_base) * w * h);
    l->y_stride = w;
    w = (w + 1) >> 1;
    h = (h + 1) >> 1;
    l->U1_base = av_malloc(sizeof(*l->U1_base) * w * h);
    l->V1_base = av_malloc(sizeof(*l->V1_base) * w * h);
    l->U2_base = av_malloc(sizeof(*l->U2_base) * w * h);
    l->V2_base = av_malloc(sizeof(*l->V1_base) * w * h);
    l->uv_stride = w;
    l->cur = 0;
    if (!l->Y1_base || !l->Y2_base || !l->U1_base ||
        !l->V1_base || !l->U2_base || !l->V2_base ||
        !l->last    || !l->clast) {
        av_freep(l->Y1_base);
        av_freep(l->Y2_base);
        av_freep(l->U1_base);
        av_freep(l->U2_base);
        av_freep(l->V1_base);
        av_freep(l->V2_base);
        av_freep(l->last);
        av_freep(l->clast);
        return AVERROR(ENOMEM);
    }
    l->Y1 = l->Y1_base + l->y_stride  * 4 + 4;
    l->Y2 = l->Y2_base + l->y_stride  * 4 + 4;
    l->U1 = l->U1_base + l->uv_stride * 2 + 2;
    l->U2 = l->U2_base + l->uv_stride * 2 + 2;
    l->V1 = l->V1_base + l->uv_stride * 2 + 2;
    l->V2 = l->V2_base + l->uv_stride * 2 + 2;

    return 0;
}

static av_cold int decode_end(AVCodecContext *avctx)
{
    TM2Context * const l = avctx->priv_data;
    AVFrame *pic = &l->pic;
    int i;

    av_free(l->last);
    av_free(l->clast);
    for (i = 0; i < TM2_NUM_STREAMS; i++)
        av_free(l->tokens[i]);
    if (l->Y1) {
        av_free(l->Y1_base);
        av_free(l->U1_base);
        av_free(l->V1_base);
        av_free(l->Y2_base);
        av_free(l->U2_base);
        av_free(l->V2_base);
    }

    av_frame_unref(pic);

    return 0;
}

AVCodec ff_truemotion2_decoder = {
    .name           = "truemotion2",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_TRUEMOTION2,
    .priv_data_size = sizeof(TM2Context),
    .init           = decode_init,
    .close          = decode_end,
    .decode         = decode_frame,
    .capabilities   = CODEC_CAP_DR1,
    .long_name      = NULL_IF_CONFIG_SMALL("Duck TrueMotion 2.0"),
};
