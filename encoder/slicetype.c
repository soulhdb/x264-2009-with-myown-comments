/*****************************************************************************
 * slicetype.c: h264 encoder library
 *****************************************************************************
 * Copyright (C) 2005-2008 x264 project
 *
 * Authors: Loren Merritt <lorenm@u.washington.edu>
 *          Jason Garrett-Glaser <darkshikari@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *****************************************************************************/

#include <math.h>

#include "common/common.h"
#include "common/cpu.h"
#include "macroblock.h"
#include "me.h"


static void x264_lowres_context_init( x264_t *h, x264_mb_analysis_t *a )
{
    a->i_qp = X264_LOOKAHEAD_QP;
    a->i_lambda = x264_lambda_tab[ a->i_qp ];
    x264_mb_analyse_load_costs( h, a );
    h->mb.i_me_method = X264_MIN( X264_ME_HEX, h->param.analyse.i_me_method ); // maybe dia?
    h->mb.i_subpel_refine = 4; // 3 should be enough, but not tweaking for speed now
    h->mb.b_chroma_me = 0;
}

static int x264_slicetype_mb_cost( x264_t *h, x264_mb_analysis_t *a,
							x264_frame_t **frames, int p0/*前向索引*/, int p1/*后向索引*/, int b/*当前索引*/,
                            int dist_scale_factor, int do_search[2] )
{
    x264_frame_t *fref0 = frames[p0];
    x264_frame_t *fref1 = frames[p1];
    x264_frame_t *fenc  = frames[b];
    const int b_bidir = (b < p1);      // 是否进行后向参考, 也就是当前帧是否是B帧
    const int i_mb_x = h->mb.i_mb_x;   // 预测宏块的X坐标
    const int i_mb_y = h->mb.i_mb_y;   // 预测宏块的Y坐标
    const int i_mb_stride = h->sps->i_mb_width;         // 以宏块为单位的宽度
    const int i_mb_xy = i_mb_x + i_mb_y * i_mb_stride;  // 二维坐标转换到一维坐标
    const int i_stride = fenc->i_stride_lowres;         // 待编码sub-pixel平面的跨度
    const int i_pel_offset = 8 * ( i_mb_x + i_mb_y * i_stride );
    const int i_bipred_weight = h->param.analyse.b_weighted_bipred ? 64 - (dist_scale_factor>>2) : 32; // 双向预测权重
	// [0][b-p0-1][i_mb_xy]: 前向预测宏块的索引 [1][p1-b-1][i_mb_xy]: 后向预测宏块的索引
	// fenc_mvs: 前后参考帧位置i_mb_xy的宏块mv
    int16_t (*fenc_mvs[2])[2] = { &frames[b]->lowres_mvs[0][b-p0-1][i_mb_xy], &frames[b]->lowres_mvs[1][p1-b-1][i_mb_xy] };
    int (*fenc_costs[2]) = { &frames[b]->lowres_mv_costs[0][b-p0-1][i_mb_xy], &frames[b]->lowres_mv_costs[1][p1-b-1][i_mb_xy] };

    ALIGNED_8( uint8_t pix1[9*FDEC_STRIDE] );
    uint8_t *pix2 = pix1+8;
    x264_me_t m[2];
    int i_bcost = COST_MAX;
    int l, i;
    int list_used = 0;

    h->mb.pic.p_fenc[0] = h->mb.pic.fenc_buf;
    h->mc.copy[PIXEL_8x8]( h->mb.pic.p_fenc[0], FENC_STRIDE, &fenc->lowres[0][i_pel_offset], i_stride, 8 ); // 拷贝1/2像素Origin平面(8x8)

    if( p0 == p1 ) // 前向索引=后向索引, 则进行I帧处理
        goto lowres_intra_mb;

    // no need for h->mb.mv_min[]
	// 设置前向全像素预测范围
    h->mb.mv_min_fpel[0] = -8*h->mb.i_mb_x - 4;
    h->mb.mv_max_fpel[0] = 8*( h->sps->i_mb_width - h->mb.i_mb_x - 1 ) + 4;
	// 设置前向半像素预测范围
    h->mb.mv_min_spel[0] = 4*( h->mb.mv_min_fpel[0] - 8 );
    h->mb.mv_max_spel[0] = 4*( h->mb.mv_max_fpel[0] + 8 );
    if( h->mb.i_mb_x >= h->sps->i_mb_width - 2 ) // 如果当前宏块是倒数1,2列, 设置后向预测范围. 为什么倒数1,2行的时候不这么做?
    {
        h->mb.mv_min_fpel[1] = -8*h->mb.i_mb_y - 4;
        h->mb.mv_max_fpel[1] = 8*( h->sps->i_mb_height - h->mb.i_mb_y - 1 ) + 4;
        h->mb.mv_min_spel[1] = 4*( h->mb.mv_min_fpel[1] - 8 );
        h->mb.mv_max_spel[1] = 4*( h->mb.mv_max_fpel[1] + 8 );
    }

// 装载1/2像素各个平面(原, 水平, 垂直, 对角线)
#define LOAD_HPELS_LUMA(dst, src) \
    { \
        (dst)[0] = &(src)[0][i_pel_offset]; \
        (dst)[1] = &(src)[1][i_pel_offset]; \
        (dst)[2] = &(src)[2][i_pel_offset]; \
        (dst)[3] = &(src)[3][i_pel_offset]; \
    }

// mv不能超过搜索范围
#define CLIP_MV( mv ) \
    { \
        mv[0] = x264_clip3( mv[0], h->mb.mv_min_spel[0], h->mb.mv_max_spel[0] ); \
        mv[1] = x264_clip3( mv[1], h->mb.mv_min_spel[1], h->mb.mv_max_spel[1] ); \
    }

// NOTES>> 用亮度块代替整个宏块的复杂度
// src1/2: 分别以mv前/后向偏移得到的8x8块
// pix1: src1和src2的中值块
// h->pixf.mbcmp[PIXEL_8x8]: 计算8x8块的SATD值
#define TRY_BIDIR( mv0, mv1, penalty ) \
    { \
        int stride1 = 16, stride2 = 16; \
        uint8_t *src1, *src2; \
        int i_cost; \
        src1 = h->mc.get_ref( pix1, &stride1, m[0].p_fref, m[0].i_stride[0], \
                              (mv0)[0], (mv0)[1], 8, 8 ); /*前向预测出8x8宏块src1*/ \
        src2 = h->mc.get_ref( pix2, &stride2, m[1].p_fref, m[1].i_stride[0], \
                              (mv1)[0], (mv1)[1], 8, 8 ); /*后向预测出8x8宏块src2*/ \
        h->mc.avg[PIXEL_8x8]( pix1, 16, src1, stride1, src2, stride2, i_bipred_weight ); /*根据src1和src2中值计算出pix1*/ \
        i_cost = penalty + h->pixf.mbcmp[PIXEL_8x8]( \
                           m[0].p_fenc[0], FENC_STRIDE, pix1, 16 ); /*计算原像素和pix1的残差cost*/\
        COPY2_IF_LT( i_bcost, i_cost, list_used, 3 ); \
    }

    m[0].i_pixel = PIXEL_8x8;
    m[0].p_cost_mv = a->p_cost_mv;
    m[0].i_stride[0] = i_stride;
    m[0].p_fenc[0] = h->mb.pic.p_fenc[0];
    LOAD_HPELS_LUMA( m[0].p_fref, fref0->lowres ); // 装载{前向参考帧}的半像素平面

    if( b_bidir ) // 如果是B帧
    {
        int16_t *mvr = fref1->lowres_mvs[0][p1-p0-1][i_mb_xy]; // 后向参考帧 的 前向参考帧 对应i_mb_xy的宏块
        int dmv[2][2];

        h->mc.memcpy_aligned( &m[1], &m[0], sizeof(x264_me_t) );
        LOAD_HPELS_LUMA( m[1].p_fref, fref1->lowres ); // 装载{后向参考帧}的半像素平面

		// 根据dist_scale_factor计算出一组双向MV
        dmv[0][0] = ( mvr[0] * dist_scale_factor + 128 ) >> 8;
        dmv[0][1] = ( mvr[1] * dist_scale_factor + 128 ) >> 8;
        dmv[1][0] = dmv[0][0] - mvr[0];
        dmv[1][1] = dmv[0][1] - mvr[1];
        CLIP_MV( dmv[0] );
        CLIP_MV( dmv[1] );

        TRY_BIDIR( dmv[0], dmv[1], 0 );                     // 根据这组MV计算双向预测cost
        if( dmv[0][0] | dmv[0][1] | dmv[1][0] | dmv[1][1] ) // 如果mv都不为0, 以 前后向参考帧的第一个8x8宏块预测
        {
            int i_cost;
            h->mc.avg[PIXEL_8x8]( pix1, 16, m[0].p_fref[0], m[0].i_stride[0], m[1].p_fref[0], m[1].i_stride[0], i_bipred_weight );
            i_cost = h->pixf.mbcmp[PIXEL_8x8]( m[0].p_fenc[0], FENC_STRIDE, pix1, 16 );
            COPY2_IF_LT( i_bcost, i_cost, list_used, 3 );
        }
    }

    for( l = 0; l < 1 + b_bidir; l++ )
    {
        if( do_search[l] )
        {
            int i_mvc = 0;
            int16_t (*fenc_mv)[2] = fenc_mvs[l]; // 每个参考帧中每个宏块的MV
            ALIGNED_4( int16_t mvc[4][2] );

            /* Reverse-order MV prediction. */
            *(uint32_t*)mvc[0] = 0;
            *(uint32_t*)mvc[1] = 0;
            *(uint32_t*)mvc[2] = 0;
#define MVC(mv) { *(uint32_t*)mvc[i_mvc] = *(uint32_t*)mv; i_mvc++; }
            if( i_mb_x < h->sps->i_mb_width - 1 )
                MVC(fenc_mv[1]);
            if( i_mb_y < h->sps->i_mb_height - 1 )
            {
                MVC(fenc_mv[i_mb_stride]);
                if( i_mb_x > 0 )
                    MVC(fenc_mv[i_mb_stride-1]);
                if( i_mb_x < h->sps->i_mb_width - 1 )
                    MVC(fenc_mv[i_mb_stride+1]);
            }
#undef MVC
            x264_median_mv( m[l].mvp, mvc[0], mvc[1], mvc[2] ); // 中值预测MV
            x264_me_search( h, &m[l], mvc, i_mvc );             // ** 这里只计算最小SATD_cost **, 应该是为了rate_control

            m[l].cost -= 2; // remove mvcost from skip mbs
            if( *(uint32_t*)m[l].mv )
                m[l].cost += 5;
            *(uint32_t*)fenc_mvs[l] = *(uint32_t*)m[l].mv; // 更新预测后得到的MV
            *fenc_costs[l] = m[l].cost;
        }
        else // 不用预测了
        {
            *(uint32_t*)m[l].mv = *(uint32_t*)fenc_mvs[l]; // 直接使用参考帧的MV
            m[l].cost = *fenc_costs[l];                    // 直接使用参考帧的MV_cost
        }
        COPY2_IF_LT( i_bcost, m[l].cost, list_used, l+1 );
    }

    if( b_bidir && ( *(uint32_t*)m[0].mv || *(uint32_t*)m[1].mv ) )
        TRY_BIDIR( m[0].mv, m[1].mv, 5 );

    /* Store to width-2 bitfield. */
    frames[b]->lowres_inter_types[b-p0][p1-b][i_mb_xy>>2] &= ~(3<<((i_mb_xy&3)*2));
    frames[b]->lowres_inter_types[b-p0][p1-b][i_mb_xy>>2] |= list_used<<((i_mb_xy&3)*2);

lowres_intra_mb:
    /* forbid intra-mbs in B-frames, because it's rare and not worth checking */
    /* FIXME: Should we still forbid them now that we cache intra scores? */
    if( !b_bidir || h->param.rc.b_mb_tree ) // 帧内计算SATD
    {
        int i_icost, b_intra;
        if( !fenc->b_intra_calculated )
        {
            ALIGNED_ARRAY_16( uint8_t, edge,[33] );
            uint8_t *pix = &pix1[8+FDEC_STRIDE - 1];
            uint8_t *src = &fenc->lowres[0][i_pel_offset - 1];
            const int intra_penalty = 5;
            int satds[4];

            memcpy( pix-FDEC_STRIDE, src-i_stride, 17 );
            for( i=0; i<8; i++ )
                pix[i*FDEC_STRIDE] = src[i*i_stride];
            pix++;

            if( h->pixf.intra_mbcmp_x3_8x8c )
            {
                h->pixf.intra_mbcmp_x3_8x8c( h->mb.pic.p_fenc[0], pix, satds );
                h->predict_8x8c[I_PRED_CHROMA_P]( pix );
                satds[I_PRED_CHROMA_P] =
                    h->pixf.mbcmp[PIXEL_8x8]( pix, FDEC_STRIDE, h->mb.pic.p_fenc[0], FENC_STRIDE );
            }
            else
            {
                for( i=0; i<4; i++ )
                {
                    h->predict_8x8c[i]( pix );
                    satds[i] = h->pixf.mbcmp[PIXEL_8x8]( pix, FDEC_STRIDE, h->mb.pic.p_fenc[0], FENC_STRIDE );
                }
            }
            i_icost = X264_MIN4( satds[0], satds[1], satds[2], satds[3] );

            h->predict_8x8_filter( pix, edge, ALL_NEIGHBORS, ALL_NEIGHBORS );
            for( i=3; i<9; i++ )
            {
                int satd;
                h->predict_8x8[i]( pix, edge );
                satd = h->pixf.mbcmp[PIXEL_8x8]( pix, FDEC_STRIDE, h->mb.pic.p_fenc[0], FENC_STRIDE );
                i_icost = X264_MIN( i_icost, satd );
            }

            i_icost += intra_penalty;
            fenc->i_intra_cost[i_mb_xy] = i_icost;
        }
        else
            i_icost = fenc->i_intra_cost[i_mb_xy];
        if( !b_bidir )
        {
            b_intra = i_icost < i_bcost;
            if( b_intra )
                i_bcost = i_icost;
            if(   (i_mb_x > 0 && i_mb_x < h->sps->i_mb_width - 1
                && i_mb_y > 0 && i_mb_y < h->sps->i_mb_height - 1)
                || h->sps->i_mb_width <= 2 || h->sps->i_mb_height <= 2 )
            {
                fenc->i_intra_mbs[b-p0] += b_intra;
                fenc->i_cost_est[0][0] += i_icost;
                if( h->param.rc.i_aq_mode )
                    fenc->i_cost_est_aq[0][0] += (i_icost * fenc->i_inv_qscale_factor[i_mb_xy] + 128) >> 8;
            }
        }
    }

    fenc->lowres_costs[b-p0][p1-b][i_mb_xy] = i_bcost; // i_mb_xy位置的宏块, 前向参考p0, 后向参考p1时的cost值

    return i_bcost;
}
#undef TRY_BIDIR

