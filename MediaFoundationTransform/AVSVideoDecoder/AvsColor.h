//----------------------------------------------------------------------------------------------
// AvsColor.h - the colour signalling of an AVS / FFmpeg bit stream turned into the
// media type attributes a Media Foundation graph expects.
//
// Where the information comes from:
//   * the container (our media source reads AVCodecParameters and publishes the
//     attributes on the compressed type - that is how a Matroska HDR AVS3 file works);
//   * the decoder (the AVS3 sequence header carries it and the FFmpeg wrapper puts it
//     on every decoded frame).
// The decoder wins when both speak, because it is closer to the picture.
//
// Values are taken from the SDK headers of this machine (mfobjects.h), not from memory.
// Two deliberate gaps:
//   * MF_MT_YUV_MATRIX is not set: the MFVideoYCbCrMatrix / MFVideoMatrixCoeff value
//     set this attribute takes is not declared anywhere in these headers, and a wrong
//     matrix mis-transforms every pixel, which is worse than leaving it to the
//     renderer (a renderer that is given BT.2020 primaries and a PQ transfer picks the
//     matrix itself).
//   * Nothing is set for "unspecified".  Inventing BT.709 for a stream that says
//     nothing is the classic way to make an SD AVS broadcast look wrong.
//----------------------------------------------------------------------------------------------
#ifndef AVSCOLOR_H
#define AVSCOLOR_H

extern "C" {
#include <libavutil/pixfmt.h>
}

// The colour description in FFmpeg's vocabulary.  Everything UNSPECIFIED means the
// stream does not say.
struct AvsColorInfo
{
    AvsColorInfo()
        : primaries(AVCOL_PRI_UNSPECIFIED), transfer(AVCOL_TRC_UNSPECIFIED)
        , matrix(AVCOL_SPC_UNSPECIFIED), range(AVCOL_RANGE_UNSPECIFIED) {}

    int primaries;      // AVCOL_PRI_*
    int transfer;       // AVCOL_TRC_*
    int matrix;         // AVCOL_SPC_*
    int range;          // AVCOL_RANGE_*

    bool IsEmpty() const
    {
        return primaries == AVCOL_PRI_UNSPECIFIED && transfer == AVCOL_TRC_UNSPECIFIED &&
               matrix == AVCOL_SPC_UNSPECIFIED && range == AVCOL_RANGE_UNSPECIFIED;
    }

    // take everything the other side states, keep what we already have
    void Merge(const AvsColorInfo& o)
    {
        if (o.primaries != AVCOL_PRI_UNSPECIFIED)   primaries = o.primaries;
        if (o.transfer  != AVCOL_TRC_UNSPECIFIED)   transfer  = o.transfer;
        if (o.matrix    != AVCOL_SPC_UNSPECIFIED)   matrix    = o.matrix;
        if (o.range     != AVCOL_RANGE_UNSPECIFIED) range     = o.range;
    }
};

// AVCOL_PRI_* -> MFVideoPrimaries.  FALSE = do not set the attribute.
inline BOOL AvsPrimariesToMF(int ff, UINT32* pOut)
{
    switch (ff)
    {
    case AVCOL_PRI_BT709:      *pOut = MFVideoPrimaries_BT709;        return TRUE;
    case AVCOL_PRI_BT470M:     *pOut = MFVideoPrimaries_BT470_2_SysM; return TRUE;
    case AVCOL_PRI_BT470BG:    *pOut = MFVideoPrimaries_BT470_2_SysBG; return TRUE;
    case AVCOL_PRI_SMPTE170M:  *pOut = MFVideoPrimaries_SMPTE170M;    return TRUE;
    case AVCOL_PRI_SMPTE240M:  *pOut = MFVideoPrimaries_SMPTE240M;    return TRUE;
    case AVCOL_PRI_BT2020:     *pOut = MFVideoPrimaries_BT2020;       return TRUE;
    case AVCOL_PRI_SMPTE428:   *pOut = MFVideoPrimaries_XYZ;          return TRUE;
    case AVCOL_PRI_SMPTE431:   *pOut = MFVideoPrimaries_DCI_P3;       return TRUE;
    case AVCOL_PRI_SMPTE432:   *pOut = MFVideoPrimaries_Display_P3;   return TRUE;
    case AVCOL_PRI_EBU3213:    *pOut = MFVideoPrimaries_EBU3213;      return TRUE;
    default:                                          return FALSE;
    }
}

