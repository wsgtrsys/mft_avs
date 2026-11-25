//----------------------------------------------------------------------------------------------
// FFmpegDecoder.cpp - libavcodec wrapper implementation.
//----------------------------------------------------------------------------------------------
#include "StdAfx.h"
#include "FFmpegDecoder.h"

#ifndef AVS_MFT_TRACE_FFMPEG
#ifdef _DEBUG
#define AVS_MFT_TRACE_FFMPEG 1
#else
#define AVS_MFT_TRACE_FFMPEG 0
#endif
#endif

static bool FF_TraceOn(void)
{
    static int on = -1;
    if (on < 0)
        on = (GetEnvironmentVariableW(L"AVS_MFT_TRACE", nullptr, 0) > 0) ? 1 : 0;
    return on != 0;
}

static int FF_DecoderThreadCount(void)
{
    // AVS_MFT_THREADS overrides the policy; it exists for measurements (-1 forces the
    // old single threaded behaviour).
    char val[32] = { 0 };
    if (GetEnvironmentVariableA("AVS_MFT_THREADS", val, DWORD(sizeof(val))) > 0)
    {
        const int n = atoi(val);
        return (n > 0) ? n : 1;
    }

    // 0 = "the decoder decides": libdavs2 and libuavs3d then use all available cores
    // (their FFmpeg wrappers pass avctx->thread_count straight through and treat 0 as
    // "use every core").  A single threaded AVS2/AVS3 decoder cannot keep up with
    // 1080p, let alone the 4K/8K samples this project is tested against.
    //
    // The ported libcavs (AVS+) runs its own thread pool and is not affected by this.
    // libavcodec hands frames back in presentation order whichever way it parallelises
    // internally, so the time stamp pairing in Decode() stays correct.
    return 0;
}

static void FFmpegLogCallback(void* /*ptr*/, int level, const char* fmt, va_list vl)
{
#if AVS_MFT_TRACE_FFMPEG
    if (level > AV_LOG_INFO)
        return;
    char buf[1024];
    StringCchVPrintfA(buf, ARRAYSIZE(buf), fmt, vl);
    OutputDebugStringA("[avs-mft][ffmpeg] ");
    OutputDebugStringA(buf);
#else
    (void)level; (void)fmt; (void)vl;
#endif
}

static HRESULT FFErrorToHR(int err)
{
    if (err >= 0)
        return S_OK;
    switch (err)
    {
    case AVERROR(ENOMEM):
        return E_OUTOFMEMORY;
    case AVERROR(EINVAL):
    case AVERROR(ENOSYS):
        return MF_E_UNSUPPORTED_FORMAT;
    case AVERROR_INVALIDDATA:
        return MF_E_INVALIDMEDIATYPE;
    default:
        return E_UNEXPECTED;
    }
}

FFmpegVideoDecoder::FFmpegVideoDecoder()
    : m_ctx(NULL)
    , m_sws(NULL)
    , m_outFormat(AV_PIX_FMT_NONE)
    , m_srcFormat(AV_PIX_FMT_NONE)
    , m_colPrimaries(AVCOL_PRI_UNSPECIFIED)
    , m_colTransfer(AVCOL_TRC_UNSPECIFIED)
    , m_colMatrix(AVCOL_SPC_UNSPECIFIED)
    , m_colRange(AVCOL_RANGE_UNSPECIFIED)
    , m_bChromaVFirst(false)
    , m_codecId(AV_CODEC_ID_NONE)
    , m_width(0)
    , m_height(0)
    , m_reorderDepth(AVS_MFT_REORDER_DEPTH)
    , m_llLastOutTime(-1)
    , m_llLastInterval(0)
{
    av_log_set_callback(FFmpegLogCallback);
    av_log_set_level(AV_LOG_WARNING);
}

FFmpegVideoDecoder::~FFmpegVideoDecoder()
{
    Close();
}