#define NUM_MBS\
   (h->sps->i_mb_width > 2 && h->sps->i_mb_height > 2 ?\
   (h->sps->i_mb_width - 2) * (h->sps->i_mb_height - 2) :\
    h->sps->i_mb_width * h->sps->i_mb_height)

// 预估此帧的复杂度(SSD其实是最佳选择, 但是需要等到重建后才能计算, 显然太慢了)。
// 这里计算的是SATD值，用于代替略微复杂的SSD。
// 估计完此帧的复杂度之后，最终转换成此帧的QP值。(当然必须是非CQP流控模式)
static int x264_slicetype_frame_cost( x264_t *h, x264_mb_analysis_t *a,
                               x264_frame_t **frames, int p0, int p1, int b,
                               int b_intra_penalty )
{

    int i_score = 0;
    /* Don't use the AQ'd scores for slicetype decision. */
    int i_score_aq = 0;
    int do_search[2];

    /* Check whether we already evaluated this frame
     * If we have tried this frame as P, then we have also tried
     * the preceding frames as B. (is this still true?) */
    /* Also check that we already calculated the row SATDs for the current frame. */
    if( frames[b]->i_cost_est[b-p0][p1-b] >= 0 && (!h->param.rc.i_vbv_buffer_size || frames[b]->i_row_satds[b-p0][p1-b][0] != -1) )
    {
        i_score = frames[b]->i_cost_est[b-p0][p1-b];
    }
    else
    {
        int dist_scale_factor = 128;
        int *row_satd = frames[b]->i_row_satds[b-p0][p1-b];

        /* For each list, check to see whether we have lowres motion-searched this reference frame before. */
        do_search[0] = b != p0 && frames[b]->lowres_mvs[0][b-p0-1][0][0] == 0x7FFF;
        do_search[1] = b != p1 && frames[b]->lowres_mvs[1][p1-b-1][0][0] == 0x7FFF;
        if( do_search[0] ) frames[b]->lowres_mvs[0][b-p0-1][0][0] = 0;
        if( do_search[1] ) frames[b]->lowres_mvs[1][p1-b-1][0][0] = 0;

        if( b == p1 )
        {
            frames[b]->i_intra_mbs[b-p0] = 0;
            frames[b]->i_cost_est[0][0] = 0;
            frames[b]->i_cost_est_aq[0][0] = 0;
        }
        if( p1 != p0 )
            dist_scale_factor = ( ((b-p0) << 8) + ((p1-p0) >> 1) ) / (p1-p0);

        /* Lowres lookahead goes backwards because the MVs are used as predictors in the main encode.
         * This considerably improves MV prediction overall. */

        /* the edge mbs seem to reduce the predictive quality of the
         * whole frame's score, but are needed for a spatial distribution. */
        if( h->param.rc.b_mb_tree || h->param.rc.i_vbv_buffer_size ||
            h->sps->i_mb_width <= 2 || h->sps->i_mb_height <= 2 )
        {
            for( h->mb.i_mb_y = h->sps->i_mb_height - 1; h->mb.i_mb_y >= 0; h->mb.i_mb_y-- )
            {
                row_satd[ h->mb.i_mb_y ] = 0; // 行的satd
                for( h->mb.i_mb_x = h->sps->i_mb_width - 1; h->mb.i_mb_x >= 0; h->mb.i_mb_x-- )
                {
                    int i_mb_cost = x264_slicetype_mb_cost( h, a, frames, p0, p1, b, dist_scale_factor, do_search );
                    int i_mb_cost_aq = i_mb_cost;
                    if( h->param.rc.i_aq_mode )
                        i_mb_cost_aq = (i_mb_cost_aq * frames[b]->i_inv_qscale_factor[h->mb.i_mb_x + h->mb.i_mb_y*h->mb.i_mb_stride] + 128) >> 8;
                    row_satd[ h->mb.i_mb_y ] += i_mb_cost_aq; // 累加一行的satd
                    if( (h->mb.i_mb_y > 0 && h->mb.i_mb_y < h->sps->i_mb_height - 1 &&
                         h->mb.i_mb_x > 0 && h->mb.i_mb_x < h->sps->i_mb_width - 1) ||
                         h->sps->i_mb_width <= 2 || h->sps->i_mb_height <= 2 )
                    {
                        /* Don't use AQ-weighted costs for slicetype decision, only for ratecontrol. */
                        i_score += i_mb_cost;       // 帧的satd
                        i_score_aq += i_mb_cost_aq; // 帧的aq
                    }
                }
            }
        }
        else
        {
            for( h->mb.i_mb_y = h->sps->i_mb_height - 2; h->mb.i_mb_y > 0; h->mb.i_mb_y-- )
                for( h->mb.i_mb_x = h->sps->i_mb_width - 2; h->mb.i_mb_x > 0; h->mb.i_mb_x-- )
                {
                    int i_mb_cost = x264_slicetype_mb_cost( h, a, frames, p0, p1, b, dist_scale_factor, do_search );
                    int i_mb_cost_aq = i_mb_cost;
                    if( h->param.rc.i_aq_mode )
                        i_mb_cost_aq = (i_mb_cost_aq * frames[b]->i_inv_qscale_factor[h->mb.i_mb_x + h->mb.i_mb_y*h->mb.i_mb_stride] + 128) >> 8;
                    i_score += i_mb_cost;
                    i_score_aq += i_mb_cost_aq;
                }
        }

        if( b != p1 )
            i_score = i_score * 100 / (120 + h->param.i_bframe_bias);
        else
            frames[b]->b_intra_calculated = 1;

        frames[b]->i_cost_est[b-p0][p1-b] = i_score;       // SATD
        frames[b]->i_cost_est_aq[b-p0][p1-b] = i_score_aq; // AQ
        x264_emms();
    }

    if( b_intra_penalty )
    {
        // arbitrary penalty for I-blocks after B-frames
        int nmb = NUM_MBS;
        i_score += i_score * frames[b]->i_intra_mbs[b-p0] / (nmb * 8);
    }
    return i_score;
}

