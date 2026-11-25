//----------------------------------------------------------------------------------------------
// FFmpegAudioDecoder.h - thin C++ wrapper around libavcodec for the DRA audio stream.
//
// Same shape as FFmpegVideoDecoder: Open()/Decode()/Flush(), samples converted to
// interleaved 16 bit PCM, which is what MFAudioFormat_PCM means.
//
// DRA (AVS audio, GB/T 20090.6) is the audio codec that accompanies AVS+ video in
// Chinese broadcast streams (CCTV-4.ts here).  The bit stream carries its own
// sample rate and channel count, and a container may leave both unknown, so the
// rate/channels of the first decoded frame are remembered and can be queried -
// the MFT uses that to fix up its output media type when the input type did not
// say anything sensible.
//----------------------------------------------------------------------------------------------
#ifndef FFMPEGAUDIODECODER_H
#define FFMPEGAUDIODECODER_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
}

class FFmpegAudioDecoder
{
public:

    struct Frame
    {
        Frame() : channels(0), rate(0), nbSamples(0), rtStart(0), rtStop(0) {}

        std::vector<BYTE> pcm;      // interleaved 16 bit samples
        int     channels;
        int     rate;
        int     nbSamples;          // per channel
        LONGLONG rtStart;           // 100ns, from the input sample time
        LONGLONG rtStop;
    };

    FFmpegAudioDecoder();
    ~FFmpegAudioDecoder();

    HRESULT Open(AVCodecID codecId);
    void    Close();
    bool    IsOpen() const { return m_ctx != NULL; }

    // One compressed access unit in, zero or more PCM frames out.
    HRESULT Decode(const BYTE* pData, UINT32 cbData, LONGLONG rtStart, LONGLONG rtStop,
                   std::vector<Frame>& framesOut);
    HRESULT Flush(std::vector<Frame>& framesOut);

    int     Channels() const   { return m_channels; }
    int     SampleRate() const { return m_rate; }

    // The channel arrangement the bit stream declares, as FFmpeg channel bits
    // (AV_CH_FRONT_LEFT ...); 0 means the stream only gives a channel count.  The MFT
    // turns this into the KSAUDIO_CHANNEL_MASK that MF_MT_AUDIO_CHANNEL_MASK wants.
    uint64_t ChannelMaskFF() const { return m_chMask; }

private:
    HRESULT Drain(std::vector<Frame>& framesOut);
    HRESULT Convert(AVFrame* src, Frame& dst);

    // The decoded audio runs on one continuous timeline (m_lastTime, -1 = not started
    // yet).  A compressed sample only re-anchors that timeline when its own time stamp
    // disagrees with where the timeline currently is by more than a couple of audio
    // frames, which is how a seek or a hole in the file is handled.  Per packet
    // stamping does not work: the DRA bit stream does not deliver a fixed number of
    // audio frames per TS packet (see the comment in the .cpp).
    void    StampFrames(std::vector<Frame>& framesOut, LONGLONG rtStart);

    AVCodecContext* m_ctx;
    AVFrame*        m_frame;
    int             m_rate;
    int             m_channels;
    uint64_t        m_chMask;
    LONGLONG        m_lastTime;
};

#endif // FFMPEGAUDIODECODER_H