void FFmpegVideoDecoder::Close()
{
    if (m_sws)
    {
        sws_freeContext(m_sws);
        m_sws = NULL;
    }
    if (m_ctx)
    {
        avcodec_free_context(&m_ctx);
        m_ctx = NULL;
    }
    m_width = m_height = 0;
    m_srcFormat = AV_PIX_FMT_NONE;
    m_colPrimaries = AVCOL_PRI_UNSPECIFIED;
    m_colTransfer  = AVCOL_TRC_UNSPECIFIED;
    m_colMatrix    = AVCOL_SPC_UNSPECIFIED;
    m_colRange     = AVCOL_RANGE_UNSPECIFIED;
    m_reorder.clear();
    m_timeQueue.clear();
    m_llLastOutTime = -1;
    m_llLastInterval = 0;
}

HRESULT FFmpegVideoDecoder::Open(AVCodecID codecId, const BYTE* sequenceHeader,
                                 UINT32 cbSequenceHeader, AVPixelFormat outputFormat)
{
    Close();

    m_codecId   = codecId;
    m_outFormat = outputFormat;

    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (codec == NULL)
    {
        // most likely AVS2/AVS3: those decoders are external libraries in FFmpeg
        // (libdavs2 / libxavs2 / libuavs3d) and were not compiled in.
        OutputDebugStringA("[avs-mft] avcodec_find_decoder: codec not built into this "
                           "FFmpeg (AVS2 needs libdavs2/libxavs2, AVS3 needs libuavs3d)\n");
        return MF_E_UNSUPPORTED_FORMAT;
    }

    m_ctx = avcodec_alloc_context3(codec);
    if (m_ctx == NULL)
        return E_OUTOFMEMORY;

    // Decoder threading: see FF_DecoderThreadCount().  MF calls an MFT from a single
    // thread at a time, so there is no re-entrancy concern here.
    m_ctx->thread_count = FF_DecoderThreadCount();
    if (FF_TraceOn())
        fprintf(stderr, "[avsmft] wrapper: decoder '%s' thread_count=%d\n",
                codec->name, m_ctx->thread_count);

    // The MFT feeds 100ns time stamps as packet pts, so make libavcodec agree.
    m_ctx->pkt_timebase = AVRational{ 1, 10000000 };

    if (sequenceHeader != NULL && cbSequenceHeader > 0)
    {
        m_ctx->extradata = (BYTE*)av_mallocz(cbSequenceHeader + AV_INPUT_BUFFER_PADDING_SIZE);
        if (m_ctx->extradata == NULL)
        {
            Close();
            return E_OUTOFMEMORY;
        }
        memcpy(m_ctx->extradata, sequenceHeader, cbSequenceHeader);
        m_ctx->extradata_size = (int)cbSequenceHeader;
    }

    int err = avcodec_open2(m_ctx, codec, NULL);
    if (err < 0)
    {
        OutputDebugStringA("[avs-mft] avcodec_open2 failed\n");
        Close();
        return FFErrorToHR(err);
    }
    return S_OK;
}