/* If MB-tree changes the quantizers, we need to recalculate the frame cost without
 * re-running lookahead. */
static int x264_slicetype_frame_cost_recalculate( x264_t *h, x264_frame_t **frames, int p0, int p1, int b )
{
    int i_score = 0;
    int *row_satd = frames[b]->i_row_satds[b-p0][p1-b];
    x264_emms();
    for( h->mb.i_mb_y = h->sps->i_mb_height - 1; h->mb.i_mb_y >= 0; h->mb.i_mb_y-- )
    {
        row_satd[ h->mb.i_mb_y ] = 0;
        for( h->mb.i_mb_x = h->sps->i_mb_width - 1; h->mb.i_mb_x >= 0; h->mb.i_mb_x-- )
        {
            int i_mb_xy = h->mb.i_mb_x + h->mb.i_mb_y*h->mb.i_mb_stride;
            int i_mb_cost = frames[b]->lowres_costs[b-p0][p1-b][i_mb_xy];
            float qp_adj = frames[b]->f_qp_offset[i_mb_xy];
            i_mb_cost = (i_mb_cost * x264_exp2fix8(qp_adj) + 128) >> 8;
            row_satd[ h->mb.i_mb_y ] += i_mb_cost;
            if( (h->mb.i_mb_y > 0 && h->mb.i_mb_y < h->sps->i_mb_height - 1 &&
                 h->mb.i_mb_x > 0 && h->mb.i_mb_x < h->sps->i_mb_width - 1) ||
                 h->sps->i_mb_width <= 2 || h->sps->i_mb_height <= 2 )
            {
                i_score += i_mb_cost;
            }
        }
    }
    return i_score;
}

