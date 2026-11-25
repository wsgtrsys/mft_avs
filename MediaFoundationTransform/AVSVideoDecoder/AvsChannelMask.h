//----------------------------------------------------------------------------------------------
// AvsChannelMask.h - FFmpeg channel arrangement -> KSAUDIO_CHANNEL_MASK, the value that
// MF_MT_AUDIO_CHANNEL_MASK expects.
//
// Both halves of the pipeline need it:
//   * CAVSMediaSource::BuildAudioType() takes it from the demuxer's codec parameters, so
//     the stream descriptor already names the arrangement;
//   * CAVSAudioDecoderMFT takes it from what the DRA bit stream itself says, for the
//     (usual) case where the container only gives a channel count.
//
// Getting this right matters: without a mask the audio renderer has to guess, and a
// 5.1 DRA stream can end up with the centre and the LFE swapped into the side speakers.
//----------------------------------------------------------------------------------------------
#ifndef AVSCHANNELMASK_H
#define AVSCHANNELMASK_H

extern "C" {
#include <libavutil/channel_layout.h>
}

// The SPEAKER_* constants come from ksmedia.h, which none of the Media Foundation
// headers used here pulls in, so they are spelled out.  They are fixed by the audio
// document, not something the SDK changes.
#ifndef SPEAKER_FRONT_LEFT
#define SPEAKER_FRONT_LEFT              0x1
#define SPEAKER_FRONT_RIGHT             0x2
#define SPEAKER_FRONT_CENTER            0x4
#define SPEAKER_LOW_FREQUENCY           0x8
#define SPEAKER_BACK_LEFT               0x10
#define SPEAKER_BACK_RIGHT              0x20
#define SPEAKER_FRONT_LEFT_OF_CENTER    0x40
#define SPEAKER_FRONT_RIGHT_OF_CENTER   0x80
#define SPEAKER_BACK_CENTER             0x100
#define SPEAKER_SIDE_LEFT               0x200
#define SPEAKER_SIDE_RIGHT              0x400
#define SPEAKER_TOP_CENTER              0x800
#define SPEAKER_TOP_FRONT_LEFT          0x1000
#define SPEAKER_TOP_FRONT_CENTER        0x2000
#define SPEAKER_TOP_FRONT_RIGHT         0x4000
#define SPEAKER_TOP_BACK_LEFT           0x8000
#define SPEAKER_TOP_BACK_CENTER         0x10000
#define SPEAKER_TOP_BACK_RIGHT          0x20000
#endif

// Returns the mask, or 0 when the arrangement cannot be named.  0 means "leave the
// attribute out": a wrong mask is worse than no mask, because the renderer would place
// channels in the wrong speakers instead of falling back to its own rule for the
// channel count.
inline UINT32 AvsChannelMaskToKS(uint64_t ffMask, UINT32 channels)
{
    if (ffMask == 0)
    {
        // only a channel count is known; mono and stereo are unambiguous, nothing else is
        if (channels == 1)
            return (UINT32)SPEAKER_FRONT_CENTER;
        if (channels == 2)
            return (UINT32)(SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        return 0;
    }

    static const struct { int ff; UINT32 ks; } map[] = {
        { AV_CHAN_FRONT_LEFT,            (UINT32)SPEAKER_FRONT_LEFT },
        { AV_CHAN_FRONT_RIGHT,           (UINT32)SPEAKER_FRONT_RIGHT },
        { AV_CHAN_FRONT_CENTER,          (UINT32)SPEAKER_FRONT_CENTER },
        { AV_CHAN_LOW_FREQUENCY,         (UINT32)SPEAKER_LOW_FREQUENCY },
        { AV_CHAN_BACK_LEFT,             (UINT32)SPEAKER_BACK_LEFT },
        { AV_CHAN_BACK_RIGHT,            (UINT32)SPEAKER_BACK_RIGHT },
        { AV_CHAN_FRONT_LEFT_OF_CENTER,  (UINT32)SPEAKER_FRONT_LEFT_OF_CENTER },
        { AV_CHAN_FRONT_RIGHT_OF_CENTER, (UINT32)SPEAKER_FRONT_RIGHT_OF_CENTER },
        { AV_CHAN_BACK_CENTER,           (UINT32)SPEAKER_BACK_CENTER },
        { AV_CHAN_SIDE_LEFT,             (UINT32)SPEAKER_SIDE_LEFT },
        { AV_CHAN_SIDE_RIGHT,            (UINT32)SPEAKER_SIDE_RIGHT },
        { AV_CHAN_TOP_CENTER,            (UINT32)SPEAKER_TOP_CENTER },
        { AV_CHAN_TOP_FRONT_LEFT,        (UINT32)SPEAKER_TOP_FRONT_LEFT },
        { AV_CHAN_TOP_FRONT_CENTER,      (UINT32)SPEAKER_TOP_FRONT_CENTER },
        { AV_CHAN_TOP_FRONT_RIGHT,       (UINT32)SPEAKER_TOP_FRONT_RIGHT },
        { AV_CHAN_TOP_BACK_LEFT,         (UINT32)SPEAKER_TOP_BACK_LEFT },
        { AV_CHAN_TOP_BACK_CENTER,       (UINT32)SPEAKER_TOP_BACK_CENTER },
        { AV_CHAN_TOP_BACK_RIGHT,        (UINT32)SPEAKER_TOP_BACK_RIGHT },
    };

    UINT32 ks = 0;
    int    n  = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    {
        if (ffMask & (1ULL << map[i].ff))
        {
            ks |= map[i].ks;
            n++;
        }
    }

    // the mask has to describe exactly the channels the stream carries; anything else
    // (an unmapped position such as binaural or wide channels) is better left unnamed
    if (n == 0 || (channels != 0 && (UINT32)n != channels))
        return 0;
    return ks;
}

// Same, straight from FFmpeg's layout structure (NATIVE carries a mask, CUSTOM has to
// be walked, UNSPEC only says how many channels there are).
inline UINT32 AvsChannelMaskToKS(const AVChannelLayout& lay)
{
    uint64_t mask = 0;
    if (lay.order == AV_CHANNEL_ORDER_NATIVE)
        mask = lay.u.mask;
    else if (lay.order == AV_CHANNEL_ORDER_CUSTOM)
    {
        for (int i = 0; i < lay.nb_channels; i++)
            if (lay.u.map[i].id >= 0 && lay.u.map[i].id < 64)
                mask |= (1ULL << lay.u.map[i].id);
    }
    return AvsChannelMaskToKS(mask, (UINT32)lay.nb_channels);
}

// How many speakers a KS mask names; used to make sure a mask and a channel count that
// came from different places really describe the same stream.
inline int AvsCountSpeakers(UINT32 mask)
{
    int n = 0;
    while (mask != 0)
    {
        mask &= (mask - 1);
        n++;
    }
    return n;
}

#endif // AVSCHANNELMASK_H