HRESULT FFmpegVideoDecoder::Decode(const BYTE* data, UINT32 cbData,
                                  LONGLONG rtStart, LONGLONG rtStop,
                                  std::vector<Frame>& framesOut)
{
    if (m_ctx == NULL)
        return E_FAIL;
    if (data == NULL || cbData == 0)
        return E_INVALIDARG;

    AVPacket* pkt = av_packet_alloc();
    if (pkt == NULL)
        return E_OUTOFMEMORY;

    int err = av_new_packet(pkt, (int)cbData);
    if (err < 0)
    {
        av_packet_free(&pkt);
        return FFErrorToHR(err);
    }
    memcpy(pkt->data, data, cbData);

    // pkt_timebase is 100ns, so the Media Foundation time stamps can be used as they
    // are.  libavcodec puts them back into AVFrame.best_effort_timestamp, which is
    // what makes it possible to output the pictures in presentation order.
    pkt->pts       = (rtStart >= 0) ? rtStart : AV_NOPTS_VALUE;
    pkt->dts       = (rtStart >= 0) ? rtStart : AV_NOPTS_VALUE;
    pkt->duration  = (rtStop > rtStart) ? (rtStop - rtStart) : 0;

    // Remember the time stamp of every compressed sample we feed in.  Pairing the
    // n-th picture with the n-th sample only works while one access unit really is
    // one picture; a damaged or truncated bit stream decodes fewer pictures than the
    // container has samples, so the pictures are matched with their sample by time
    // stamp instead (see NextFrame).  The time stamps are already Media Foundation
    // 100 ns values because pkt_timebase is 1/10000000.
    SampleTime st = { rtStart, rtStop, rtStart >= 0 };
    m_timeQueue.push_back(st);
    // a safety valve only: samples that never produce a picture would otherwise grow
    // the queue without bound on a badly damaged stream
    while (m_timeQueue.size() > 4096)
        m_timeQueue.erase(m_timeQueue.begin());   // samples that will never be decoded

    HRESULT hr = S_OK;
    if (avcodec_send_packet(m_ctx, pkt) < 0)
    {
        // A decoder only refuses a packet when it is badly damaged; the stream can
        // normally continue, so the packet is simply dropped.
        OutputDebugStringA("[avs-mft] avcodec_send_packet failed, packet dropped\n");
    }
    av_packet_free(&pkt);

    hr = NextFrame(framesOut);
    if (FF_TraceOn() && !framesOut.empty())
    {
        static long s_nOut = 0;
        s_nOut += (long)framesOut.size();
        fprintf(stderr, "[avsmft] wrapper: decode gave %zu picture(s), total %ld, held %zu\n",
                framesOut.size(), s_nOut, m_reorder.size());
    }
    return hr;
}

HRESULT FFmpegVideoDecoder::Flush(std::vector<Frame>& framesOut)
{
    if (m_ctx == NULL)
        return E_FAIL;

    if (avcodec_send_packet(m_ctx, NULL) < 0)
    {
        // AVERROR(EINVAL) == already flushed
    }
    HRESULT hr = NextFrame(framesOut);
    EmitReady(framesOut, true);       // the reorder buffer goes out with the stream
    if (FF_TraceOn())
        fprintf(stderr, "[avsmft] wrapper: flush gave %zu picture(s)\n", framesOut.size());
    return hr;
}