static void x264_macroblock_tree_finish( x264_t *h, x264_frame_t *frame, int b_bidir )
{
    int mb_index;
    x264_emms();
    if( b_bidir )
        memcpy( frame->f_qp_offset, frame->f_qp_offset_aq, sizeof( frame->f_qp_offset ) );
    else
    {
        /* Allow the strength to be adjusted via qcompress, since the two
         * concepts are very similar. */
        float strength = 5.0f * (1.0f - h->param.rc.f_qcompress);
        for( mb_index = 0; mb_index < h->mb.i_mb_count; mb_index++ )
        {
            int intra_cost = (frame->i_intra_cost[mb_index] * frame->i_inv_qscale_factor[mb_index]+128)>>8;
            if( intra_cost )
            {
                int propagate_cost = frame->i_propagate_cost[mb_index];
                float log2_ratio = x264_log2(intra_cost + propagate_cost) - x264_log2(intra_cost);
                frame->f_qp_offset[mb_index] = frame->f_qp_offset_aq[mb_index] - strength * log2_ratio;
            }
        }
    }
}

static void x264_macroblock_tree_propagate( x264_t *h, x264_frame_t **frames, int p0, int p1, int b )
{
    uint16_t *ref_costs[2] = {frames[p0]->i_propagate_cost,frames[p1]->i_propagate_cost};
    int dist_scale_factor = ( ((b-p0) << 8) + ((p1-p0) >> 1) ) / (p1-p0);
    int i_bipred_weight = h->param.analyse.b_weighted_bipred ? 64 - (dist_scale_factor>>2) : 32;
    int16_t (*mvs[2])[2] = { frames[b]->lowres_mvs[0][b-p0-1], frames[b]->lowres_mvs[1][p1-b-1] };
    int bipred_weights[2] = {i_bipred_weight, 64 - i_bipred_weight};
    int *buf = (int *)h->scratch_buffer;
	int idx0weight, idx1weight, idx2weight, idx3weight;

    for( h->mb.i_mb_y = 0; h->mb.i_mb_y < h->sps->i_mb_height; h->mb.i_mb_y++ )
    {
        int mb_index = h->mb.i_mb_y*h->mb.i_mb_stride;
        h->mc.mbtree_propagate_cost( buf, frames[b]->i_propagate_cost+mb_index,
            frames[b]->i_intra_cost+mb_index, frames[b]->lowres_costs[b-p0][p1-b]+mb_index,
            frames[b]->i_inv_qscale_factor+mb_index, h->sps->i_mb_width );
        for( h->mb.i_mb_x = 0; h->mb.i_mb_x < h->sps->i_mb_width; h->mb.i_mb_x++, mb_index++ )
        {
            int propagate_amount = buf[h->mb.i_mb_x];
            /* Don't propagate for an intra block. */
            if( propagate_amount > 0 )
            {
                /* Access width-2 bitfield. */
                int lists_used = (frames[b]->lowres_inter_types[b-p0][p1-b][mb_index>>2] >> ((mb_index&3)*2))&3;
                int list;
                /* Follow the MVs to the previous frame(s). */
                for( list = 0; list < 2; list++ )
                    if( (lists_used >> list)&1 )
                    {
                        int x = mvs[list][mb_index][0];
                        int y = mvs[list][mb_index][1];
                        int listamount = propagate_amount;
                        int mbx = (x>>5)+h->mb.i_mb_x;
                        int mby = (y>>5)+h->mb.i_mb_y;
                        int idx0 = mbx + mby*h->mb.i_mb_stride;
                        int idx1 = idx0 + 1;
                        int idx2 = idx0 + h->mb.i_mb_stride;
                        int idx3 = idx0 + h->mb.i_mb_stride + 1;
                        x &= 31;
                        y &= 31;
                        idx0weight = (32-y)*(32-x);
                        idx1weight = (32-y)*x;
                        idx2weight = y*(32-x);
                        idx3weight = y*x;

                        /* Apply bipred weighting. */
                        if( lists_used == 3 )
                            listamount = (listamount * bipred_weights[list] + 32) >> 6;

#define CLIP_ADD(s,x) (s) = X264_MIN((s)+(x),(1<<16)-1)

                        /* We could just clip the MVs, but pixels that lie outside the frame probably shouldn't
                         * be counted. */
                        if( mbx < h->sps->i_mb_width-1 && mby < h->sps->i_mb_height-1 && mbx >= 0 && mby >= 0 )
                        {
                            CLIP_ADD( ref_costs[list][idx0], (listamount*idx0weight+512)>>10 );
                            CLIP_ADD( ref_costs[list][idx1], (listamount*idx1weight+512)>>10 );
                            CLIP_ADD( ref_costs[list][idx2], (listamount*idx2weight+512)>>10 );
                            CLIP_ADD( ref_costs[list][idx3], (listamount*idx3weight+512)>>10 );
                        }
                        else /* Check offsets individually */
                        {
                            if( mbx < h->sps->i_mb_width && mby < h->sps->i_mb_height && mbx >= 0 && mby >= 0 )
                                CLIP_ADD( ref_costs[list][idx0], (listamount*idx0weight+512)>>10 );
                            if( mbx+1 < h->sps->i_mb_width && mby < h->sps->i_mb_height && mbx+1 >= 0 && mby >= 0 )
                                CLIP_ADD( ref_costs[list][idx1], (listamount*idx1weight+512)>>10 );
                            if( mbx < h->sps->i_mb_width && mby+1 < h->sps->i_mb_height && mbx >= 0 && mby+1 >= 0 )
                                CLIP_ADD( ref_costs[list][idx2], (listamount*idx2weight+512)>>10 );
                            if( mbx+1 < h->sps->i_mb_width && mby+1 < h->sps->i_mb_height && mbx+1 >= 0 && mby+1 >= 0 )
                                CLIP_ADD( ref_costs[list][idx3], (listamount*idx3weight+512)>>10 );
                        }
                    }
            }
        }
    }

    if( h->param.rc.i_vbv_buffer_size )
        x264_macroblock_tree_finish( h, frames[b], b != p1 );
}

