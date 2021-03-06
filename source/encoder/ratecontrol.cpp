/*****************************************************************************
 * Copyright (C) 2013 x265 project
 *
 * Authors: Sumalatha Polureddy <sumalatha@multicorewareinc.com>
 *          Aarthi Priya Thirumalai <aarthi@multicorewareinc.com>
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
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@multicorewareinc.com.
 *****************************************************************************/

#include "TLibCommon/TComPic.h"
#include "slicetype.h"
#include "ratecontrol.h"
#include "TLibEncoder/TEncCfg.h"
#include <math.h>

using namespace x265;

#define BASE_FRAME_DURATION 0.04

/* Arbitrary limitations as a sanity check. */
#define MAX_FRAME_DURATION 1.00
#define MIN_FRAME_DURATION 0.01

#define CLIP_DURATION(f) Clip3(MIN_FRAME_DURATION, MAX_FRAME_DURATION, f)

/* The qscale - qp conversion is specified in the standards.
 * Approx qscale increases by 12%  with every qp increment */
static inline double qScale2qp(double qScale)
{
    return 12.0 + 6.0 * (double)X265_LOG2(qScale / 0.85);
}

static inline double qp2qScale(double qp)
{
    return 0.85 * pow(2.0, (qp - 12.0) / 6.0);
}

/* Compute variance to derive AC energy of each block */
static inline uint32_t acEnergyVar(TComPic *pic, uint64_t sum_ssd, int shift, int i)
{
    uint32_t sum = (uint32_t)sum_ssd;
    uint32_t ssd = (uint32_t)(sum_ssd >> 32);

    pic->m_lowres.wp_sum[i] += sum;
    pic->m_lowres.wp_ssd[i] += ssd;
    return ssd - ((uint64_t)sum * sum >> shift);
}

/* Find the energy of each block in Y/Cb/Cr plane */
static inline uint32_t acEnergyPlane(TComPic *pic, pixel* src, int srcStride, int bChroma)
{
    if (bChroma)
    {
        ALIGN_VAR_8(pixel, pix[8 * 8]);
        primitives.luma_copy_pp[LUMA_8x8](pix, 8, src, srcStride);
        return acEnergyVar(pic, primitives.var[LUMA_8x8](pix, 8), 6, bChroma);
    }
    else
        return acEnergyVar(pic, primitives.var[LUMA_16x16](src, srcStride), 8, bChroma);
}

/* Find the total AC energy of each block in all planes */
double RateControl::acEnergyCu(TComPic* pic, uint32_t block_x, uint32_t block_y)
{
    int stride = pic->getPicYuvOrg()->getStride();
    int cStride = pic->getPicYuvOrg()->getCStride();
    uint32_t blockOffsetLuma = block_x + (block_y * stride);
    uint32_t blockOffsetChroma = (block_x >> 1) + ((block_y >> 1) * cStride);

    uint32_t var;

    var  = acEnergyPlane(pic, pic->getPicYuvOrg()->getLumaAddr() + blockOffsetLuma, stride, 0);
    var += acEnergyPlane(pic, pic->getPicYuvOrg()->getCbAddr() + blockOffsetChroma, cStride, 1);
    var += acEnergyPlane(pic, pic->getPicYuvOrg()->getCrAddr() + blockOffsetChroma, cStride, 2);
    x265_emms();
    double strength = cfg->param.rc.aqStrength * 1.0397f;
    return strength * (X265_LOG2(X265_MAX(var, 1)) - 14.427f);
}