HRESULT FFmpegVideoDecoder::NextFrame(std::vector<Frame>& framesOut)
{
    if (m_ctx == NULL)
        return E_FAIL;

    AVFrame* frame = av_frame_alloc();
    if (frame == NULL)
        return E_OUTOFMEMORY;

    HRESULT hr = S_OK;
    for (;;)
    {
        int err = avcodec_receive_frame(m_ctx, frame);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
            break;
        if (err < 0)
        {
            hr = FFErrorToHR(err);
            break;
        }

        Frame f;
        HRESULT hrConvert = Convert(frame, f);
        if (SUCCEEDED(hrConvert))
        {
            // Absolute time: match the picture with the compressed sample that produced
            // it.  libavcodec reports the packet a frame came from in
            // best_effort_timestamp, and the values we fed in are already 100 ns
            // Media Foundation times, so the match is exact.  Taking the samples one by
            // one in feed order instead would compress the timeline as soon as the
            // decoder drops an access unit: a truncated MPEG-TS of this sample holds 72
            // access units but only 23 pictures, and the presentation would have ended
            // after the 23rd time stamp, i.e. one second into a three second file.
            SampleTime st = { 0, 0, false };
            LONGLONG ts = frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE)
                ts = frame->pts;

            bool bMatched = false;
            if (ts != AV_NOPTS_VALUE)
            {
                for (size_t k = 0; k < m_timeQueue.size(); k++)
                {
                    if (m_timeQueue[k].rtStart == ts)
                    {
                        st = m_timeQueue[k];
                        bMatched = true;
                        // Only this entry is removed.  The queue is in feed (i.e.
                        // decode) order, so the entries before it are not "older":
                        // with B pictures they are often the later display times, and
                        // discarding them cost the last frames of a truncated stream
                        // their real time stamps.
                        m_timeQueue.erase(m_timeQueue.begin() + k);
                        break;
                    }
                }
            }
            if (!bMatched && FF_TraceOn())
                fprintf(stderr, "[avsmft] wrapper: picture ts=%lld matched=no, queue=%zu\n",
                        (long long)ts, m_timeQueue.size());
            if (!bMatched)
            {
                // No sample with this time stamp is known: the picture must still get a
                // time that is later than everything already handed out, otherwise the
                // pipeline stalls on a sample that belongs in the past.
                //
                // The compressed access unit times handed to us by the pipeline are the
                // authoritative timeline.  libavcodec's own picture time stamps come last:
                // for a raw elementary stream the parser invents them (and a parser guess
                // is not what the sink and the clock are driven by).
                LONGLONG rtBest = -1;
                for (const SampleTime& q : m_timeQueue)
                {
                    if (q.bHasTime && q.rtStart > m_llLastOutTime &&
                        (rtBest < 0 || q.rtStart < rtBest))
                        rtBest = q.rtStart;
                }
                if (rtBest < 0 && ts != AV_NOPTS_VALUE && ts > m_llLastOutTime)
                    rtBest = ts;                       // libavcodec reports 100 ns too
                if (rtBest >= 0)
                {
                    st.rtStart = rtBest;
                    st.bHasTime = true;
                    st.rtStop = 0;
                    // drop the queue entry that was used, if any
                    for (size_t k = 0; k < m_timeQueue.size(); k++)
                    {
                        if (m_timeQueue[k].rtStart == rtBest)
                        {
                            m_timeQueue.erase(m_timeQueue.begin() + k);
                            break;
                        }
                    }
                }
                else if (!m_timeQueue.empty())
                {
                    // nothing better known: keep the feed order
                    st = m_timeQueue.front();
                    m_timeQueue.erase(m_timeQueue.begin());
                }
            }

            f.rtStart = st.rtStart;
            f.bHasTime = st.bHasTime;
            f.rtStop = st.rtStop;
            f.ptsKey = (ts != AV_NOPTS_VALUE && ts > 0) ? ts : 0;
            if (FF_TraceOn())
                fprintf(stderr, "[avsmft] picture out=%lld key=%lld decode_ts=%lld matched=%d held=%zu\n",
                        (long long)f.rtStart, (long long)f.ptsKey, (long long)ts,
                        bMatched ? 1 : 0, m_reorder.size());

            // An intra picture cannot depend on a picture that has not been decoded yet, so
            // when the decoder hands one back, every picture it had to wait for is already
            // behind us.  Anything still unemitted therefore belongs *after* nothing: it is
            // this picture.  Decode order is the decoder's own order, and it is the one thing
            // a container cannot make wrong.
            f.bClosesRun = ((frame->flags & AV_FRAME_FLAG_KEY) != 0) ||
                           (m_reorderDepth == 0) ||
                           (frame->pict_type == AV_PICTURE_TYPE_I);

            m_reorder.push_back(std::move(f));
            EmitReady(framesOut, false);
        }
        av_frame_unref(frame);
    }
    av_frame_free(&frame);
    return hr;
}

//-------------------------------------------------------------------------------------------------
// Is the buffered run still open, i.e. could a picture with an earlier place in the
// presentation order still arrive?  The only reliable answer comes from the decoder: it
// returns pictures in decode order and marks the ones nothing later can reference.
// A stream that does not reorder at all never marks any, so the depth bound decides.
//-------------------------------------------------------------------------------------------------
bool FFmpegVideoDecoder::ReorderRunOpen(const std::vector<Frame>& held)
{
    for (const Frame& f : held)
        if (f.bClosesRun)
            return false;
    return true;
}