static void x264_macroblock_tree( x264_t *h, x264_mb_analysis_t *a, x264_frame_t **frames, int num_frames, int b_intra )
{
    int i, idx = !b_intra;
    int last_nonb, cur_nonb = 1;
    if( b_intra )
        x264_slicetype_frame_cost( h, a, frames, 0, 0, 0, 0 );

    i = num_frames-1;
    while( i > 0 && frames[i]->i_type == X264_TYPE_B )
        i--;
    last_nonb = i;

    if( last_nonb < 0 )
        return;

    memset( frames[last_nonb]->i_propagate_cost, 0, h->mb.i_mb_count * sizeof(uint16_t) );
    while( i-- > idx )
    {
        cur_nonb = i;
        while( frames[cur_nonb]->i_type == X264_TYPE_B && cur_nonb > 0 )
            cur_nonb--;
        if( cur_nonb < idx )
            break;
        x264_slicetype_frame_cost( h, a, frames, cur_nonb, last_nonb, last_nonb, 0 );
        memset( frames[cur_nonb]->i_propagate_cost, 0, h->mb.i_mb_count * sizeof(uint16_t) );
        x264_macroblock_tree_propagate( h, frames, cur_nonb, last_nonb, last_nonb );
        while( frames[i]->i_type == X264_TYPE_B && i > 0 )
        {
            x264_slicetype_frame_cost( h, a, frames, cur_nonb, last_nonb, i, 0 );
            memset( frames[i]->i_propagate_cost, 0, h->mb.i_mb_count * sizeof(uint16_t) );
            x264_macroblock_tree_propagate( h, frames, cur_nonb, last_nonb, i );
            i--;
        }
        last_nonb = cur_nonb;
    }

    x264_macroblock_tree_finish( h, frames[last_nonb], 0 );
}

static int x264_vbv_frame_cost( x264_t *h, x264_mb_analysis_t *a, x264_frame_t **frames, int p0, int p1, int b )
{
    int cost = x264_slicetype_frame_cost( h, a, frames, p0, p1, b, 0 );
    if( h->param.rc.i_aq_mode )
    {
        if( h->param.rc.b_mb_tree )
            return x264_slicetype_frame_cost_recalculate( h, frames, p0, p1, b );
        else
            return frames[b]->i_cost_est_aq[b-p0][p1-b];
    }
    return cost;
}

static void x264_vbv_lookahead( x264_t *h, x264_mb_analysis_t *a, x264_frame_t **frames, int num_frames, int keyframe )
{
    int last_nonb = 0, cur_nonb = 1, next_nonb, i, idx = 0;
    while( cur_nonb < num_frames && frames[cur_nonb]->i_type == X264_TYPE_B )
        cur_nonb++;
    next_nonb = keyframe ? last_nonb : cur_nonb;

    while( cur_nonb <= num_frames )
    {
        /* P/I cost: This shouldn't include the cost of next_nonb */
        if( next_nonb != cur_nonb )
        {
            int p0 = IS_X264_TYPE_I( frames[cur_nonb]->i_type ) ? cur_nonb : last_nonb;
            frames[next_nonb]->i_planned_satd[idx] = x264_vbv_frame_cost( h, a, frames, p0, cur_nonb, cur_nonb );
            frames[next_nonb]->i_planned_type[idx] = frames[cur_nonb]->i_type;
            idx++;
        }
        /* Handle the B-frames: coded order */
        for( i = last_nonb+1; i < cur_nonb; i++, idx++ )
        {
            frames[next_nonb]->i_planned_satd[idx] = x264_vbv_frame_cost( h, a, frames, last_nonb, cur_nonb, i );
            frames[next_nonb]->i_planned_type[idx] = X264_TYPE_B;
        }
        last_nonb = cur_nonb;
        cur_nonb++;
        while( cur_nonb <= num_frames && frames[cur_nonb]->i_type == X264_TYPE_B )
            cur_nonb++;
    }
    frames[next_nonb]->i_planned_type[idx] = X264_TYPE_AUTO;
}

