//----------------------------------------------------------------------------------------------
// FFmpegAudioDecoder.cpp - libavcodec wrapper used by the DRA audio decoder MFT.
//----------------------------------------------------------------------------------------------
#include "StdAfx.h"
#include "FFmpegAudioDecoder.h"

#include <algorithm>
#include <vector>

FFmpegAudioDecoder::FFmpegAudioDecoder()
    : m_ctx(NULL), m_frame(NULL), m_rate(0), m_channels(0), m_chMask(0), m_lastTime(-1)
{
}

FFmpegAudioDecoder::~FFmpegAudioDecoder()
{
    Close();
}

void FFmpegAudioDecoder::Close()
{
    if (m_frame != NULL)
    {
        av_frame_free(&m_frame);
        m_frame = NULL;
    }
    if (m_ctx != NULL)
    {
        avcodec_free_context(&m_ctx);
        m_ctx = NULL;
    }
    m_rate = m_channels = 0;
    m_chMask = 0;
    m_lastTime = -1;
}

HRESULT FFmpegAudioDecoder::Open(AVCodecID codecId)
{
    const AVCodec* pCodec = avcodec_find_decoder(codecId);
    if (pCodec == NULL)
        return MF_E_UNSUPPORTED_REPRESENTATION;

    m_ctx = avcodec_alloc_context3(pCodec);
    if (m_ctx == NULL)
        return E_OUTOFMEMORY;

    // One thread keeps the output order and the timestamps predictable inside an MFT.
    m_ctx->thread_count = 1;
    AVRational t100ns = { 1, 10000000 };
    m_ctx->pkt_timebase = t100ns;

    if (avcodec_open2(m_ctx, pCodec, NULL) < 0)
    {
        Close();
        return MF_E_INVALIDMEDIATYPE;
    }

    m_frame = av_frame_alloc();
    if (m_frame == NULL)
    {
        Close();
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

HRESULT FFmpegAudioDecoder::Decode(const BYTE* pData, UINT32 cbData,
                                  LONGLONG rtStart, LONGLONG rtStop,
                                  std::vector<Frame>& framesOut)
{
    if (m_ctx == NULL)
        return MF_E_NOT_INITIALIZED;

    AVPacket* pkt = av_packet_alloc();
    if (pkt == NULL)
        return E_OUTOFMEMORY;

    pkt->data     = const_cast<BYTE*>(pData);
    pkt->size     = (int)cbData;
    pkt->pts      = (rtStart >= 0) ? rtStart : AV_NOPTS_VALUE;
    pkt->dts      = (rtStart >= 0) ? rtStart : AV_NOPTS_VALUE;
    pkt->duration = (rtStop > rtStart) ? (long long)(rtStop - rtStart) : 0;

    int err = avcodec_send_packet(m_ctx, pkt);
    av_packet_free(&pkt);
    if (err == AVERROR(EAGAIN))
    {
        // Should not happen with a single threaded decoder, but recover by draining.
        std::vector<Frame> tmp;
        Drain(tmp);
        StampFrames(tmp, rtStart);
        framesOut.insert(framesOut.end(), tmp.begin(), tmp.end());

        AVPacket* p2 = av_packet_alloc();
        if (p2 == NULL)
            return E_OUTOFMEMORY;
        p2->data = const_cast<BYTE*>(pData);
        p2->size = (int)cbData;
        p2->pts  = p2->dts = rtStart;
        err = avcodec_send_packet(m_ctx, p2);
        av_packet_free(&p2);
    }
    if (err < 0 && err != AVERROR(EAGAIN))
        return MF_E_INVALIDMEDIATYPE;      // damaged access unit, keep the decoder alive

    std::vector<Frame> tmp;
    HRESULT hr = Drain(tmp);
    if (FAILED(hr))
        return hr;
    StampFrames(tmp, rtStart);
    framesOut.insert(framesOut.end(), tmp.begin(), tmp.end());
    return S_OK;
}

HRESULT FFmpegAudioDecoder::Flush(std::vector<Frame>& framesOut)
{
    if (m_ctx == NULL)
        return MF_E_NOT_INITIALIZED;

    avcodec_send_packet(m_ctx, NULL);

    std::vector<Frame> tmp;
    HRESULT hr = Drain(tmp);
    if (FAILED(hr))
        return hr;
    StampFrames(tmp, m_lastTime);
    framesOut.insert(framesOut.end(), tmp.begin(), tmp.end());
    return S_OK;
}

// Give the frames that just came out of libavcodec their presentation times.
//
// The audio runs on a continuous internal timeline.  A DRA access unit in a transport
// stream is spaced for four 1024 sample frames, but it does not always decode to four:
// in CCTV-4.ts the first packet gives one frame and the rest alternate between three and
// five.  Stamping every group with its packet's time stamp therefore alternates between
// overlapping and gapped output samples even though the total is right.  The packet time
// stamp is used to start the timeline and to re-anchor it when the two disagree by more
// than a few frames, which means a real discontinuity: a seek, a hole in the file, or a
// damaged stream.
void FFmpegAudioDecoder::StampFrames(std::vector<Frame>& framesOut, LONGLONG rtStart)
{
    const LONGLONG HNS = 10000000LL;

    LONGLONG frameDur = 0;
    if (m_rate > 0)
        frameDur = ((LONGLONG)1024 * HNS) / m_rate;

    if (m_lastTime < 0)
        m_lastTime = (rtStart >= 0) ? rtStart : 0;
    else if (rtStart >= 0 && frameDur > 0)
    {
        // Five audio frames (about 0.1 s at 48 kHz) is enough slack for the bit stream
        // to move frames between packets, and still far below what a seek or a real
        // break in the file looks like.
        const LONGLONG llSlack = 5 * frameDur;
        const LONGLONG drift = rtStart - m_lastTime;
        if (drift > llSlack || drift < -llSlack)
            m_lastTime = rtStart;
    }

    LONGLONG t = m_lastTime;

    for (size_t i = 0; i < framesOut.size(); i++)
    {
        Frame& f = framesOut[i];
        const int rate = f.rate > 0 ? f.rate : m_rate;
        if (rate <= 0 || f.nbSamples <= 0)
            continue;

        f.rtStart = t;
        f.rtStop  = t + ((LONGLONG)f.nbSamples * HNS) / rate;
        t = f.rtStop;
    }
    if (!framesOut.empty())
        m_lastTime = t;
}

HRESULT FFmpegAudioDecoder::Drain(std::vector<Frame>& framesOut)
{
    if (m_frame == NULL)
        return MF_E_NOT_INITIALIZED;

    for (;;)
    {
        int err = avcodec_receive_frame(m_ctx, m_frame);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
            break;
        if (err < 0)
            break;

        Frame f;
        HRESULT hr = Convert(m_frame, f);
        if (SUCCEEDED(hr))
            framesOut.push_back(f);
        av_frame_unref(m_frame);
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// Any libavcodec sample format -> interleaved 16 bit.  DRA decodes to packed
// S16 already, the other conversions are there so that a different audio codec
// can be plugged into this MFT without touching it.
// ---------------------------------------------------------------------------
static inline short FromS32(int v)
{
    if (v > 0x7FFF) return (short)0x7FFF;
    if (v < -0x8000) return (short)-0x8000;
    return (short)v;
}

static inline short FromFloat(double v)
{
    if (v > 1.0) v = 1.0;
    if (v < -1.0) v = -1.0;
    return (short)(v * 32767.0);
}

HRESULT FFmpegAudioDecoder::Convert(AVFrame* src, Frame& dst)
{
    const int channels = src->ch_layout.nb_channels;
    const int nbSamples = src->nb_samples;
    if (channels <= 0 || nbSamples <= 0)
        return MF_E_INVALIDMEDIATYPE;

    const bool planar = av_sample_fmt_is_planar((AVSampleFormat)src->format) != 0;
    const int bps = av_get_bytes_per_sample((AVSampleFormat)src->format);
    if (channels > 8 || bps <= 0 || bps > 8)
        return MF_E_INVALIDMEDIATYPE;

    dst.channels  = channels;
    dst.rate      = src->sample_rate;
    dst.nbSamples = nbSamples;
    dst.pcm.assign((size_t)channels * nbSamples * 2, 0);

    const uint8_t* p[8] = { NULL };
    for (int c = 0; c < channels && c < (int)(sizeof(p) / sizeof(p[0])); c++)
        p[c] = src->data[c];
    const int stride = src->linesize[0];
    if (p[0] == NULL)
        return MF_E_INVALIDMEDIATYPE;

    int16_t* out = (int16_t*)dst.pcm.data();

    for (int s = 0; s < nbSamples; s++)
    {
        for (int c = 0; c < channels; c++)
        {
            const uint8_t* base = planar ? p[c] : p[0];
            if (base == NULL)
                return MF_E_INVALIDMEDIATYPE;

            size_t off;
            if (planar)
                off = (size_t)s * bps;
            else
                off = ((size_t)s * channels + c) * bps;

            const uint8_t* q = base + off;
            short v = 0;
            switch ((AVSampleFormat)src->format)
            {
            case AV_SAMPLE_FMT_U8:
            case AV_SAMPLE_FMT_U8P:
                v = (short)(((int)*q - 128) * 257);
                break;
            case AV_SAMPLE_FMT_S16:
            case AV_SAMPLE_FMT_S16P:
                v = *(const int16_t*)q;
                break;
            case AV_SAMPLE_FMT_S32:
            case AV_SAMPLE_FMT_S32P:
                v = FromS32(*(const int32_t*)q);
                break;
            case AV_SAMPLE_FMT_FLT:
            case AV_SAMPLE_FMT_FLTP:
                v = FromFloat(*(const float*)q);
                break;
            case AV_SAMPLE_FMT_DBL:
            case AV_SAMPLE_FMT_DBLP:
                v = FromFloat(*(const double*)q);
                break;
            default:
                return MF_E_INVALIDMEDIATYPE;
            }
            *out++ = v;
        }
        (void)stride;
    }

    // remember what the bit stream actually said
    m_rate     = src->sample_rate;
    m_channels = channels;

    // the channel arrangement, as FFmpeg channel bits.  A NATIVE layout carries a mask;
    // a CUSTOM one has to be walked.  UNSPEC means the codec only says how many
    // channels there are, and then the MFT must not claim a mask it does not know.
    switch ((int)src->ch_layout.order)
    {
    case AV_CHANNEL_ORDER_NATIVE:
        m_chMask = src->ch_layout.u.mask;
        break;
    case AV_CHANNEL_ORDER_CUSTOM:
    {
        uint64_t mask = 0;
        for (int i = 0; i < src->ch_layout.nb_channels; i++)
        {
            const int id = src->ch_layout.u.map[i].id;
            if (id >= 0 && id < 64)
                mask |= (1ULL << id);
        }
        m_chMask = mask;
        break;
    }
    default:
        break;
    }
    return S_OK;
}