//-------------------------------------------------------------------------------------------------
// Pictures are kept in m_reorder until it is certain that the picture with the
// smallest presentation time cannot be overtaken by a later decoded one.
void FFmpegVideoDecoder::EmitReady(std::vector<Frame>& framesOut, bool flushAll)
{
    // Sorting needs a time stamp on every held picture.  A picture for which the
    // pipeline gave no time stamp keeps the decode order instead.
    //
    // Round 19: a broadcast MPEG-TS also makes the *time stamp* key unusable, without
    // leaving the stream untimed.  video/P2016-8-14-20-20-301.ts delivers the two fields of
    // one coded picture in two PES packets whose pts are not monotonic in decode order (the
    // source hands the MFT 24.5 -> 20.5 -> 32.5 ms), so sorting on it swapped every pair of
    // pictures: the picture set was complete and every picture was bit exact, but mft[1]
    // was ref[2] and mft[2] was ref[1] (scripts/broadcast-ts-test.py only looks at frame
    // hashes, which is why it never saw this).  The order therefore comes from the decoder
    // (bClosesRun), and the container time stamps are used for the *times* only.
    bool timed = false;
    for (const Frame& f : m_reorder)
    {
        if (!f.bHasTime)
        {
            timed = false;
            break;
        }
    }
    // Sorting is only justified while the time stamps really identify the pictures.
    // For an elementary AVS stream the times the pipeline can give us are not unique
    // per picture (measured on samples/CCTV-4-short.avs: 75 compressed samples carry
    // only 24 distinct time stamps for 26 pictures), and at end of stream the decoder
    // reports a time stamp that was already used.  Sorting on such a key moves pictures
    // that the decoder already produced in presentation order - that is the rotated tail
    // pinned by 11f.  With a degenerate key the decoder's own order is kept, and the
    // strictly-increasing rule below still gives every picture its own time stamp.
    if (timed)
    {
        const char* why = nullptr;
        for (size_t i = 0; i < m_reorder.size() && !why; i++)
        {
            if (m_reorder[i].rtStart <= m_llLastOutTime)
                why = "a time stamp from the past";      // already used by an earlier picture
            else
            {
                for (size_t j = i + 1; j < m_reorder.size(); j++)
                {
                    if (m_reorder[i].rtStart == m_reorder[j].rtStart)
                    {
                        why = "two pictures with one time stamp";
                        break;
                    }
                }
            }
        }
        if (why)
        {
            timed = false;              // the key does not identify the pictures: not sortable
            if (FF_TraceOn())
                fprintf(stderr, "[avsmft] wrapper: order NOT re-sorted (%s): %zu picture(s) held, "
                                "last time %lld\n",
                        why, m_reorder.size(), m_llLastOutTime);
        }
    }
    if (timed && FF_TraceOn())
        fprintf(stderr, "[avsmft] wrapper: order re-sorted by time stamp (%zu picture(s), "
                        "all distinct and newer than %lld)\n",
                m_reorder.size(), m_llLastOutTime);
    if (timed)
    {
        // The pipeline's own time stamps decide the presentation order: they are what
        // the source (and therefore the audio stream) uses.  The time stamps that
        // libavcodec derives for AVS are re-created by its elementary stream parser
        // and can follow the bit stream's internal timeline instead, which need not be
        // the same as the container's, so they are only kept for diagnostics.
        //
        // A picture whose time stamp was NOT found among the samples the pipeline gave
        // us does not belong to the presentation timeline at all.  That is what happens
        // to the last pictures of an AVS stream: the decoder hands them back while
        // flushing and reports the time stamp of a picture that was already output, so
        // sorting on it moves a later picture in front of earlier ones.  Measured on
        // samples/CCTV-4-short.avs (26/26) and samples/CCTV-4.avs (700/700): the
        // decoder's hand-back order is the reference order, and only this sort rotated
        // the end.  Such a picture therefore keeps the order the decoder produced it
        // in; the strictly-increasing rule below still gives it a real time stamp.
        std::stable_sort(m_reorder.begin(), m_reorder.end(),
                         [](const Frame& a, const Frame& b) { return a.rtStart < b.rtStart; });
    }

    // Emit the run the decoder closed by handing back a picture nothing later can
    // reference, and keep holding the rest (the strictly-increasing rule below still gives
    // every picture its own time stamp, so a repaired order cannot collide on the clock).
    while (!m_reorder.empty() &&
           (flushAll || (m_reorder.size() > m_reorderDepth && !ReorderRunOpen(m_reorder))))
    {
        Frame f = std::move(m_reorder.front());
        m_reorder.erase(m_reorder.begin());

        // The pipeline needs strictly increasing presentation times.  Two pictures
        // carrying the same time stamp (a truncated bit stream can end that way) would
        // otherwise be scheduled at the same instant and the clock stops there.
        if (f.bHasTime && f.rtStart <= m_llLastOutTime)
        {
            LONGLONG step = (m_llLastInterval > 0) ? m_llLastInterval : 0;
            f.rtStart = m_llLastOutTime + step;
            if (f.rtStart <= m_llLastOutTime)
                f.rtStart = m_llLastOutTime + 1;      // no frame rate known
            f.rtStop = 0;
        }
        if (f.bHasTime)
        {
            if (m_llLastOutTime >= 0 && f.rtStart - m_llLastOutTime < 10 * 10000000)
                m_llLastInterval = f.rtStart - m_llLastOutTime;
            m_llLastOutTime = f.rtStart;
        }

        // the next picture in presentation order is the end of this one
        if (!m_reorder.empty() && m_reorder.front().rtStart > f.rtStart)
            f.rtStop = m_reorder.front().rtStart;

        framesOut.push_back(std::move(f));
    }
}