static int x264_slicetype_path_cost( x264_t *h, x264_mb_analysis_t *a, x264_frame_t **frames, char *path, int threshold )
{
    int loc = 1;
    int cost = 0;
    int cur_p = 0;
    path--; /* Since the 1st path element is really the second frame */
    while( path[loc] )
    {
        int next_p = loc;
        int next_b;
        /* Find the location of the next P-frame. */
        while( path[next_p] && path[next_p] != 'P' )
            next_p++;
        /* Return if the path doesn't end on a P-frame. */
        if( path[next_p] != 'P' )
            return cost;

        /* Add the cost of the P-frame found above */
        cost += x264_slicetype_frame_cost( h, a, frames, cur_p, next_p, next_p, 0 );
        /* Early terminate if the cost we have found is larger than the best path cost so far */
        if( cost > threshold )
            break;

        for( next_b = loc; next_b < next_p && cost < threshold; next_b++ )
            cost += x264_slicetype_frame_cost( h, a, frames, cur_p, next_p, next_b, 0 );

        loc = next_p + 1;
        cur_p = next_p;
    }
    return cost;
}

/* Viterbi/trellis slicetype decision algorithm. */
/* Uses strings due to the fact that the speed of the control functions is
   negligable compared to the cost of running slicetype_frame_cost, and because
   it makes debugging easier. */

// char best_paths[N][N]: (char *)best_paths[n]表示当总帧数=n时的最佳类型决策
static void x264_slicetype_path( x264_t *h, x264_mb_analysis_t *a, x264_frame_t **frames, int length, int max_bframes, char (*best_paths)[X264_LOOKAHEAD_MAX] )
{
    char paths[X264_BFRAME_MAX+2][X264_LOOKAHEAD_MAX] = {{0}};
    int num_paths = X264_MIN(max_bframes+1, length); // 需要决策帧类型的总帧数 (最多不能超过B帧数目的限制)
    int suffix_size, loc, path;
    int best_cost = COST_MAX;
    int best_path_index = 0;
    length = X264_MIN(length,X264_LOOKAHEAD_MAX);    // 整个frames的总帧数

	// 只重新决策best_paths的最后num_paths个路径. 因为前面的已经不能再调整了, 已是最优.
    for( suffix_size = 0; suffix_size < num_paths; suffix_size++ )
    {
        memcpy( paths[suffix_size], best_paths[length - (suffix_size + 1)], length - (suffix_size + 1) ); // 每次决策一个帧的类型时, 会重新计算所有可能的最佳路径
        for( loc = 0; loc < suffix_size; loc++ )
            strcat( paths[suffix_size], "B" ); // 尽量填充B帧直到最多可容忍的值(i_max_bframes)
        strcat( paths[suffix_size], "P" );     // 每种决策最后帧一定是个P帧
    }

    /* Calculate the actual cost of each of the current paths */
    for( path = 0; path < num_paths; path++ )
    {
        int cost = x264_slicetype_path_cost( h, a, frames, paths[path], best_cost );
        if( cost < best_cost )
        {
            best_cost = cost;
            best_path_index = path; // 计算每种决策的SATD_cost. 取最佳的!
        }
    }

    /* Store the best path. */
    memcpy( best_paths[length], paths[best_path_index], length ); // 总帧数为length时的最佳决策
}

// 当前帧是否不适合帧间预测(SATD太大意味着不适合帧间预测, 而应该产生一个I帧/IDR帧)
static int scenecut( x264_t *h, x264_mb_analysis_t *a, x264_frame_t **frames, int p0, int p1, int print )
{
	float f_bias;
	int icost, pcost;
	int i_gop_size, res;
	float f_thresh_max, f_thresh_min;

    x264_frame_t *frame = frames[p1];
    x264_slicetype_frame_cost( h, a, frames, p0, p1, p1, 0 ); // 对带编码帧作"前向帧间SATD计算" 和 "帧内SATD计算"

    icost = frame->i_cost_est[0][0];     // 当前帧SATD_cost
    pcost = frame->i_cost_est[p1-p0][0]; // 前向参考帧的SATD_cost
    
    i_gop_size = frame->i_frame - h->lookahead->i_last_idr;
    f_thresh_max = h->param.i_scenecut_threshold / 100.0;
    /* magic numbers pulled out of thin air */
    f_thresh_min = f_thresh_max * h->param.i_keyint_min
                         / ( h->param.i_keyint_max * 4 );
    

    if( h->param.i_keyint_min == h->param.i_keyint_max )
        f_thresh_min= f_thresh_max;
    if( i_gop_size < h->param.i_keyint_min / 4 )
        f_bias = f_thresh_min / 4;
    else if( i_gop_size <= h->param.i_keyint_min )
        f_bias = f_thresh_min * i_gop_size / h->param.i_keyint_min;
    else
    {
        f_bias = f_thresh_min
                 + ( f_thresh_max - f_thresh_min )
                    * ( i_gop_size - h->param.i_keyint_min )
                   / ( h->param.i_keyint_max - h->param.i_keyint_min ) ;
    }

    res = pcost >= (1.0 - f_bias) * icost;
    if( res && print )
    {
        int imb = frame->i_intra_mbs[p1-p0];
        int pmb = NUM_MBS - imb;
        x264_log( h, X264_LOG_DEBUG, "scene cut at %d Icost:%d Pcost:%d ratio:%.4f bias:%.4f gop:%d (imb:%d pmb:%d)\n",
                  frame->i_frame,
                  icost, pcost, 1. - (double)pcost / icost,
                  f_bias, i_gop_size, imb, pmb );
    }
    return res;
}

