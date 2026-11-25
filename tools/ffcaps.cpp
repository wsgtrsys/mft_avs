// tools/ffcaps.cpp - what our own FFmpeg build actually contains, asked at runtime instead of
// grepping a config file that the build tree does not keep.
//
// The question came up because video/dfws.ts carries E-AC-3 audio: our media source has to decide
// whether to describe that stream for a system decoder (this machine has none registered) or for our
// own audio MFT, and that depends on whether ff91-install even contains an E-AC-3 decoder.
#include <stdio.h>
// FFmpeg's public headers are C headers without extern "C" guards; every C++ tool in this tree
// wraps them (see tools/avsmft_test.cpp), and without the wrapper the linker looks for C++-mangled
// avcodec_version and fails.
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

int main(void)
{
    printf("libavcodec version %d\n", avcodec_version());
    printf("configured: %s\n", avcodec_configuration());

    static const char* names[] = {
        "cavs", "libcavs", "libdavs2", "libuavs3d", "dra", "ac3", "eac3", "aac", "mp3", NULL
    };
    for (int i = 0; names[i]; i++)
    {
        const AVCodec* d = avcodec_find_decoder_by_name(names[i]);
        printf("  decoder %-10s %s\n", names[i], d ? "PRESENT" : "absent");
    }

    // the MFT asks by codec id, not by name, so report that route too
    struct IdCase { const char* what; enum AVCodecID id; };
    static const IdCase ids[] = {
        { "AV_CODEC_ID_CAVS", AV_CODEC_ID_CAVS },
        { "AV_CODEC_ID_AVS2", AV_CODEC_ID_AVS2 },
        { "AV_CODEC_ID_AVS3", AV_CODEC_ID_AVS3 },
        { "AV_CODEC_ID_DRA",  AV_CODEC_ID_DRA  },
        { "AV_CODEC_ID_AC3",  AV_CODEC_ID_AC3  },
        { "AV_CODEC_ID_EAC3", AV_CODEC_ID_EAC3 },
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
    {
        const AVCodec* d = avcodec_find_decoder(ids[i].id);
        printf("  find_decoder(%-16s) -> %s\n", ids[i].what, d ? d->name : "none");
    }

    // Which containers this build can even open.  The media source falls back to the raw CAVS
    // demuxer when nothing else matches, and that fallback hid the real reason a plain MP4 was
    // described as video/AVS 28x8192: with no mov demuxer compiled in, the content probe can only
    // answer "nothing" (measured: probe over 131072 byte(s): (none) (score 0)).
    printf("demuxers compiled into this build:\n");
    void* it = NULL;
    const AVInputFormat* f = NULL;
    int nFormats = 0;
    while ((f = av_demuxer_iterate(&it)) != NULL)
        nFormats++;
    printf("  count: %d\n", nFormats);
    static const char* want[] = { "mov", "mpegts", "mpeg", "cavsvideo", "avs2", "avs3",
                                  "h264", "hevc", "aac", "mp3", NULL };
    for (int i = 0; want[i]; i++)
        printf("  demuxer %-10s %s\n", want[i], av_find_input_format(want[i]) ? "PRESENT" : "absent");
    return 0;
}