// AVCOL_TRC_* -> MFVideoTransferFunction.  MF has no separate value for SMPTE 170M and
// H.273 defines TRC 1 and TRC 6 as the same curve, so both land on MFVideoTransFunc_709.
inline BOOL AvsTransferToMF(int ff, UINT32* pOut)
{
    switch (ff)
    {
    case AVCOL_TRC_BT709:        *pOut = MFVideoTransFunc_709;       return TRUE;
    case AVCOL_TRC_SMPTE170M:    *pOut = MFVideoTransFunc_709;       return TRUE;
    case AVCOL_TRC_SMPTE240M:    *pOut = MFVideoTransFunc_240M;      return TRUE;
    case AVCOL_TRC_GAMMA22:      *pOut = MFVideoTransFunc_22;        return TRUE;
    case AVCOL_TRC_GAMMA28:      *pOut = MFVideoTransFunc_28;        return TRUE;
    case AVCOL_TRC_LINEAR:       *pOut = MFVideoTransFunc_10;        return TRUE;
    case AVCOL_TRC_IEC61966_2_4: *pOut = MFVideoTransFunc_28;        return TRUE;
    case AVCOL_TRC_IEC61966_2_1: *pOut = MFVideoTransFunc_sRGB;      return TRUE;
    case AVCOL_TRC_BT1361_ECG:   *pOut = MFVideoTransFunc_BT1361_ECG; return TRUE;
    case AVCOL_TRC_LOG:          *pOut = MFVideoTransFunc_Log_100;   return TRUE;
    case AVCOL_TRC_LOG_SQRT:     *pOut = MFVideoTransFunc_Log_316;   return TRUE;
    case AVCOL_TRC_BT2020_10:    *pOut = MFVideoTransFunc_2020;      return TRUE;
    case AVCOL_TRC_BT2020_12:    *pOut = MFVideoTransFunc_2020;      return TRUE;
    case AVCOL_TRC_SMPTE2084:    *pOut = MFVideoTransFunc_2084;      return TRUE;
    case AVCOL_TRC_SMPTE428:     *pOut = MFVideoTransFunc_SMPTE428;  return TRUE;
    case AVCOL_TRC_ARIB_STD_B67: *pOut = MFVideoTransFunc_HLG;       return TRUE;
    default:                                          return FALSE;
    }
}

// AVCOL_RANGE_* -> MFNominalRange ("video range" = 16..235, "full range" = 0..255)
inline BOOL AvsRangeToMF(int ff, UINT32* pOut)
{
    switch (ff)
    {
    case AVCOL_RANGE_MPEG:  *pOut = MFNominalRange_16_235; return TRUE;
    case AVCOL_RANGE_JPEG:  *pOut = MFNominalRange_0_255;  return TRUE;
    default:                return FALSE;
    }
}

// Put everything that is known onto a media type.  Attributes that are not known are
// left out; setting MFVideoPrimaries_Unknown would be an explicit "unknown", which
// some renderers honour by falling back to their own defaults anyway, but it also
// stops the pipeline from carrying an inherited value forward.
inline void AvsApplyColor(IMFMediaType* pType, const AvsColorInfo& c)
{
    if (pType == NULL)
        return;

    UINT32 v = 0;
    if (AvsPrimariesToMF(c.primaries, &v))
        pType->SetUINT32(MF_MT_VIDEO_PRIMARIES, v);
    if (AvsTransferToMF(c.transfer, &v))
        pType->SetUINT32(MF_MT_TRANSFER_FUNCTION, v);
    if (AvsRangeToMF(c.range, &v))
        pType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, v);
}

// Read the same attributes back out of a media type (used by the decoder MFT to
// inherit what the media source published on the compressed type).
inline void AvsReadColor(IMFMediaType* pType, AvsColorInfo* pOut)
{
    if (pType == NULL || pOut == NULL)
        return;

    UINT32 v = 0;
    if (SUCCEEDED(pType->GetUINT32(MF_MT_VIDEO_PRIMARIES, &v)))
    {
        switch (v)
        {
        case MFVideoPrimaries_BT709:        pOut->primaries = AVCOL_PRI_BT709;    break;
        case MFVideoPrimaries_BT470_2_SysM: pOut->primaries = AVCOL_PRI_BT470M;   break;
        case MFVideoPrimaries_BT470_2_SysBG:pOut->primaries = AVCOL_PRI_BT470BG;  break;
        case MFVideoPrimaries_SMPTE170M:    pOut->primaries = AVCOL_PRI_SMPTE170M;break;
        case MFVideoPrimaries_SMPTE240M:    pOut->primaries = AVCOL_PRI_SMPTE240M;break;
        case MFVideoPrimaries_BT2020:       pOut->primaries = AVCOL_PRI_BT2020;   break;
        default: break;
        }
    }
    if (SUCCEEDED(pType->GetUINT32(MF_MT_TRANSFER_FUNCTION, &v)))
    {
        switch (v)
        {
        case MFVideoTransFunc_709:   pOut->transfer = AVCOL_TRC_BT709;    break;
        case MFVideoTransFunc_22:    pOut->transfer = AVCOL_TRC_GAMMA22;  break;
        case MFVideoTransFunc_28:    pOut->transfer = AVCOL_TRC_GAMMA28;  break;
        case MFVideoTransFunc_240M:  pOut->transfer = AVCOL_TRC_SMPTE240M;break;
        case MFVideoTransFunc_2084:  pOut->transfer = AVCOL_TRC_SMPTE2084;break;
        case MFVideoTransFunc_HLG:   pOut->transfer = AVCOL_TRC_ARIB_STD_B67; break;
        default: break;
        }
    }
    if (SUCCEEDED(pType->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &v)))
    {
        if (v == MFNominalRange_16_235) pOut->range = AVCOL_RANGE_MPEG;
        else if (v == MFNominalRange_0_255) pOut->range = AVCOL_RANGE_JPEG;
    }
}

#endif // AVSCOLOR_H