void x264_slicetype_analyse( x264_t *h, int keyframe )
{
	int n, num_bframes, max_bframes, num_analysed_frames, reset_start;
	char best_paths[X264_LOOKAHEAD_MAX][X264_LOOKAHEAD_MAX] = {"","P"};
    x264_mb_analysis_t a;
    x264_frame_t *frames[X264_LOOKAHEAD_MAX+3] = { NULL, };
    int num_frames, keyint_limit, idr_frame_type, i, j;
    int i_mb_count = NUM_MBS;
    int cost1p0, cost2p0, cost1b1, cost2p1;
    int i_max_search = X264_MIN( h->lookahead->next.i_size, X264_LOOKAHEAD_MAX );
    if( h->param.b_deterministic )
        i_max_search = X264_MIN( i_max_search, h->lookahead->i_slicetype_length + !keyframe );

    assert( h->frames.b_have_lowres );

    if( !h->lookahead->last_nonb )
        return;
    frames[0] = h->lookahead->last_nonb;
    for( j = 0; j < i_max_search && h->lookahead->next.list[j]->i_type == X264_TYPE_AUTO; j++ )
        frames[j+1] = h->lookahead->next.list[j]; // frames[0]是nonb帧, 其他是待编码帧

    if( !j )
        return;

    keyint_limit = h->param.i_keyint_max - frames[0]->i_frame + h->lookahead->i_last_idr - 1; // 除了frames[0]最多可容纳多少帧
    num_frames = X264_MIN( j, keyint_limit ); // 总帧数

    x264_lowres_context_init( h, &a );
    idr_frame_type = frames[1]->i_frame - h->lookahead->i_last_idr >= h->param.i_keyint_min ? X264_TYPE_IDR : X264_TYPE_I; // 是IDR还是I帧

    /* This is important psy-wise: if we have a non-scenecut keyframe,
     * there will be significant visual artifacts if the frames just before
     * go down in quality due to being referenced less, despite it being
     * more RD-optimal. */
    if( (h->param.analyse.b_psy && h->param.rc.b_mb_tree) || h->param.rc.i_vbv_buffer_size )
        num_frames = j;
    else if( num_frames == 1 )
    {
        frames[1]->i_type = X264_TYPE_P;
        if( h->param.i_scenecut_threshold && scenecut( h, &a, frames, 0, 1, 1 ) )
            frames[1]->i_type = idr_frame_type;
        return;
    }
    else if( num_frames == 0 )
    {
        frames[1]->i_type = idr_frame_type;
        return;
    }

    num_bframes = 0;
    max_bframes = X264_MIN(num_frames-1, h->param.i_bframe);
    num_analysed_frames = num_frames;

    if( h->param.i_scenecut_threshold && scenecut( h, &a, frames, 0, 1, 1 ) ) // frames[1]以frames[0]为参考帧作P预测。是否要产生一个I帧(场景变化太大)
    {
        frames[1]->i_type = idr_frame_type;
        return;
    }

    if( h->param.i_bframe ) // 判断可否有B帧
    {
        if( h->param.i_bframe_adaptive == X264_B_ADAPT_TRELLIS ) //TRELLIS决策算法(较X264_B_ADAPT_FAST要慢)
        {
            /* Perform the frametype analysis. */
            for( n = 2; n < num_frames-1; n++ )
                x264_slicetype_path( h, &a, frames, n, max_bframes, best_paths );
            if( num_frames > 1 )
            {
                num_bframes = strspn( best_paths[num_frames-2], "B" ); // 检查有多少个B帧
                /* Load the results of the analysis into the frame types. */
                for( j = 1; j < num_frames; j++ )
                    frames[j]->i_type = best_paths[num_frames-2][j-1] == 'B' ? X264_TYPE_B : X264_TYPE_P;
            }
            frames[num_frames]->i_type = X264_TYPE_P; // 最后一个一定是P帧
        }
        else if( h->param.i_bframe_adaptive == X264_B_ADAPT_FAST )
        {
			// 确定frames[1-N]的类型(P or B)
            for( i = 0; i < num_frames-(2-!i); )
            {
				// [FX*XX] -> [FXFXX]
                cost2p1 = x264_slicetype_frame_cost( h, &a, frames, i+0, i+2, i+2, 1 ); // 当前P帧[i+2] 前-2 后+0
                if( frames[i+2]->i_intra_mbs[2] > i_mb_count / 2 ) // 当前P帧[i+2]以帧[i]预测后,产生的帧內宏块数量太多(场景变化太大)
                {
                    frames[i+1]->i_type = X264_TYPE_P;
                    frames[i+2]->i_type = X264_TYPE_P;
                    i += 2;
                    continue;
                }

				// [F*FXX] -> [FFFXX]
                cost1b1 = x264_slicetype_frame_cost( h, &a, frames, i+0, i+2, i+1, 0 );  // 当前B帧[i+1] 前-1 后+1
                cost1p0 = x264_slicetype_frame_cost( h, &a, frames, i+0, i+1, i+1, 0 );  // 当前P帧[i+1] 前-1 后+0
                cost2p0 = x264_slicetype_frame_cost( h, &a, frames, i+1, i+2, i+2, 0 );  // 当前P帧[i+2] 前-1 后+0

                if( cost1p0 + cost2p0 < cost1b1 + cost2p1 ) // 连续两帧a1,a2: 都用P预测的sum_cost < a1用B预测+a2用P预测的sum_cost
                {
					// 用P预测更佳
                    frames[i+1]->i_type = X264_TYPE_P;
                    frames[i+2]->i_type = X264_TYPE_P;
                    i += 2;
                    continue;
                }

                // arbitrary and untuned
                #define INTER_THRESH 300
                #define P_SENS_BIAS (50 - h->param.i_bframe_bias)
                frames[i+1]->i_type = X264_TYPE_B;
                frames[i+2]->i_type = X264_TYPE_P;

                for( j = i+2; j <= X264_MIN( h->param.i_bframe, num_frames-1 ); j++ )
                {
                    int pthresh = X264_MAX(INTER_THRESH - P_SENS_BIAS * (j-i-1), INTER_THRESH/10);
                    int pcost = x264_slicetype_frame_cost( h, &a, frames, i+0, j+1, j+1, 1 ); // 以lastnonb帧为参考帧, 对frames[i+2]后面的一帧计算SATD

                    if( pcost > pthresh*i_mb_count || frames[j+1]->i_intra_mbs[j-i+1] > i_mb_count/3 )
                    {
                        frames[j]->i_type = X264_TYPE_P;
                        break;
                    }
                    else
                        frames[j]->i_type = X264_TYPE_B;
                }
                i = j;
            }
            frames[i+!i]->i_type = X264_TYPE_P; // 如果有B帧, 那么最后一帧必须是P帧
            num_bframes = 0;
            while( num_bframes < num_frames && frames[num_bframes+1]->i_type == X264_TYPE_B )
                num_bframes++; // Number of b-frames.
        }
        else
        {
            num_bframes = X264_MIN(num_frames-1, h->param.i_bframe);
            for( j = 1; j < num_frames; j++ )
                frames[j]->i_type = (j%(num_bframes+1)) ? X264_TYPE_B : X264_TYPE_P;
            frames[num_frames]->i_type = X264_TYPE_P;
        }

        /* Check scenecut on the first minigop. */
        for( j = 1; j < num_bframes+1; j++ )
            if( h->param.i_scenecut_threshold && scenecut( h, &a, frames, j, j+1, 0 ) ) // 看看要不要场景切换
            {
                frames[j]->i_type = X264_TYPE_P; // B切换成P
                num_analysed_frames = j;
                break;
            }

        reset_start = keyframe ? 1 : X264_MIN( num_bframes+2, num_analysed_frames+1 ); // reset_start之后的帧重置为X264_TYPE_AUTO
    }
    else
    {
        for( j = 1; j < num_frames; j++ )
            frames[j]->i_type = X264_TYPE_P;
        reset_start = !keyframe + 1;
        num_bframes = 0;
    }

    for( j = 1; j <= num_frames; j++ )
        if( frames[j]->i_type == X264_TYPE_AUTO )
            frames[j]->i_type = X264_TYPE_P;

    /* Perform the actual macroblock tree analysis.
     * Don't go farther than the maximum keyframe interval; this helps in short GOPs. */
    if( h->param.rc.b_mb_tree )
        x264_macroblock_tree( h, &a, frames, X264_MIN(num_frames, h->param.i_keyint_max), keyframe );

    /* Enforce keyframe limit. */
    for( j = 0; j < num_frames; j++ )
    {
        if( ((j-keyint_limit) % h->param.i_keyint_max) == 0 )
        {
            if( j && h->param.i_keyint_max > 1 )
                frames[j]->i_type = X264_TYPE_P;
            frames[j+1]->i_type = X264_TYPE_IDR;
            reset_start = X264_MIN( reset_start, j+2 );
        }
    }

    if( h->param.rc.i_vbv_buffer_size )
        x264_vbv_lookahead( h, &a, frames, num_frames, keyframe );

    /* Restore frametypes for all frames that haven't actually been decided yet. */
    for( j = reset_start; j <= num_frames; j++ )
        frames[j]->i_type = X264_TYPE_AUTO;
}