void RateControl::calcAdaptiveQuantFrame(TComPic *pic)
{
    /* Actual adaptive quantization */
    int maxCol = pic->getPicYuvOrg()->getWidth();
    int maxRow = pic->getPicYuvOrg()->getHeight();
    for (int y = 0; y < 3; y++ )
    {
        pic->m_lowres.wp_ssd[y] = 0;
        pic->m_lowres.wp_sum[y] = 0;
    }

    /* Calculate Qp offset for each 16x16 block in the frame */
    int block_xy = 0;
    int block_x = 0, block_y = 0;

    for (block_y = 0; block_y < maxRow; block_y += 16)
    {
        for (block_x = 0; block_x < maxCol; block_x += 16)
        {
            double qp_adj = acEnergyCu(pic, block_x, block_y);
            if (cfg->param.rc.aqMode)
            {
                pic->m_lowres.qpAqOffset[block_xy] = qp_adj;
                pic->m_lowres.invQscaleFactor[block_xy] = x265_exp2fix8(qp_adj);
                block_xy++;
            }
        }
    }

    if (cfg->param.bEnableWeightedPred)
    {
        for (int i = 0; i < 3; i++)
        {
            UInt64 sum, ssd;
            sum = pic->m_lowres.wp_sum[i];
            ssd = pic->m_lowres.wp_ssd[i];
            pic->m_lowres.wp_ssd[i] = ssd - (sum * sum + (block_x * block_y) / 2) / (block_x * block_y);
        }
    }
}

RateControl::RateControl(TEncCfg * _cfg)
{
    this->cfg = _cfg;
    ncu = (int)((cfg->param.sourceHeight * cfg->param.sourceWidth) / pow((int)16, 2.0));

    // validate for cfg->param.rc, maybe it is need to add a function like x265_parameters_valiate()
    cfg->param.rc.rfConstant = Clip3((double)-QP_BD_OFFSET, (double)51, cfg->param.rc.rfConstant);
    if (cfg->param.rc.rateControlMode == X265_RC_CRF)
    {
        cfg->param.rc.qp = (int)cfg->param.rc.rfConstant + QP_BD_OFFSET;
        cfg->param.rc.bitrate = 0;

        double baseCplx = ncu * (cfg->param.bframes ? 120 : 80);
        double mbtree_offset = 0; //added later
        rateFactorConstant = pow(baseCplx, 1 - cfg->param.rc.qCompress) /
            qp2qScale(cfg->param.rc.rfConstant + mbtree_offset + QP_BD_OFFSET);
    }

    isAbr = cfg->param.rc.rateControlMode != X265_RC_CQP; // later add 2pass option

    bitrate = cfg->param.rc.bitrate * 1000;
    frameDuration = 1.0 / cfg->param.frameRate;
    lastNonBPictType = -1;
    baseQp = cfg->param.rc.qp;
    qp = baseQp;
    lastRceq = 1; /* handles the cmplxrsum when the previous frame cost is zero */
    totalBits = 0;
    shortTermCplxSum = 0;
    shortTermCplxCount = 0;
    framesDone = 0;
    lastNonBPictType = I_SLICE;

    if (cfg->param.rc.rateControlMode == X265_RC_ABR)
    {
        /* Adjust the first frame in order to stabilize the quality level compared to the rest */
#define ABR_INIT_QP_MIN (24 + QP_BD_OFFSET)
#define ABR_INIT_QP_MAX (34 + QP_BD_OFFSET)
        accumPNorm = .01;
        accumPQp = (ABR_INIT_QP_MIN)*accumPNorm;
        /* estimated ratio that produces a reasonable QP for the first I-frame */
        cplxrSum = .01 * pow(7.0e5, cfg->param.rc.qCompress) * pow(ncu, 0.5);
        wantedBitsWindow = bitrate * frameDuration;
    }
    else if (cfg->param.rc.rateControlMode == X265_RC_CRF)
    {
#define ABR_INIT_QP ((int)cfg->param.rc.rfConstant + QP_BD_OFFSET)
        accumPNorm = .01;
        accumPQp = ABR_INIT_QP * accumPNorm;
        /* estimated ratio that produces a reasonable QP for the first I-frame */
        cplxrSum = .01 * pow(7.0e5, cfg->param.rc.qCompress) * pow(ncu, 0.5);
        wantedBitsWindow = bitrate * frameDuration;
    }

    ipOffset = 6.0 * X265_LOG2(cfg->param.rc.ipFactor);
    pbOffset = 6.0 * X265_LOG2(cfg->param.rc.pbFactor);
    for (int i = 0; i < 3; i++)
    {
        lastQScaleFor[i] = qp2qScale(ABR_INIT_QP_MIN);
        lmin[i] = qp2qScale(MIN_QP);
        lmax[i] = qp2qScale(MAX_QP);
    }

    if (cfg->param.rc.rateControlMode == X265_RC_CQP)
    {
        qpConstant[P_SLICE] = baseQp;
        qpConstant[I_SLICE] = Clip3(0, MAX_QP, (int)(baseQp - ipOffset + 0.5));
        qpConstant[B_SLICE] = Clip3(0, MAX_QP, (int)(baseQp + pbOffset + 0.5));
    }

    /* qstep - value set as encoder specific */
    lstep = pow(2, cfg->param.rc.qpStep / 6.0);
}