//---------------------------------------------------------------------------------------------
// The colour metadata the bit stream carries.  libavcodec puts it on every decoded frame
// when the sequence header declares it (the AVS3 wrapper does; the ported libcavs
// decoder does not), and a stream does not contradict itself, so remembering the last
// stated value is enough.  Reserved values (0) are not information.
//---------------------------------------------------------------------------------------------
void FFmpegVideoDecoder::NoteColor(const AVFrame* src)
{
    m_srcFormat = (AVPixelFormat)src->format;

    if (src->color_primaries > 0 && src->color_primaries != AVCOL_PRI_UNSPECIFIED)
        m_colPrimaries = src->color_primaries;
    if (src->color_trc > 0 && src->color_trc != AVCOL_TRC_UNSPECIFIED)
        m_colTransfer = src->color_trc;
    if (src->colorspace > 0 && src->colorspace != AVCOL_SPC_UNSPECIFIED)
        m_colMatrix = src->colorspace;
    if (src->color_range > 0 && src->color_range != AVCOL_RANGE_UNSPECIFIED)
        m_colRange = src->color_range;
}

HRESULT FFmpegVideoDecoder::Convert(AVFrame* src, Frame& dst)
{
    const UINT32 w = (UINT32)src->width;
    const UINT32 h = (UINT32)src->height;
    if (w == 0 || h == 0)
        return MF_E_INVALIDMEDIATYPE;

    NoteColor(src);

    // A decoder may change the picture size mid stream (AVS sequence header change).
    const AVPixelFormat outFmt = m_outFormat;
    // P010 carries each sample in 16 bits, and Media Foundation wants the stride of
    // such a format aligned to 16 bytes; the 8 bit formats are aligned to 4.
    const bool  bInterleaved = (outFmt == AV_PIX_FMT_NV12 || outFmt == AV_PIX_FMT_P010);
    const int   bps   = (outFmt == AV_PIX_FMT_P010) ? 2 : 1;
    const UINT32 stride = (bps == 2) ? ((w * 2 + 15) & ~15u) : ((w + 3) & ~3u);

    dst.width  = w;
    dst.height = h;
    dst.stride = stride;
    m_width    = w;
    m_height   = h;

    UINT32 chromaHeight = (h + 1) / 2;
    // Interleaved formats (NV12, P010) carry one chroma plane with the luma byte pitch.
    // The planar formats carry two half-width planes, and their pitch is the half width
    // aligned - not the luma pitch: consumers read the chroma stride as (width + 1) / 2
    // (rounded the same way), and MF_MT_SAMPLE_SIZE has to describe the buffer that is
    // really handed out.  Padding the chroma rows to the luma pitch makes a picture
    // whose luma pitch is not twice its chroma pitch show up with wrong colours.
    UINT32 chromaStride = bInterleaved ? stride : (((w + 1) / 2 + 3) & ~3u);

    size_t cb = 0;
    switch (outFmt)
    {
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_P010:
        cb = (size_t)stride * h + (size_t)stride * chromaHeight;
        break;
    case AV_PIX_FMT_YUV420P:
        cb = (size_t)stride * h + (size_t)chromaStride * chromaHeight * 2;
        break;
    default:
        return MF_E_UNSUPPORTED_FORMAT;
    }

    dst.pixels.resize(cb);
    BYTE* ydst = dst.pixels.data();
    BYTE* c0dst = ydst + (size_t)stride * h;

    // The two chroma planes are interchangeable as far as sizes go, but not as far as
    // content goes: MF's YV12 wants Cr in the second plane and Cb in the third, while
    // the decoders all hand over Cb first (that is what I420 means).
    const size_t chromaPlaneSize = (size_t)chromaStride * chromaHeight;
    BYTE* cbDst = m_bChromaVFirst ? (c0dst + chromaPlaneSize) : c0dst;
    BYTE* crDst = m_bChromaVFirst ? c0dst : (c0dst + chromaPlaneSize);

    // how many bytes of real picture data one row holds, in each plane
    const UINT32 rowY  = w * (UINT32)bps;
    const UINT32 rowUV = bInterleaved ? w * (UINT32)bps : ((w + 1) / 2);

    if (src->format == outFmt)
    {
        // straight plane copy
        for (UINT32 y = 0; y < h; y++)
            memcpy(ydst + (size_t)stride * y, src->data[0] + (size_t)src->linesize[0] * y, rowY);
        if (bInterleaved)
        {
            for (UINT32 y = 0; y < chromaHeight; y++)
                memcpy(c0dst + (size_t)stride * y, src->data[1] + (size_t)src->linesize[1] * y, rowUV);
        }
        else
        {
            for (UINT32 y = 0; y < chromaHeight; y++)
                memcpy(cbDst + (size_t)chromaStride * y, src->data[1] + (size_t)src->linesize[1] * y, rowUV);
            for (UINT32 y = 0; y < chromaHeight; y++)
                memcpy(crDst + (size_t)chromaStride * y,
                       src->data[2] + (size_t)src->linesize[2] * y, rowUV);
        }
        return S_OK;
    }

    if (src->format == AV_PIX_FMT_YUV420P && outFmt == AV_PIX_FMT_NV12)
    {
        // the normal path: AVS+/AVS2/AVS3 decoders all emit yuv420p
        for (UINT32 y = 0; y < h; y++)
            memcpy(ydst + (size_t)stride * y, src->data[0] + (size_t)src->linesize[0] * y, w);
        const BYTE* up = src->data[1];
        const BYTE* vp = src->data[2];
        int su = src->linesize[1];
        int sv = src->linesize[2];
        for (UINT32 y = 0; y < chromaHeight; y++)
        {
            BYTE* d = c0dst + (size_t)stride * y;
            for (UINT32 x = 0; x < (w + 1) / 2; x++)
            {
                d[2 * x]     = up[x];
                d[2 * x + 1] = vp[x];
            }
            up += su;
            vp += sv;
        }
        return S_OK;
    }

    // anything else: let swscale do the work
    m_sws = sws_getCachedContext(m_sws, (int)w, (int)h, (AVPixelFormat)src->format,
                                 (int)w, (int)h, outFmt, SWS_BILINEAR, NULL, NULL, NULL);
    if (m_sws == NULL)
        return E_OUTOFMEMORY;

    const uint8_t* srcSlice[4] = { src->data[0], src->data[1], src->data[2], NULL };
    int srcStride[4] = { src->linesize[0], src->linesize[1], src->linesize[2], 0 };
    uint8_t* dstSlice[4] = { NULL, NULL, NULL, NULL };
    int dstStride[4] = { 0, 0, 0, 0 };
    dstSlice[0] = ydst;
    dstStride[0] = (int)stride;
    if (bInterleaved)
    {
        dstSlice[1] = c0dst;
        dstStride[1] = (int)stride;
    }
    else
    {
        dstSlice[1] = cbDst;
        dstStride[1] = (int)chromaStride;
        dstSlice[2] = crDst;
        dstStride[2] = (int)chromaStride;
    }

    int scaled = sws_scale(m_sws, srcSlice, srcStride, 0, (int)h, dstSlice, dstStride);
    if (scaled <= 0)
        return E_UNEXPECTED;

    // remember the size of the pictures that are really coming out, the MFT asks for
    // it when the bit stream changes the picture size
    m_width  = w;
    m_height = h;
    return S_OK;
}