void x264_slicetype_decide( x264_t *h )
{
    x264_frame_t *frm;
    int bframes;
    int i;

    if( !h->lookahead->next.i_size )
        return;

    if( h->param.rc.b_stat_read )
    {
        /* Use the frame types from the first pass */
        for( i = 0; i < h->lookahead->next.i_size; i++ )
            h->lookahead->next.list[i]->i_type =
                x264_ratecontrol_slice_type( h, h->lookahead->next.list[i]->i_frame );
    }
    else if( (h->param.i_bframe && h->param.i_bframe_adaptive)
             || h->param.i_scenecut_threshold
             || h->param.rc.b_mb_tree
             || (h->param.rc.i_vbv_buffer_size && h->param.rc.i_lookahead) )
        x264_slicetype_analyse( h, 0 );

    for( bframes = 0;; bframes++ )
    {
        frm = h->lookahead->next.list[bframes];

        /* Limit GOP size */
        if( frm->i_frame - h->lookahead->i_last_idr >= h->param.i_keyint_max ) // 如果达到GOP产生IDR帧的条件
        {
            if( frm->i_type == X264_TYPE_AUTO )
                frm->i_type = X264_TYPE_IDR;
            if( frm->i_type != X264_TYPE_IDR )
                x264_log( h, X264_LOG_WARNING, "specified frame type (%d) is not compatible with keyframe interval\n", frm->i_type );
        }
        if( frm->i_type == X264_TYPE_IDR )
        {
            /* Close GOP */
            h->lookahead->i_last_idr = frm->i_frame;
            if( bframes > 0 )
            {
                bframes--;
                h->lookahead->next.list[bframes]->i_type = X264_TYPE_P;
            }
        }

        if( bframes == h->param.i_bframe ||
            !h->lookahead->next.list[bframes+1] )
        {
            if( IS_X264_TYPE_B( frm->i_type ) )
                x264_log( h, X264_LOG_WARNING, "specified frame type is not compatible with max B-frames\n" );
            if( frm->i_type == X264_TYPE_AUTO
                || IS_X264_TYPE_B( frm->i_type ) )
                frm->i_type = X264_TYPE_P;
        }

        if( frm->i_type == X264_TYPE_AUTO )
            frm->i_type = X264_TYPE_B;

        else if( !IS_X264_TYPE_B( frm->i_type ) ) break;
    }

    if( bframes )
        h->lookahead->next.list[bframes-1]->b_last_minigop_bframe = 1;
    h->lookahead->next.list[bframes]->i_bframes = bframes; // 此帧前面有多少个B帧

    /* calculate the frame costs ahead of time for x264_rc_analyse_slice while we still have lowres */
	/* 在x264_rc_analyse_slice之前计算cost */
    if( h->param.rc.i_rc_method != X264_RC_CQP )
    {
        x264_mb_analysis_t a;
        x264_frame_t *frames[X264_BFRAME_MAX+2] = { NULL, };
        int p0=0, p1, b;

        x264_lowres_context_init( h, &a );

        if( IS_X264_TYPE_I( h->lookahead->next.list[bframes]->i_type ) )
            p1 = b = 0;           // 如果next链表里最后一个是I帧
        else
            p1 = b = bframes + 1; // 如果next链表里最后一个是P帧
        frames[p0] = h->lookahead->last_nonb;          // 前一个已编码帧
        frames[b] = h->lookahead->next.list[bframes];  // 当前待编码帧

		// if I帧 then frames里只有当前的I帧
		// if P帧 then frames里[nonb-frame][nul]...[p-frame]
        x264_slicetype_frame_cost( h, &a, frames, p0, p1, b, 0 ); // 如果没有计算当前帧的cost, 那么计算它
    }
}

/* 计算当前帧的复杂度SATD */
int x264_rc_analyse_slice( x264_t *h )
{
    x264_frame_t *frames[X264_BFRAME_MAX+2] = { NULL, };
    int p0=0, p1, b;
    int cost;

    if( IS_X264_TYPE_I(h->fenc->i_type) ) // 如果当前帧是I帧
        p1 = b = 0;
    else // P
        p1 = b = h->fenc->i_bframes + 1;
    frames[p0] = h->fref0[0];
    frames[b] = h->fenc;

    /* cost should have been already calculated by x264_slicetype_decide */
    cost = frames[b]->i_cost_est[b-p0][p1-b]; // 当前编码帧的satd_cost
    assert( cost >= 0 );

    if( h->param.rc.b_mb_tree && !h->param.rc.b_stat_read )
        cost = x264_slicetype_frame_cost_recalculate( h, frames, p0, p1, b );
    /* In AQ, use the weighted score instead. */
    else if( h->param.rc.i_aq_mode )
        cost = frames[b]->i_cost_est_aq[b-p0][p1-b];

    h->fenc->i_row_satd = h->fenc->i_row_satds[b-p0][p1-b];
    h->fdec->i_row_satd = h->fdec->i_row_satds[b-p0][p1-b];
    h->fdec->i_satd = cost;
    memcpy( h->fdec->i_row_satd, h->fenc->i_row_satd, h->sps->i_mb_height * sizeof(int) );
    return cost;
}