void RateControl::rateControlStart(TComPic* pic, Lookahead *l, RateControlEntry* rce)
{
    curSlice = pic->getSlice();
    sliceType = curSlice->getSliceType();
    rce->sliceType = sliceType;

    if (isAbr) //ABR,CRF
    {
        lastSatd = l->getEstimatedPictureCost(pic);
        double q = qScale2qp(rateEstimateQscale(rce));
        qp = Clip3(MIN_QP, MAX_QP, (int)(q + 0.5));
        rce->qpaRc = q;
        /* copy value of lastRceq into thread local rce struct *to be used in RateControlEnd() */
        rce->qRceq = lastRceq;
        accumPQpUpdate();
    }
    else //CQP
    {
        qp = qpConstant[sliceType];
    }

    if (sliceType != B_SLICE)
        lastNonBPictType = sliceType;
    framesDone++;
    /* set the final QP to slice structure */
    curSlice->setSliceQp(qp);
    curSlice->setSliceQpBase(qp);
}

void RateControl::accumPQpUpdate()
{
    accumPQp   *= .95;
    accumPNorm *= .95;
    accumPNorm += 1;
    if (sliceType == I_SLICE)
        accumPQp += qp + ipOffset;
    else
        accumPQp += qp;
}

double RateControl::rateEstimateQscale(RateControlEntry *rce)
{
    double q;

    if (sliceType == B_SLICE)
    {
        /* B-frames don't have independent rate control, but rather get the
         * average QP of the two adjacent P-frames + an offset */
        TComSlice* prevRefSlice = curSlice->getRefPic(REF_PIC_LIST_0, 0)->getSlice();
        TComSlice* nextRefSlice = curSlice->getRefPic(REF_PIC_LIST_1, 0)->getSlice();
        bool i0 = prevRefSlice->getSliceType() == I_SLICE;
        bool i1 = nextRefSlice->getSliceType() == I_SLICE;
        int dt0 = abs(curSlice->getPOC() - prevRefSlice->getPOC());
        int dt1 = abs(curSlice->getPOC() - nextRefSlice->getPOC());
        double q0 = prevRefSlice->getSliceQp();
        double q1 = nextRefSlice->getSliceQp();

        if (prevRefSlice->getSliceType() == B_SLICE && prevRefSlice->isReferenced())
            q0 -= pbOffset / 2;
        if (nextRefSlice->getSliceType() == B_SLICE && nextRefSlice->isReferenced())
            q1 -= pbOffset / 2;
        if (i0 && i1)
            q = (q0 + q1) / 2 + ipOffset;
        else if (i0)
            q = q1;
        else if (i1)
            q = q0;
        else
            q = (q0 * dt1 + q1 * dt0) / (dt0 + dt1);

        if (curSlice->isReferenced())
            q += pbOffset / 2;
        else
            q += pbOffset;

        return qp2qScale(q);
    }
    else
    {
        double abrBuffer = 2 * cfg->param.rc.rateTolerance * bitrate;

        /* 1pass ABR */

        /* Calculate the quantizer which would have produced the desired
         * average bitrate if it had been applied to all frames so far.
         * Then modulate that quant based on the current frame's complexity
         * relative to the average complexity so far (using the 2pass RCEQ).
         * Then bias the quant up or down if total size so far was far from
         * the target.
         * Result: Depending on the value of rate_tolerance, there is a
         * tradeoff between quality and bitrate precision. But at large
         * tolerances, the bit distribution approaches that of 2pass. */

        double wantedBits, overflow = 1;
        shortTermCplxSum *= 0.5;
        shortTermCplxCount *= 0.5;
        shortTermCplxSum += lastSatd / (CLIP_DURATION(frameDuration) / BASE_FRAME_DURATION);
        shortTermCplxCount++;
        rce->texBits = lastSatd;
        rce->blurredComplexity = shortTermCplxSum / shortTermCplxCount;
        rce->mvBits = 0;
        rce->sliceType = sliceType;

        if (cfg->param.rc.rateControlMode == X265_RC_CRF)
        {
            q = getQScale(rce, rateFactorConstant);
        }
        else
        {
            q = getQScale(rce, wantedBitsWindow / cplxrSum);

            /* ABR code can potentially be counterproductive in CBR, so just don't bother.
             * Don't run it if the frame complexity is zero either. */
            if (lastSatd)
            {
                /* use framesDone instead of POC as poc count is not serial with bframes enabled */
                double timeDone = (double)(framesDone - cfg->param.frameNumThreads + 1) / cfg->param.frameRate;
                wantedBits = timeDone * bitrate;
                if (wantedBits > 0 && totalBits > 0)
                {
                    abrBuffer *= X265_MAX(1, sqrt(timeDone));
                    overflow = Clip3(.5, 2.0, 1.0 + (totalBits - wantedBits) / abrBuffer);
                    q *= overflow;
                }
            }
        }

        if (sliceType == I_SLICE && cfg->param.keyframeMax > 1
            && lastNonBPictType != I_SLICE)
        {
            q = qp2qScale(accumPQp / accumPNorm);
            q /= fabs(cfg->param.rc.ipFactor);
        }
        else if (framesDone > 0)
        {
            if (cfg->param.rc.rateControlMode != X265_RC_CRF)
            {
                double lqmin = 0, lqmax = 0;
                if (totalBits == 0)
                {
                    lqmin = qp2qScale(ABR_INIT_QP_MIN) / lstep;
                    lqmax = qp2qScale(ABR_INIT_QP_MAX) * lstep;
                }
                else
                {
                    lqmin = lastQScaleFor[sliceType] / lstep;
                    lqmax = lastQScaleFor[sliceType] * lstep;
                }

                if (overflow > 1.1 && framesDone > 3)
                    lqmax *= lstep;
                else if (overflow < 0.9)
                    lqmin /= lstep;

                q = Clip3(lqmin, lqmax, q);
            }
        }

        double lmin1 = lmin[sliceType];
        double lmax1 = lmax[sliceType];
        q = Clip3(lmin1, lmax1, q);
        lastQScaleFor[sliceType] = q;

        if (curSlice->getPOC() == 0)
            lastQScaleFor[P_SLICE] = q * fabs(cfg->param.rc.ipFactor);

        return q;
    }
}

