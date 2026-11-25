//----------------------------------------------------------------------------------------------
// FFmpegDecoder.h - thin C++ wrapper around libavcodec used by the AVS MFT decoder.
//
// One instance == one video decoder instance:
//   Open()      avcodec_find_decoder + avcodec_alloc_context3 + avcodec_open2
//   Decode()    avcodec_send_packet / avcodec_receive_frame loop
//   Flush()     avcodec_send_packet(NULL) to pull the delayed frames out
//
// Frames are converted to the requested packed YUV layout (NV12 / I420 / YV12) so
// that the MFT can hand out IMFVideoSampleExtensions the EVR / D3D presenters accept.
//
// Output order: libavcodec hands pictures over in decode order (an AVS P16 stream
// codes B pictures after the P picture that follows them), while Media Foundation
// expects a decoder's output samples in presentation order.  The timestamps of the
// compressed samples are therefore passed to the packet (pkt_timebase is 100ns) and
// the pictures are held in a small reorder buffer, smallest presentation time first.
//
// Codec coverage (see also scripts/port_avs_to_ffmpeg.py):
//   AV_CODEC_ID_CAVS  AVS1-P2 / AVS1-P16 (AVS+) - the libcavs decoder that was
//                     ported out of FFmpeg-AVSPlus (AVS Guangdian + Jizhun profile)
//   AV_CODEC_ID_AVS2  only when FFmpeg was configured with --enable-libdavs2 or
//                     --enable-libxavs2
//   AV_CODEC_ID_AVS3  only when FFmpeg was configured with --enable-libuavs3d
//----------------------------------------------------------------------------------------------
#ifndef FFMPEGDECODER_H
#define FFMPEGDECODER_H

// pictures held back to fix the decode/display order (AVS P16 with B pictures needs 2)
#ifndef AVS_MFT_REORDER_DEPTH
#define AVS_MFT_REORDER_DEPTH 4
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

class FFmpegVideoDecoder
{
public:

    struct Frame
    {
        Frame() : stride(0), width(0), height(0), rtStart(0), rtStop(0), ptsKey(0),
                    bHasTime(false), bClosesRun(false) {}

        std::vector<BYTE> pixels;   // one packed image in the requested output format
        UINT32  stride;             // row stride of the Y plane, in bytes
        UINT32  width;
        UINT32  height;
        LONGLONG rtStart;           // 100ns, taken from the input sample time
        LONGLONG rtStop;
        LONGLONG ptsKey;            // what libavcodec reported; used only for diagnostics
        bool     bHasTime;          // false when the pipeline gave no time stamp (-1)
        // False for a picture the decoder cannot reference a later picture from (an
        // intra picture / a stream without reordering).  Everything before such a
        // picture in decode order is already in our buffer, so the buffer can be closed
        // at it.  This is the only order key that the container cannot corrupt.
        bool     bClosesRun;
    };

    FFmpegVideoDecoder();
    ~FFmpegVideoDecoder();

    // sequenceHeader may be NULL: for AVS+/AVS the sequence header is carried in band.
    HRESULT Open(AVCodecID codecId, const BYTE* sequenceHeader, UINT32 cbSequenceHeader,
                 AVPixelFormat outputFormat);
    void   Close();

    // rtStart / rtStop are the Media Foundation time stamps of the compressed sample
    // (100ns units); they are put on the packet so that libavcodec can put them back
    // on the decoded pictures.
    HRESULT Decode(const BYTE* data, UINT32 cbData, LONGLONG rtStart, LONGLONG rtStop,
                   std::vector<Frame>& framesOut);
    HRESULT Flush(std::vector<Frame>& framesOut);

    // How many pictures may be held back to repair the decode/display order.  AVS P2
    // with B pictures needs 2, the default leaves some margin for damaged streams.
    void   SetReorderDepth(UINT32 n) { m_reorderDepth = n; }

    // Media Foundation's YV12 stores Cr before Cb, which is the opposite of what the
    // planar output of every AVS decoder looks like; the wrapper has to be told.
    void SetChromaVFirst(bool b) { m_bChromaVFirst = b; }

    UINT32       Width()  const { return m_width; }
    UINT32       Height() const { return m_height; }
    bool         IsOpen() const { return m_ctx != NULL; }
    AVPixelFormat OutputFormat() const { return m_outFormat; }

    // the pixel format the decoder is really producing (yuv420p, yuv420p10le, ...),
    // which is what tells the MFT that a stream is 10 bit
    AVPixelFormat SourceFormat() const { return m_srcFormat; }

    // What the bit stream says about the sample values.  Everything UNSPECIFIED means
    // "not stated", and the MFT then leaves the matching MF attribute out; guessing a
    // colour space is how a BT.2020 stream ends up looking washed out on screen.
    struct Color
    {
        Color() : primaries(AVCOL_PRI_UNSPECIFIED), transfer(AVCOL_TRC_UNSPECIFIED),
                  matrix(AVCOL_SPC_UNSPECIFIED), range(AVCOL_RANGE_UNSPECIFIED) {}

        int primaries;      // AVCOL_PRI_*
        int transfer;       // AVCOL_TRC_*
        int matrix;         // AVCOL_SPC_*
        int range;          // AVCOL_RANGE_*
    };
    Color ColorInfo() const
    {
        Color c;
        c.primaries = m_colPrimaries;
        c.transfer  = m_colTransfer;
        c.matrix    = m_colMatrix;
        c.range     = m_colRange;
        return c;
    }

private:
    HRESULT Convert(AVFrame* src, Frame& dst);
    HRESULT NextFrame(std::vector<Frame>& framesOut);
    void    EmitReady(std::vector<Frame>& framesOut, bool flushAll);
    bool    ReorderRunOpen(const std::vector<Frame>& held);
    void    NoteColor(const AVFrame* src);

    AVCodecContext* m_ctx;
    SwsContext*     m_sws;
    AVPixelFormat   m_outFormat;
    AVPixelFormat   m_srcFormat;
    int             m_colPrimaries;
    int             m_colTransfer;
    int             m_colMatrix;
    int             m_colRange;
    bool            m_bChromaVFirst;
    AVCodecID       m_codecId;
    UINT32          m_width;
    UINT32          m_height;
    UINT32          m_reorderDepth;

    std::vector<Frame> m_reorder;         // held pictures, presentation order

    struct SampleTime { LONGLONG rtStart; LONGLONG rtStop; bool bHasTime; };
    std::vector<SampleTime> m_timeQueue;  // input samples in feed order

    // The newest presentation time that has left the decoder, and the last normal
    // picture interval.  A picture whose time stamp cannot be matched must not be
    // given an older time: the Media Foundation pipeline needs strictly increasing
    // sample times, and one early sample stalls the presentation clock.
    LONGLONG m_llLastOutTime;
    LONGLONG m_llLastInterval;
};

#endif // FFMPEGDECODER_H