/* modify the bitrate curve from pass1 for one frame */
double RateControl::getQScale(RateControlEntry *rce, double rateFactor)
{
    double q;

    q = pow(rce->blurredComplexity, 1 - cfg->param.rc.qCompress);

    // avoid NaN's in the rc_eq
    if (rce->texBits + rce->mvBits == 0)
        q = lastQScaleFor[rce->sliceType];
    else
    {
        lastRceq = q;
        q /= rateFactor;
    }
    return q;
}

/* After encoding one frame, update rate control state */
int RateControl::rateControlEnd(int64_t bits, RateControlEntry* rce)
{
    if (isAbr)
    {
        if (rce->sliceType != B_SLICE)
            /* The factor 1.5 is to tune up the actual bits, otherwise the cplxrSum is scaled too low
             * to improve short term compensation for next frame. */
            cplxrSum += bits * qp2qScale(rce->qpaRc) / rce->qRceq;
        else
        {
            /* Depends on the fact that B-frame's QP is an offset from the following P-frame's.
             * Not perfectly accurate with B-refs, but good enough. */
            cplxrSum += bits * qp2qScale(rce->qpaRc) / (rce->qRceq * fabs(cfg->param.rc.pbFactor));
        }
        wantedBitsWindow += frameDuration * bitrate;
        rce = NULL;
    }
    totalBits += bits;
    return 0;
}
